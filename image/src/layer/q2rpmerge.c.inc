// q2rpmerge - merges vrcompositor's one-draw render passes (included by q2vrdisplay.c; vrcompositor only).
//
// Adapted from quest1_rpmerge.c of Quest1-SteamOS (github.com/nicolas-riera/Quest1-SteamOS), MIT licence:
//   Copyright (c) 2026 nicolas-riera
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
//   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Changes for this port: our layer's types and libc-free build, our own per-command-buffer staging arena (his layer's
// ub_* arenas), the off switches below, flush hooks for the Vulkan 1.3 "2" commands.
//
// vrcompositor draws each layer of an eye (environment, overlays, every controller part, the laser...) in a render pass
// of its own.  Before each draw it rewrites the draw's uniforms with vkCmdUpdateBuffer between a fragment-to-transfer and
// a transfer-to-vertex barrier.  On Turnip every such boundary is a GPU wait-for-idle plus cache flushes (and with
// TU_DEBUG=flushpass a full clean + invalidate), so the many small passes of the controllers and the laser add up.
// The layer keeps the render pass open across them instead:
// - The app's vkCmdEndRenderPass is held back.  If the next render pass begins on the same framebuffer with a render pass
//   that loads what the open one stores (same formats, layouts and store ops), it continues the open one.
// - The uniform updates in between are absorbed: the data goes into a CPU copy of the buffer (kept for every buffer that
//   is only a uniform and transfer buffer, up to 64 KB).  Buffer barriers that only cover such buffers are dropped.
// - Before each draw in a merged pass, every bound descriptor set whose uniform-buffer descriptors read an updated buffer
//   is replaced by a copy whose descriptors point at a snapshot of the CPU copy in the command buffer's staging arena.
// - When the merged pass really ends (another framebuffer, a transfer, a dispatch, an image barrier, the end of the
//   command buffer...), the updated buffers get their latest contents with one copy between two barriers, and the app's
//   own descriptor sets are bound again.
// - A draw whose sets cannot be redirected splits the pass: end, buffers updated, begin again with a loading render pass.
// Off: /var/lib/q2/norpmerge (at start) or Q2VR_RPMERGE=0; at run time "touch /tmp/q2vr-nomerge" (from the next pass).
#include <stdbool.h>

#define RM_BUF_MAX (64 * 1024)
#define RM_SLOTS 8
#define RM_DIRTY 16
#define RM_SET_BUFS 8
#define RM_HASH 256
#define RM_SETS 4096
#define RM_LAYOUTS 256
#define RM_RPS 128
#define RM_CBS 32
#define RM_CHUNK (256 * 1024)
#define RM_CHUNKS 16

static int rm_active(void)
{
	static int on = -1;
	if (on >= 0) return on;
	on = 0;
	char c[32]; long n = 0; int f = open("/proc/self/comm", 0);
	if (f >= 0) { n = read(f, c, sizeof c - 1); close(f); }
	if (n > 0 && c[n - 1] == '\n') n--;
	c[n > 0 ? n : 0] = 0;
	if (strstr(c, "vrcompositor") == c && envi("Q2VR_RPMERGE", 1)) {
		int g = open("/var/lib/q2/norpmerge", 0);
		if (g >= 0) close(g); else on = 1;
	}
	if (strstr(c, "vrcompositor") == c) qlog("render pass merge: %s", on ? "on" : "off (/var/lib/q2/norpmerge or Q2VR_RPMERGE=0)");
	return on;
}

static bool rm_off(void)   // run-time switch, read once a second
{
	static uint64_t checked; static bool off;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		int f = open("/tmp/q2vr-nomerge", 0);
		off = f >= 0; if (f >= 0) close(f);
	}
	return off;
}

static qmutex rm_lock;   // zero-initialised = a valid default glibc mutex
static unsigned rm_hash(uint64_t h) { return (unsigned)((h >> 4) * 2654435761u); }

// --- buffers that are only uniform/transfer buffers, with a CPU copy of what vkCmdUpdateBuffer wrote --------------

struct rm_buf { VkBuffer b; VkDeviceSize size; uint8_t *shadow, *valid; struct rm_buf *next; };
static struct rm_buf *rm_bufs[RM_HASH];

static struct rm_buf *rm_buf_find(VkBuffer b)   // under rm_lock
{
	for (struct rm_buf *r = rm_bufs[rm_hash((uint64_t)b) % RM_HASH]; r; r = r->next)
		if (r->b == b) return r;
	return NULL;
}

