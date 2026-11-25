// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/ion for the Quest 2's Android userspace (Meta's tracker), on a kernel without ION.
 *
 * Speaks the ION ABI of Meta's 4.19 kernel (version 2: ION_IOC_ALLOC and ION_IOC_HEAP_QUERY, plus
 * Qualcomm's PREFETCH/DRAIN) and hands out dma-bufs backed by ordinary pages for every heap id. Buffers
 * are cached unless ION_FLAG_CACHED is clear; CPU access is kept coherent through the dma-buf
 * begin/end_cpu_access calls. Secure (content-protected) allocations are refused.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>

#define ION_FLAG_CACHED		1
#define ION_FLAG_SECURE		(1U << 31)
#define ION_FLAGS_CP_MASK	0x6FFE0000

struct ion_allocation_data {
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

struct ion_heap_data {
	char name[32];
	__u32 type;
	__u32 heap_id;
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;
};

struct ion_heap_query {
	__u32 cnt;
	__u32 reserved0;
	__u64 heaps;
	__u32 reserved1;
	__u32 reserved2;
};

struct ion_prefetch_regions {
	__u64 sizes;
	__u32 vmid;
	__u32 nr_sizes;
};

struct ion_prefetch_data {
	__u64 len;
	__u64 regions;
	__u32 heap_id;
	__u32 nr_regions;
};

#define ION_IOC_ALLOC		_IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_HEAP_QUERY	_IOWR('I', 8, struct ion_heap_query)
#define ION_IOC_PREFETCH	_IOWR('M', 3, struct ion_prefetch_data)
#define ION_IOC_DRAIN		_IOWR('M', 4, struct ion_prefetch_data)

/* The heaps Quest OS's kernel lists: name, type, id (msm_ion.h). All are served from system memory here. */
static const struct ion_heap_data q2_heaps[] = {
	{ "system", 0, 25 },
	{ "qsecom", 4, 27 },
	{ "adsp", 4, 22 },
	{ "user_contig", 0, 26 },
	{ "audio", 4, 28 },
	{ "camera", 4, 20 },
};

struct q2ion_buffer {
	size_t size;
	bool cached;
	struct page **pages;
	unsigned long nr_pages;
	struct sg_table sgt;
	struct mutex lock;
	struct list_head maps;
	void *vaddr;
	int vmap_count;
};

/*
 * A device's mapping of the buffer. It stays until the buffer is freed, as downstream ION's delayed unmap does: fastrpc
 * attaches, maps, unmaps and detaches a buffer on every DSP call, and mapping a pool of several MB each time (IOMMU
 * page tables and a cache flush of the whole buffer) cost up to 4 ms per call.
 */
struct q2ion_map {
	struct device *dev;
	struct sg_table sgt;
	struct list_head node;
};

static struct sg_table *q2ion_map(struct dma_buf_attachment *a, enum dma_data_direction dir)
{
	struct q2ion_buffer *b = a->dmabuf->priv;
	struct q2ion_map *m;
	int ret;

