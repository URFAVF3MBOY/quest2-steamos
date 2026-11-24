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
	void *vaddr;
	int vmap_count;
};

static struct sg_table *q2ion_map(struct dma_buf_attachment *a, enum dma_data_direction dir)
{
	struct q2ion_buffer *b = a->dmabuf->priv;
	struct sg_table *t = kzalloc(sizeof(*t), GFP_KERNEL);
	int ret;

	if (!t)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table_from_pages(t, b->pages, b->nr_pages, 0, b->size, GFP_KERNEL);
	if (ret)
		goto err_free;
	ret = dma_map_sgtable(a->dev, t, dir, b->cached ? 0 : DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto err_table;
	return t;

err_table:
	sg_free_table(t);
err_free:
	kfree(t);
	return ERR_PTR(ret);
}

static void q2ion_unmap(struct dma_buf_attachment *a, struct sg_table *t, enum dma_data_direction dir)
{
	struct q2ion_buffer *b = a->dmabuf->priv;

	dma_unmap_sgtable(a->dev, t, dir, b->cached ? 0 : DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(t);
	kfree(t);
}

static int q2ion_cpu_access(struct dma_buf *d, enum dma_data_direction dir, bool begin)
{
	struct q2ion_buffer *b = d->priv;
	struct dma_buf_attachment *a;

	if (!b->cached)
		return 0;
	dma_resv_lock(d->resv, NULL);
	list_for_each_entry(a, &d->attachments, node) {
		if (!a->sgt)
			continue;
		if (begin)
			dma_sync_sgtable_for_cpu(a->dev, a->sgt, dir);
		else
			dma_sync_sgtable_for_device(a->dev, a->sgt, dir);
	}
	dma_resv_unlock(d->resv);
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
	unsigned long i;

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

static int q2ion_alloc(struct ion_allocation_data *req)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct q2ion_buffer *b;
	struct dma_buf *d;
	unsigned long i = 0;
	int fd = -ENOMEM;

	if (!req->len || !req->heap_id_mask)
		return -EINVAL;
	if (req->flags & (ION_FLAG_SECURE | ION_FLAGS_CP_MASK))
		return -EOPNOTSUPP;
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->size = PAGE_ALIGN(req->len);
	b->nr_pages = b->size >> PAGE_SHIFT;
	b->cached = req->flags & ION_FLAG_CACHED;
	mutex_init(&b->lock);
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
	if (IS_ERR(d)) {
		fd = PTR_ERR(d);
		goto err_pages;
	}
	fd = dma_buf_fd(d, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(d);
		return fd;
	}
	req->fd = fd;
	return 0;

err_pages:
	i = min(i, b->nr_pages);
	while (i--)
		__free_page(b->pages[i]);
	kvfree(b->pages);
err_buf:
	kfree(b);
	return fd;
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