static void rm_forget_buffer(VkBuffer b)
{
	if (!rm_active() || !b) return;
	pthread_mutex_lock(&rm_lock);
	for (struct rm_buf **p = &rm_bufs[rm_hash((uint64_t)b) % RM_HASH]; *p; p = &(*p)->next)
		if ((*p)->b == b) { struct rm_buf *r = *p; *p = r->next; free(r->shadow); free(r->valid); free(r); break; }
	pthread_mutex_unlock(&rm_lock);
}

static void rm_note_buffer(VkBuffer b, const VkBufferCreateInfo *ci)
{
	const VkBufferUsageFlags ok = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (!rm_active()) return;
	rm_forget_buffer(b);   // a handle used again
	if (!(ci->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || (ci->usage & ~ok) || ci->size > RM_BUF_MAX) return;
	struct rm_buf *r = calloc(1, sizeof(*r));
	if (!r) return;
	r->b = b; r->size = ci->size;
	r->shadow = calloc(1, ci->size); r->valid = calloc(1, (ci->size + 7) / 8);
	if (!r->shadow || !r->valid) { free(r->shadow); free(r->valid); free(r); return; }
	pthread_mutex_lock(&rm_lock);
	unsigned h = rm_hash((uint64_t)b) % RM_HASH;
	r->next = rm_bufs[h]; rm_bufs[h] = r;
	pthread_mutex_unlock(&rm_lock);
}

static void rm_buf_mark(struct rm_buf *r, VkDeviceSize off, VkDeviceSize size, bool valid)
{
	for (VkDeviceSize i = off; i < off + size && i < r->size; i++)
		if (valid) r->valid[i / 8] |= (uint8_t)(1u << (i % 8));
		else r->valid[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static bool rm_buf_valid(const struct rm_buf *r, VkDeviceSize off, VkDeviceSize size)
{
	if (off + size > r->size) return false;
	for (VkDeviceSize i = off; i < off + size; i++)
		if (!(r->valid[i / 8] & (1u << (i % 8)))) return false;
	return true;
}

static void rm_buf_unknown(VkBuffer b, VkDeviceSize off, VkDeviceSize size)   // a transfer into b whose data the layer does not see
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	struct rm_buf *r = rm_buf_find(b);
	if (r && off < r->size) rm_buf_mark(r, off, size == VK_WHOLE_SIZE ? r->size - off : size, false);
	pthread_mutex_unlock(&rm_lock);
}

// --- descriptor set layouts and sets ----------------------------------------------------------------------------

struct rm_layout { VkDescriptorSetLayout l; uint32_t n; struct { uint32_t binding, count; VkDescriptorType type; } b[32]; };
static struct rm_layout rm_layouts[RM_LAYOUTS];
static unsigned rm_nlayouts;

static const struct rm_layout *rm_layout_find(VkDescriptorSetLayout l)   // under rm_lock
{
	for (unsigned i = 0; i < RM_LAYOUTS && i < rm_nlayouts; i++)
		if (rm_layouts[i].l == l) return &rm_layouts[i];
	return NULL;
}

static void rm_note_layout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo *ci)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	struct rm_layout *e = (struct rm_layout *)rm_layout_find(l);
	if (!e) e = &rm_layouts[rm_nlayouts++ % RM_LAYOUTS];
	memset(e, 0, sizeof(*e));
	if (ci->bindingCount <= 32 && !ci->pNext) {   // else unknown: its sets are never redirected
		e->l = l; e->n = ci->bindingCount;
		for (uint32_t i = 0; i < ci->bindingCount; i++) {
			e->b[i].binding = ci->pBindings[i].binding;
			e->b[i].count = ci->pBindings[i].descriptorCount;
			e->b[i].type = ci->pBindings[i].descriptorType;
		}
	}
	pthread_mutex_unlock(&rm_lock);
}

struct rm_set {
	VkDescriptorSet s; VkDescriptorSetLayout l; bool bad; uint32_t nbuf;
	struct { uint32_t binding, elem; VkDescriptorType type; VkBuffer b; VkDeviceSize off, range; } buf[RM_SET_BUFS];
};
static struct rm_set rm_sets[RM_SETS];