	mutex_lock(&b->lock);
	list_for_each_entry(m, &b->maps, node) {
		if (m->dev == a->dev)
			goto out;
	}
	ret = -ENOMEM;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		goto err;
	ret = sg_alloc_table_from_pages(&m->sgt, b->pages, b->nr_pages, 0, b->size, GFP_KERNEL);
	if (ret)
		goto err_free;
	ret = dma_map_sgtable(a->dev, &m->sgt, DMA_BIDIRECTIONAL, b->cached ? 0 : DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto err_table;
	m->dev = get_device(a->dev);
	list_add(&m->node, &b->maps);
out:
	mutex_unlock(&b->lock);
	return &m->sgt;

err_table:
	sg_free_table(&m->sgt);
err_free:
	kfree(m);
err:
	mutex_unlock(&b->lock);
	return ERR_PTR(ret);
}

static void q2ion_unmap(struct dma_buf_attachment *a, struct sg_table *t, enum dma_data_direction dir)
{
}

/* Clean (for a device) or invalidate (for the CPU) the cache lines of len bytes at off; the pages are not contiguous. */
static void q2ion_sync_pages(struct q2ion_buffer *b, size_t off, size_t len, bool for_device)
{
	while (len) {
		size_t po = off & ~PAGE_MASK, n = min_t(size_t, len, PAGE_SIZE - po);
		unsigned long va = (unsigned long)page_address(b->pages[off >> PAGE_SHIFT]) + po;

		if (for_device)
			dcache_clean_poc(va, va + n);
		else
			dcache_inval_poc(va, va + n);
		off += n;
		len -= n;
	}
}

/*
 * The whole buffer, once (the caches are indexed by physical address: one pass covers every device), when a device
 * has it mapped.
 */
static int q2ion_cpu_access(struct dma_buf *d, enum dma_data_direction dir, bool begin)
{
	struct q2ion_buffer *b = d->priv;
	bool mapped;

	if (!b->cached)
		return 0;
	mutex_lock(&b->lock);
	mapped = !list_empty(&b->maps);
	mutex_unlock(&b->lock);
	if (mapped && !(begin && dir == DMA_TO_DEVICE))
		q2ion_sync_pages(b, 0, b->size, !begin);
	return 0;
}

static int q2ion_begin_cpu(struct dma_buf *d, enum dma_data_direction dir)
{
	return q2ion_cpu_access(d, dir, true);
}

static int q2ion_end_cpu(struct dma_buf *d, enum dma_data_direction dir)
{
	return q2ion_cpu_access(d, dir, false);
}

static int q2ion_mmap(struct dma_buf *d, struct vm_area_struct *vma)
{
	struct q2ion_buffer *b = d->priv;
	unsigned long first = vma->vm_pgoff, n = vma_pages(vma);

	if (first + n > b->nr_pages)
		return -EINVAL;
	if (!b->cached)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return vm_map_pages_zero(vma, b->pages + first, n);
}

static int q2ion_vmap(struct dma_buf *d, struct iosys_map *map)
{
	struct q2ion_buffer *b = d->priv;
	int ret = 0;

	mutex_lock(&b->lock);
	if (!b->vmap_count) {
		b->vaddr = vmap(b->pages, b->nr_pages, VM_MAP,
				b->cached ? PAGE_KERNEL : pgprot_writecombine(PAGE_KERNEL));
		if (!b->vaddr)
			ret = -ENOMEM;
	}
	if (!ret) {
		b->vmap_count++;
		iosys_map_set_vaddr(map, b->vaddr);
	}
	mutex_unlock(&b->lock);
	return ret;
}

static void q2ion_vunmap(struct dma_buf *d, struct iosys_map *map)
{
	struct q2ion_buffer *b = d->priv;

	mutex_lock(&b->lock);
	if (!--b->vmap_count) {
		vunmap(b->vaddr);
		b->vaddr = NULL;
	}
	mutex_unlock(&b->lock);
}

static void q2ion_release(struct dma_buf *d)
{
	struct q2ion_buffer *b = d->priv;
	struct q2ion_map *m, *tmp;
	unsigned long i;

	list_for_each_entry_safe(m, tmp, &b->maps, node) {
		dma_unmap_sgtable(m->dev, &m->sgt, DMA_BIDIRECTIONAL, DMA_ATTR_SKIP_CPU_SYNC);
		sg_free_table(&m->sgt);
		put_device(m->dev);
		kfree(m);
	}
	for (i = 0; i < b->nr_pages; i++)
		__free_page(b->pages[i]);
	kvfree(b->pages);
	kfree(b);
}

static const struct dma_buf_ops q2ion_ops = {
	.map_dma_buf = q2ion_map,
	.unmap_dma_buf = q2ion_unmap,
	.begin_cpu_access = q2ion_begin_cpu,
	.end_cpu_access = q2ion_end_cpu,
	.mmap = q2ion_mmap,
	.vmap = q2ion_vmap,
	.vunmap = q2ion_vunmap,
	.release = q2ion_release,
};

/* A buffer as an ION allocation gives it; also the kernel's ion_alloc for the camera drivers. */
struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask, unsigned int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct q2ion_buffer *b;
	struct dma_buf *d = ERR_PTR(-ENOMEM);
	unsigned long i = 0;