static struct rm_set *rm_set_find(VkDescriptorSet s, bool add)   // under rm_lock
{
	unsigned h = rm_hash((uint64_t)s);
	for (unsigned k = 0; k < 16; k++) {
		struct rm_set *e = &rm_sets[(h + k) % RM_SETS];
		if (e->s == s || (add && !e->s)) return e;
	}
	return add ? &rm_sets[h % RM_SETS] : NULL;   // evicts one: it is then unknown, and split on
}

static void rm_note_sets(const VkDescriptorSetAllocateInfo *ai, const VkDescriptorSet *sets)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
		struct rm_set *e = rm_set_find(sets[i], true);
		memset(e, 0, sizeof(*e));
		e->s = sets[i]; e->l = ai->pSetLayouts[i];
		e->bad = ai->pNext != NULL || !rm_layout_find(e->l);
	}
	pthread_mutex_unlock(&rm_lock);
}

static bool rm_buffer_type(VkDescriptorType t)
{
	return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
	       t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static void rm_note_writes(uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < nw; i++) {
		struct rm_set *e = rm_set_find(w[i].dstSet, false);
		if (!e || e->bad) continue;
		const struct rm_layout *l = rm_layout_find(e->l);
		uint32_t count = 0;
		for (uint32_t k = 0; l && k < l->n; k++) if (l->b[k].binding == w[i].dstBinding) count = l->b[k].count;
		if (w[i].dstArrayElement + w[i].descriptorCount > count) { e->bad = true; continue; }   // runs over into the next binding
		if (!rm_buffer_type(w[i].descriptorType)) continue;
		for (uint32_t j = 0; j < w[i].descriptorCount && !e->bad; j++) {
			uint32_t elem = w[i].dstArrayElement + j, k;
			for (k = 0; k < e->nbuf; k++) if (e->buf[k].binding == w[i].dstBinding && e->buf[k].elem == elem) break;
			if (k == RM_SET_BUFS) { e->bad = true; break; }
			if (k == e->nbuf) e->nbuf++;
			const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[j];
			e->buf[k].binding = w[i].dstBinding; e->buf[k].elem = elem; e->buf[k].type = w[i].descriptorType;
			e->buf[k].b = bi->buffer; e->buf[k].off = bi->offset; e->buf[k].range = bi->range;
		}
	}
	for (uint32_t i = 0; i < nc; i++) {   // copies from other sets: not followed
		struct rm_set *e = rm_set_find(c[i].dstSet, false);
		if (e) e->bad = true;
	}
	pthread_mutex_unlock(&rm_lock);
}

static void rm_note_template_write(VkDescriptorSet s)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	struct rm_set *e = rm_set_find(s, false);
	if (e) e->bad = true;
	pthread_mutex_unlock(&rm_lock);
}

// --- render passes ------------------------------------------------------------------------------------------------

struct rm_rp { VkRenderPass rp, load; uint32_t n, ncol; bool ds; VkAttachmentDescription att[4]; };   // load: the same, loading every attachment
static struct rm_rp rm_rps[RM_RPS];
static unsigned rm_nrps;

static const struct rm_rp *rm_rp_find(VkRenderPass rp)   // under rm_lock
{
	for (unsigned i = 0; i < RM_RPS && i < rm_nrps; i++) if (rm_rps[i].rp == rp) return &rm_rps[i];
	return NULL;
}

static void rm_note_rp(struct dev *d, VkRenderPass rp, const VkRenderPassCreateInfo *ci)
{
	if (!rm_active() || !d->CreateRenderPass || ci->pNext || ci->subpassCount != 1 || ci->attachmentCount == 0 || ci->attachmentCount > 4) return;
	const VkSubpassDescription *sp = &ci->pSubpasses[0];
	if (sp->inputAttachmentCount || sp->pResolveAttachments || sp->preserveAttachmentCount) return;
	struct rm_rp e; memset(&e, 0, sizeof e);
	e.rp = rp; e.n = ci->attachmentCount; e.ncol = sp->colorAttachmentCount; e.ds = sp->pDepthStencilAttachment != NULL;
	VkAttachmentDescription load[4];
	bool loads = true;
	for (uint32_t i = 0; i < ci->attachmentCount; i++) {
		e.att[i] = load[i] = ci->pAttachments[i];
		if (load[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || load[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) loads = false;
		load[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (load[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) load[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (load[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) load[i].initialLayout = load[i].finalLayout;
	}
	if (loads) e.load = rp;
	else {
		VkRenderPassCreateInfo lci = *ci; lci.pAttachments = load;
		if (d->CreateRenderPass(d->dev, &lci, NULL, &e.load) != VK_SUCCESS) e.load = VK_NULL_HANDLE;
	}
	pthread_mutex_lock(&rm_lock);
	struct rm_rp *slot = (struct rm_rp *)rm_rp_find(rp);
	if (!slot) slot = &rm_rps[rm_nrps++ % RM_RPS];   // (an evicted variant is leaked: vrcompositor makes a few dozen)
	*slot = e;
	pthread_mutex_unlock(&rm_lock);
}

static void rm_forget_rp(struct dev *d, VkRenderPass rp)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	struct rm_rp *e = (struct rm_rp *)rm_rp_find(rp);
	VkRenderPass load = e && e->load != rp ? e->load : VK_NULL_HANDLE;
	if (e) memset(e, 0, sizeof(*e));
	pthread_mutex_unlock(&rm_lock);
	if (load && d->DestroyRenderPass) d->DestroyRenderPass(d->dev, load, NULL);
}

static bool rm_continues(const struct rm_rp *a, const struct rm_rp *b)   // b loads what a stores, into the same layouts
{
	if (a->n != b->n || a->ncol != b->ncol || a->ds != b->ds) return false;
	for (uint32_t i = 0; i < a->n; i++) {
		const VkAttachmentDescription *x = &a->att[i], *y = &b->att[i];
		if (x->format != y->format || x->samples != y->samples || x->storeOp != y->storeOp ||
		    x->stencilStoreOp != y->stencilStoreOp || x->finalLayout != y->finalLayout ||
		    y->initialLayout != x->finalLayout || y->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
		    y->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			return false;
	}
	return true;
}

// --- per command buffer -------------------------------------------------------------------------------------------

struct rm_slot { VkDescriptorSet orig, cur; VkPipelineLayout pl; unsigned ver; bool dyn; };
struct rm_chunk { VkBuffer b; VkDeviceMemory m; uint8_t *p; };
struct rm_cb {
	VkCommandBuffer cb; struct dev *d; VkDescriptorPool pool;
	bool cand;      // the app's current render pass may go on into its next one
	bool deferred;  // the app ended it, the layer did not
	bool dropped;   // barriers dropped since
	VkRenderPass rp, cont;   // the open pass as begun, the app's latest in it
	VkFramebuffer fb; VkRect2D area;
	unsigned ndirty, ver;
	struct { VkBuffer b; VkDeviceSize lo, hi; } dirty[RM_DIRTY];   // updates absorbed in this pass
	struct rm_slot slot[RM_SLOTS];
	// staging arena (uniform snapshots, buffer contents): reset when the command buffer is begun again
	struct rm_chunk ch[RM_CHUNKS]; unsigned nch, cur; VkDeviceSize off;
};
static struct rm_cb rm_cbs[RM_CBS];
static unsigned rm_stat_merged, rm_stat_splits, rm_stat_redirects, rm_stat_passes;

static struct rm_cb *rm_get(VkCommandBuffer cb)
{
	if (!rm_active()) return NULL;
	struct rm_cb *r = NULL;
	pthread_mutex_lock(&rm_lock);
	for (unsigned i = 0; i < RM_CBS && !r; i++) if (rm_cbs[i].cb == cb) r = &rm_cbs[i];
	pthread_mutex_unlock(&rm_lock);
	return r;
}

static uint32_t rm_hostmem(struct dev *d, uint32_t bits)
{
	const VkMemoryPropertyFlags w = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d->memprops.memoryTypes[i].propertyFlags & w) == w) return i;
	return 0xffffffffu;
}

// n bytes of host-visible, coherent memory usable as a uniform buffer and a copy source, valid until the command
// buffer is begun again (it is then no longer pending)
static bool rm_stage(struct rm_cb *c, VkDeviceSize n, VkBuffer *buf, VkDeviceSize *off, void **ptr)
{
	struct dev *d = c->d;
	if (n > RM_CHUNK) return false;
	for (;;) {
		if (c->cur < c->nch) {
			VkDeviceSize o = (c->off + 255) & ~(VkDeviceSize)255;
			if (o + n <= RM_CHUNK) { *buf = c->ch[c->cur].b; *off = o; *ptr = c->ch[c->cur].p + o; c->off = o + n; return true; }
			c->cur++; c->off = 0;
			continue;
		}
		if (c->nch == RM_CHUNKS) return false;
		struct rm_chunk *k = &c->ch[c->nch];
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, RM_CHUNK,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
		if (d->CreateBuffer(d->dev, &bci, 0, &k->b) != VK_SUCCESS) return false;
		VkMemoryRequirements mr; d->GetBufferMemoryRequirements(d->dev, k->b, &mr);
		uint32_t t = rm_hostmem(d, mr.memoryTypeBits);
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, t };
		void *p = 0;
		if (t == 0xffffffffu || d->AllocateMemory(d->dev, &mai, 0, &k->m) != VK_SUCCESS) { d->DestroyBuffer(d->dev, k->b, 0); return false; }
		if (d->BindBufferMemory(d->dev, k->b, k->m, 0) != VK_SUCCESS || d->MapMemory(d->dev, k->m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
			d->DestroyBuffer(d->dev, k->b, 0); d->FreeMemory(d->dev, k->m, 0); return false;
		}
		k->p = p; c->cur = c->nch++; c->off = 0;
	}
}

static void rm_stats(void);
static void rm_cb_begin(struct dev *d, VkCommandBuffer cb)   // vkBeginCommandBuffer: not pending, its pool and arena can be reused
{
	if (!rm_active()) return;
	rm_stats();
	pthread_mutex_lock(&rm_lock);
	struct rm_cb *c = NULL;
	for (unsigned i = 0; i < RM_CBS && !c; i++) if (rm_cbs[i].cb == cb) c = &rm_cbs[i];
	for (unsigned i = 0; i < RM_CBS && !c; i++)
		if (!rm_cbs[i].cb) { c = &rm_cbs[i]; memset(c, 0, sizeof *c); c->cb = cb; c->d = d; }
	pthread_mutex_unlock(&rm_lock);
	if (!c) return;   // more command buffers than tracked: those keep their passes
	VkDescriptorPool pool = c->pool;
	struct rm_chunk ch[RM_CHUNKS]; unsigned nch = c->nch;
	memcpy(ch, c->ch, sizeof ch);
	memset(c, 0, sizeof(*c));
	c->cb = cb; c->d = d; c->pool = pool; memcpy(c->ch, ch, sizeof ch); c->nch = nch;
	if (pool) d->ResetDescriptorPool(d->dev, pool, 0);
}

static void rm_cb_drop(struct dev *d, VkCommandBuffer cb)   // freed (or its pool destroyed / reset with release)
{
	if (!rm_active()) return;
	struct rm_cb c; int found = 0;
	pthread_mutex_lock(&rm_lock);
	for (unsigned i = 0; i < RM_CBS; i++)
		if (rm_cbs[i].cb == cb) { c = rm_cbs[i]; found = 1; memset(&rm_cbs[i], 0, sizeof(rm_cbs[i])); }
	pthread_mutex_unlock(&rm_lock);
	if (!found) return;
	if (c.pool) d->DestroyDescriptorPool(d->dev, c.pool, NULL);
	for (unsigned i = 0; i < c.nch; i++) { d->DestroyBuffer(d->dev, c.ch[i].b, 0); d->FreeMemory(d->dev, c.ch[i].m, 0); }
}

// which pool each command buffer came from (vkAllocateCommandBuffers), for vkDestroyCommandPool
static struct { VkCommandBuffer cb; VkCommandPool pool; } rm_cbpool[256];
static void rm_note_cbs(const VkCommandBufferAllocateInfo *ai, const VkCommandBuffer *cbs)
{
	if (!rm_active()) return;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
		unsigned h = rm_hash((uint64_t)(uintptr_t)cbs[i]) % 256, k;
		for (k = 0; k < 256 && rm_cbpool[(h + k) % 256].cb && rm_cbpool[(h + k) % 256].cb != cbs[i]; k++) ;
		if (k < 256) { rm_cbpool[(h + k) % 256].cb = cbs[i]; rm_cbpool[(h + k) % 256].pool = ai->commandPool; }
	}
	pthread_mutex_unlock(&rm_lock);
}
static VkCommandPool rm_cb_pool(VkCommandBuffer cb)
{
	VkCommandPool p = VK_NULL_HANDLE;
	pthread_mutex_lock(&rm_lock);
	for (unsigned k = 0; k < 256; k++) if (rm_cbpool[k].cb == cb) { p = rm_cbpool[k].pool; break; }
	pthread_mutex_unlock(&rm_lock);
	return p;
}
static void rm_cbpool_forget(VkCommandBuffer cb)
{
	pthread_mutex_lock(&rm_lock);
	for (unsigned k = 0; k < 256; k++) if (rm_cbpool[k].cb == cb) { rm_cbpool[k].cb = 0; rm_cbpool[k].pool = VK_NULL_HANDLE; }
	pthread_mutex_unlock(&rm_lock);
}
static void rm_pool_drop(struct dev *d, VkCommandPool pool)   // vkDestroyCommandPool frees its command buffers
{
	if (!rm_active()) return;
	VkCommandBuffer list[RM_CBS]; unsigned n = 0;
	pthread_mutex_lock(&rm_lock);
	for (unsigned i = 0; i < RM_CBS; i++) if (rm_cbs[i].cb && rm_cbs[i].d == d) list[n++] = rm_cbs[i].cb;
	pthread_mutex_unlock(&rm_lock);
	for (unsigned i = 0; i < n; i++) if (rm_cb_pool(list[i]) == pool) { rm_cb_drop(d, list[i]); rm_cbpool_forget(list[i]); }
	pthread_mutex_lock(&rm_lock);
	for (unsigned k = 0; k < 256; k++) if (rm_cbpool[k].pool == pool) { rm_cbpool[k].cb = 0; rm_cbpool[k].pool = VK_NULL_HANDLE; }
	pthread_mutex_unlock(&rm_lock);
}

static void rm_settle(struct rm_cb *c)   // updated buffers get their latest contents; the app's sets are bound again. Outside a pass.
{
	struct dev *d = c->d;
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
	if (c->dropped || c->ndirty)
		d->CmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	if (c->ndirty) {
		for (unsigned i = 0; i < c->ndirty; i++) {
			VkDeviceSize lo = c->dirty[i].lo, n = c->dirty[i].hi - lo;
			VkBuffer sb; VkDeviceSize soff; void *p;
			pthread_mutex_lock(&rm_lock);
			struct rm_buf *r = rm_buf_find(c->dirty[i].b);
			bool ok = r && rm_buf_valid(r, lo, n);
			if (ok && rm_stage(c, n, &sb, &soff, &p)) { memcpy(p, r->shadow + lo, n); pthread_mutex_unlock(&rm_lock);
				VkBufferCopy bc = { soff, lo, n };
				d->CmdCopyBuffer(c->cb, sb, c->dirty[i].b, 1, &bc);
			} else if (ok) { d->CmdUpdateBuffer(c->cb, c->dirty[i].b, lo, n, r->shadow + lo); pthread_mutex_unlock(&rm_lock);   // copies the data at once
			} else { pthread_mutex_unlock(&rm_lock);
				qlog("render pass merge: no copy of buffer %p [%llu, %llu): its update is lost", (void *)c->dirty[i].b,
				     (unsigned long long)lo, (unsigned long long)(lo + n));
			}
		}
		d->CmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	}
	for (unsigned i = 0; i < RM_SLOTS; i++) {
		struct rm_slot *s = &c->slot[i];
		if (s->orig && s->cur != s->orig) { d->CmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl, i, 1, &s->orig, 0, NULL); s->cur = s->orig; }
	}
	c->ndirty = 0; c->dropped = false;
}

static void rm_close(struct rm_cb *c)   // the held-back end of the app's render pass, before a command that cannot go in it
{
	c->deferred = false; c->cand = false;
	c->d->CmdEndRenderPass(c->cb);
	rm_settle(c);
}

static void rm_split(struct rm_cb *c)   // inside a merged pass, before a draw that cannot read the updates through copies
{
	pthread_mutex_lock(&rm_lock);
	const struct rm_rp *r = rm_rp_find(c->cont);
	VkRenderPass load = r ? r->load : VK_NULL_HANDLE;
	pthread_mutex_unlock(&rm_lock);
	c->d->CmdEndRenderPass(c->cb);
	rm_settle(c);
	rm_stat_splits++;
	if (!load) { qlog("render pass merge: no loading render pass to split %p", (void *)c->cont); load = c->cont; }
	VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, load, c->fb, c->area, 0, NULL };
	c->d->CmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	c->rp = load;
}

static void rm_flush(VkCommandBuffer cb)
{
	struct rm_cb *c = rm_get(cb);
	if (c && c->deferred) rm_close(c);
}

static bool rm_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents)   // true: continues the open pass
{
	struct rm_cb *c = rm_get(cb);
	if (!c) return false;
	rm_stat_passes++;
	pthread_mutex_lock(&rm_lock);
	const struct rm_rp *r = rm_rp_find(bi->renderPass), *o = c->deferred ? rm_rp_find(c->rp) : NULL;
	bool cont = r && o && bi->framebuffer == c->fb && contents == VK_SUBPASS_CONTENTS_INLINE && !bi->pNext && rm_continues(o, r) &&
	            bi->renderArea.offset.x >= c->area.offset.x && bi->renderArea.offset.y >= c->area.offset.y &&
	            bi->renderArea.offset.x + bi->renderArea.extent.width <= c->area.offset.x + c->area.extent.width &&
	            bi->renderArea.offset.y + bi->renderArea.extent.height <= c->area.offset.y + c->area.extent.height;
	bool cand = r && r->load && contents == VK_SUBPASS_CONTENTS_INLINE && !bi->pNext && !rm_off();
	pthread_mutex_unlock(&rm_lock);
	if (cont) { c->deferred = false; c->cont = bi->renderPass; rm_stat_merged++; return true; }
	if (c->deferred) rm_close(c);
	c->cand = cand;
	if (cand) { c->rp = c->cont = bi->renderPass; c->fb = bi->framebuffer; c->area = bi->renderArea; }
	return false;
}