	if (!len || !heap_id_mask)
		return ERR_PTR(-EINVAL);
	if (flags & (ION_FLAG_SECURE | ION_FLAGS_CP_MASK))
		return ERR_PTR(-EOPNOTSUPP);
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return ERR_PTR(-ENOMEM);
	b->size = PAGE_ALIGN(len);
	b->nr_pages = b->size >> PAGE_SHIFT;
	b->cached = flags & ION_FLAG_CACHED;
	mutex_init(&b->lock);
	INIT_LIST_HEAD(&b->maps);
	b->pages = kvmalloc_array(b->nr_pages, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages)
		goto err_buf;
	for (i = 0; i < b->nr_pages; i++) {
		b->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[i])
			goto err_pages;
	}
	if (!b->cached) {
		/* Zeroed through the cache: clean it to memory before the buffer is used uncached. */
		for (i = 0; i < b->nr_pages; i++) {
			unsigned long va = (unsigned long)page_address(b->pages[i]);

			dcache_clean_poc(va, va + PAGE_SIZE);
		}
	}
	info.exp_name = "ion";
	info.ops = &q2ion_ops;
	info.size = b->size;
	info.flags = O_RDWR;
	info.priv = b;
	d = dma_buf_export(&info);
	if (!IS_ERR(d))
		return d;

err_pages:
	i = min(i, b->nr_pages);
	while (i--)
		__free_page(b->pages[i]);
	kvfree(b->pages);
err_buf:
	kfree(b);
	return d;
}
EXPORT_SYMBOL_GPL(ion_alloc);

/* The ION flags of a buffer from ion_alloc (the downstream dma_buf_get_flags). */
int dma_buf_get_flags(struct dma_buf *dmabuf, unsigned long *flags)
{
	if (dmabuf->ops != &q2ion_ops)
		return -EINVAL;
	*flags = ((struct q2ion_buffer *)dmabuf->priv)->cached ? ION_FLAG_CACHED : 0;
	return 0;
}
EXPORT_SYMBOL_GPL(dma_buf_get_flags);

/*
 * Part of a buffer from ion_alloc (the downstream partial CPU access, used by fastrpc for an argument that is a
 * slice of a larger buffer): -EINVAL for other dma-bufs.
 */
int q2ion_sync_range(struct dma_buf *d, size_t off, size_t len, bool for_device)
{
	struct q2ion_buffer *b;

	if (d->ops != &q2ion_ops)
		return -EINVAL;
	b = d->priv;
	if (!b->cached || off >= b->size)
		return 0;
	q2ion_sync_pages(b, off, min(len, b->size - off), for_device);
	return 0;
}
EXPORT_SYMBOL_GPL(q2ion_sync_range);

static int q2ion_alloc(struct ion_allocation_data *req)
{
	struct dma_buf *d = ion_alloc(req->len, req->heap_id_mask, req->flags);
	int fd;

	if (IS_ERR(d))
		return PTR_ERR(d);
	fd = dma_buf_fd(d, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(d);
		return fd;
	}
	req->fd = fd;
	return 0;
}

static long q2ion_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *u = (void __user *)arg;

	switch (cmd) {
	case ION_IOC_ALLOC: {
		struct ion_allocation_data d;
		int ret;

		if (copy_from_user(&d, u, sizeof(d)))
			return -EFAULT;
		ret = q2ion_alloc(&d);
		if (ret)
			return ret;
		if (copy_to_user(u, &d, sizeof(d)))
			return -EFAULT;
		return 0;
	}
	case ION_IOC_HEAP_QUERY: {
		struct ion_heap_query q;
		u32 n = ARRAY_SIZE(q2_heaps);

		if (copy_from_user(&q, u, sizeof(q)))
			return -EFAULT;
		if (q.heaps) {
			n = min(n, q.cnt);
			if (copy_to_user(u64_to_user_ptr(q.heaps), q2_heaps, n * sizeof(q2_heaps[0])))
				return -EFAULT;
		}
		q.cnt = n;
		if (copy_to_user(u, &q, sizeof(q)))
			return -EFAULT;
		return 0;
	}
	case ION_IOC_PREFETCH:
	case ION_IOC_DRAIN:
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations q2ion_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = q2ion_ioctl,
	.compat_ioctl = q2ion_ioctl,
};

static struct miscdevice q2ion_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &q2ion_fops,
};

static int __init q2ion_init(void)
{
	return misc_register(&q2ion_dev);
}
device_initcall(q2ion_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ION ABI (Quest 2 Android userspace) on page-backed dma-bufs");