static bool rm_end(VkCommandBuffer cb)   // vkCmdEndRenderPass: true when held back
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->cand) return false;
	c->deferred = true;
	return true;
}

// vkCmdPipelineBarrier: true to drop it (buffer barriers of tracked uniform buffers between two passes being merged)
static bool rm_barrier(VkCommandBuffer cb, uint32_t nm, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni)
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->deferred) return false;
	bool drop = nm == 0 && ni == 0 && nb > 0;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < nb && drop; i++) drop = rm_buf_find(b[i].buffer) != NULL && b[i].srcQueueFamilyIndex == b[i].dstQueueFamilyIndex;
	pthread_mutex_unlock(&rm_lock);
	if (drop) { c->dropped = true; return true; }
	rm_close(c);
	return false;
}

// vkCmdUpdateBuffer: always noted in the buffer's copy; true when absorbed (the caller records nothing)
static bool rm_update(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data)
{
	if (!rm_active()) return false;
	pthread_mutex_lock(&rm_lock);
	struct rm_buf *r = rm_buf_find(b);
	if (r && off + size <= r->size) { memcpy(r->shadow + off, data, size); rm_buf_mark(r, off, size, true); }
	pthread_mutex_unlock(&rm_lock);
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->deferred) return false;
	unsigned k;
	for (k = 0; k < c->ndirty && c->dirty[k].b != b; k++) ;
	if (!r || off + size > r->size || k == RM_DIRTY) { rm_close(c); return false; }
	if (k == c->ndirty) { c->ndirty++; c->dirty[k].b = b; c->dirty[k].lo = off; c->dirty[k].hi = off + size; }
	else {
		if (off < c->dirty[k].lo) c->dirty[k].lo = off;
		if (off + size > c->dirty[k].hi) c->dirty[k].hi = off + size;
	}
	c->ver++;
	return true;
}

static void rm_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd)
{
	if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
	struct rm_cb *c = rm_get(cb);
	if (!c) return;
	for (uint32_t i = 0; i < n && first + i < RM_SLOTS; i++) {
		struct rm_slot s = { sets[i], sets[i], pl, 0, nd > 0 };
		c->slot[first + i] = s;
	}
}

// 1: the set reads an updated buffer through uniform-buffer descriptors only; 0: none; -1: cannot tell or redirect (under rm_lock)
static int rm_set_reads(const struct rm_cb *c, VkDescriptorSet s)
{
	const struct rm_set *e = rm_set_find(s, false);
	if (!e || e->bad) return -1;
	int r = 0;
	for (uint32_t i = 0; i < e->nbuf; i++)
		for (unsigned k = 0; k < c->ndirty; k++)
			if (e->buf[i].b == c->dirty[k].b) { if (e->buf[i].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) return -1; r = 1; }
	return r;
}

static bool rm_redirect(struct rm_cb *c, unsigned i)   // binds a copy of slot i's set whose updated uniform buffers are snapshots
{
	struct dev *d = c->d;
	struct rm_slot *s = &c->slot[i];
	if (!c->pool) {
		VkDescriptorPoolSize sizes[11];
		for (unsigned t = 0; t < 11; t++) { sizes[t].type = (VkDescriptorType)t; sizes[t].descriptorCount = 1024; }
		VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 256, 11, sizes };
		if (d->CreateDescriptorPool(d->dev, &pci, NULL, &c->pool) != VK_SUCCESS) { c->pool = VK_NULL_HANDLE; return false; }
	}
	struct rm_set e; struct rm_layout l;
	pthread_mutex_lock(&rm_lock);
	const struct rm_set *pe = rm_set_find(s->orig, false);
	const struct rm_layout *pl = pe ? rm_layout_find(pe->l) : NULL;
	if (pe) e = *pe;
	if (pl) l = *pl;
	pthread_mutex_unlock(&rm_lock);
	if (!pe || !pl || e.bad) return false;
	VkDescriptorSet copy;
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, c->pool, 1, &e.l };
	if (d->AllocateDescriptorSets(d->dev, &ai, &copy) != VK_SUCCESS) return false;
	VkCopyDescriptorSet cp[32]; uint32_t ncp = 0;
	for (uint32_t k = 0; k < l.n; k++) {
		if (l.b[k].count == 0) continue;
		if ((unsigned)l.b[k].type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) return false;
		VkCopyDescriptorSet x = { VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, s->orig, l.b[k].binding, 0, copy, l.b[k].binding, 0, l.b[k].count };
		cp[ncp++] = x;
	}
	VkWriteDescriptorSet wr[RM_SET_BUFS]; VkDescriptorBufferInfo bi[RM_SET_BUFS]; uint32_t nwr = 0;
	for (uint32_t k = 0; k < e.nbuf; k++) {
		bool dirty = false;
		for (unsigned j = 0; j < c->ndirty; j++) dirty |= e.buf[k].b == c->dirty[j].b;
		if (!dirty) continue;
		VkDeviceSize off = e.buf[k].off, n = e.buf[k].range;
		VkBuffer sb; VkDeviceSize soff; void *p; bool ok = false;
		pthread_mutex_lock(&rm_lock);
		struct rm_buf *r = rm_buf_find(e.buf[k].b);
		if (r && n == VK_WHOLE_SIZE) n = off < r->size ? r->size - off : 0;
		if (r && n && rm_buf_valid(r, off, n) && rm_stage(c, n, &sb, &soff, &p)) { memcpy(p, r->shadow + off, n); ok = true; }
		pthread_mutex_unlock(&rm_lock);
		if (!ok) return false;
		VkDescriptorBufferInfo b1 = { sb, soff, n }; bi[nwr] = b1;
		VkWriteDescriptorSet w1 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, copy, e.buf[k].binding, e.buf[k].elem, 1,
		                            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nwr], NULL };
		wr[nwr++] = w1;
	}
	d->UpdateDescriptorSets(d->dev, 0, NULL, ncp, cp);   // copies first: writes are applied before copies
	d->UpdateDescriptorSets(d->dev, nwr, wr, 0, NULL);
	d->CmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl, i, 1, &copy, 0, NULL);
	s->cur = copy; s->ver = c->ver;
	rm_stat_redirects++;
	return true;
}

static void rm_draw(VkCommandBuffer cb)   // before a draw: in a merged pass with absorbed updates, redirect the sets that read them
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->ndirty) return;
	for (unsigned i = 0; i < RM_SLOTS; i++) {
		struct rm_slot *s = &c->slot[i];
		if (!s->orig || (s->cur != s->orig && s->ver == c->ver)) continue;
		pthread_mutex_lock(&rm_lock);
		int reads = rm_set_reads(c, s->orig);
		pthread_mutex_unlock(&rm_lock);
		if (reads == 0) continue;
		if (reads < 0 || s->dyn || !rm_redirect(c, i)) { rm_split(c); return; }   // settles every slot: nothing left to redirect
	}
}

static void rm_stats(void)   // every 5 s, from vkBeginCommandBuffer
{
	static uint64_t last;
	uint64_t t = now_ns();
	if (t - last < 5000000000ull) return;
	last = t;
	unsigned p = __atomic_exchange_n(&rm_stat_passes, 0, __ATOMIC_RELAXED), m = __atomic_exchange_n(&rm_stat_merged, 0, __ATOMIC_RELAXED),
	         s = __atomic_exchange_n(&rm_stat_splits, 0, __ATOMIC_RELAXED), r = __atomic_exchange_n(&rm_stat_redirects, 0, __ATOMIC_RELAXED);
	qlog("render pass merge: %u of %u render passes merged, %u splits, %u descriptor set copies in 5 s%s", m, p, s, r,
	     rm_off() ? " (off: /tmp/q2vr-nomerge)" : "");
}
