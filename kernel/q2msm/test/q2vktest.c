// SPDX-License-Identifier: GPL-2.0-only
/*
 * q2vktest: offscreen Vulkan smoke test for the q2msm GPU driver.  Needs no display: every test does real GPU work,
 * reads the result back and checks it.  Each submission waits at most 5 s, so a GPU hang shows up as FAIL.
 *   1. fill     vkCmdFillBuffer 4 MiB                       (CP + memory path)
 *   2. compute  shader writes out[i] = i * 3 + 1, 1M words   (shader cores)
 *   3. draw     triangle into a 256x256 image + copy back    (full graphics pipeline)
 * Loads libvulkan.so.1 at runtime, so it only needs the Vulkan headers to build.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "comp.h"
#include "vert.h"
#include "frag.h"

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define VKI(name) PFN_##name name
VKI(vkCreateInstance); VKI(vkEnumeratePhysicalDevices); VKI(vkGetPhysicalDeviceProperties);
VKI(vkGetPhysicalDeviceQueueFamilyProperties); VKI(vkGetPhysicalDeviceMemoryProperties); VKI(vkCreateDevice);
VKI(vkGetDeviceProcAddr); VKI(vkGetDeviceQueue); VKI(vkCreateBuffer); VKI(vkGetBufferMemoryRequirements);
VKI(vkAllocateMemory); VKI(vkBindBufferMemory); VKI(vkMapMemory); VKI(vkCreateCommandPool);
VKI(vkAllocateCommandBuffers); VKI(vkBeginCommandBuffer); VKI(vkEndCommandBuffer); VKI(vkCmdFillBuffer);
VKI(vkQueueSubmit); VKI(vkCreateFence); VKI(vkWaitForFences); VKI(vkResetFences); VKI(vkResetCommandBuffer);
VKI(vkCreateShaderModule); VKI(vkCreateDescriptorSetLayout); VKI(vkCreatePipelineLayout);
VKI(vkCreateComputePipelines); VKI(vkCreateDescriptorPool); VKI(vkAllocateDescriptorSets);
VKI(vkUpdateDescriptorSets); VKI(vkCmdBindPipeline); VKI(vkCmdBindDescriptorSets); VKI(vkCmdDispatch);
VKI(vkCmdPipelineBarrier); VKI(vkCreateImage); VKI(vkGetImageMemoryRequirements); VKI(vkBindImageMemory);
VKI(vkCreateImageView); VKI(vkCreateRenderPass); VKI(vkCreateFramebuffer); VKI(vkCreateGraphicsPipelines);
VKI(vkCmdBeginRenderPass); VKI(vkCmdEndRenderPass); VKI(vkCmdDraw); VKI(vkCmdCopyImageToBuffer);

static VkPhysicalDevice pdev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static uint32_t qfam;
static int fails;

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FATAL: %s = %d (line %d)\n", #x, r_, __LINE__); exit(2); } } while (0)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
	for (i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	printf("FATAL: no memory type %x\n", want);
	exit(2);
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	VkBuffer buf;

	CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, size, 0, map));
	return buf;
}

static void begin(void)
{
	VkCommandBufferBeginInfo b = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

	CHECK(vkResetCommandBuffer(cmd, 0));
	CHECK(vkBeginCommandBuffer(cmd, &b));
}

/* submit, wait at most 5 s; returns GPU time in ms or -1 on timeout */
static double submit(void)
{
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	double t0;
	VkResult r;

	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkResetFences(dev, 1, &fence));
	t0 = now_ms();
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
	if (r != VK_SUCCESS) {
		printf("  fence wait: %d (%s)\n", r, r == VK_TIMEOUT ? "GPU did not finish in 5 s" : "error");
		return -1;
	}
	return now_ms() - t0;
}

static void result(const char *name, int ok, double ms, const char *detail)
{
	printf("%-8s %s  %.2f ms  %s\n", name, ok ? "PASS" : "FAIL", ms, detail);
	fails += !ok;
}

static void test_fill(void)
{
	const VkDeviceSize n = 1 << 20;
	uint32_t *p, i, bad = 0;
	VkBuffer b = host_buffer(n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&p);
	char d[96];
	double ms;

	memset(p, 0, n * 4);
	begin();
	vkCmdFillBuffer(cmd, b, 0, n * 4, 0xdeadbeef);
	ms = submit();
	for (i = 0; i < n; i++)
		bad += p[i] != 0xdeadbeef;
	snprintf(d, sizeof(d), "%u/%u words wrong, [0]=%08x [last]=%08x", bad, (unsigned)n, p[0], p[n - 1]);
	result("fill", ms >= 0 && !bad, ms, d);
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
	VkShaderModule m;

	CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
	return m;
}

static void test_compute(void)
{
	const uint32_t n = 1 << 20;
	uint32_t *p, i, bad = 0, first = 0;
	VkBuffer b = host_buffer(n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&p);
	VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &bind };
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorSetLayout dl;
	VkPipelineLayout pl;
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1 };
	VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1 };
	VkDescriptorBufferInfo dbi = { b, 0, n * 4 };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	VkDescriptorPool dp;
	VkDescriptorSet ds;
	VkPipeline pipe;
	char d[96];
	double ms;

	memset(p, 0, n * 4);
	CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
	pli.pSetLayouts = &dl;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
	cpi.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader(comp_spv, sizeof(comp_spv)), .pName = "main" };
	cpi.layout = pl;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	dai.descriptorPool = dp;
	dai.pSetLayouts = &dl;
	CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
	w.dstSet = ds;
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

	begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdDispatch(cmd, n / 64, 1, 1);
	ms = submit();
	for (i = 0; i < n; i++)
		if (p[i] != i * 3 + 1 && !bad++)
			first = i;
	snprintf(d, sizeof(d), "%u/%u words wrong (first at %u: %08x)", bad, n, first, p[first]);
	result("compute", ms >= 0 && !bad, ms, d);
}

static void test_draw(void)
{
	const uint32_t W = 256, H = 256;
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
		VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
	VkPipelineShaderStageCreateInfo st[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
		.pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms_, .pColorBlendState = &cb };
	VkClearValue clear = { .color = { .float32 = { 0, 0, 1, 1 } } };	/* blue background */
	VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .attachmentCount = 1,
		.width = W, .height = H, .layers = 1 };
	VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderArea = sc,
		.clearValueCount = 1, .pClearValues = &clear };
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	VkImage img;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipelineLayout pl;
	VkPipeline pipe;
	uint32_t *px, centre, corner, red = 0, blue = 0, i;
	VkBuffer out = host_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&px);
	char d[128];
	double t;

	memset(px, 0, W * H * 4);
	CHECK(vkCreateImage(dev, &ici, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	vci.image = img;
	CHECK(vkCreateImageView(dev, &vci, NULL, &view));
	CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
	fbi.renderPass = rp;
	fbi.pAttachments = &view;
	CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
	st[0].module = shader(vert_spv, sizeof(vert_spv));
	st[1].module = shader(frag_spv, sizeof(frag_spv));
	gpi.layout = pl;
	gpi.renderPass = rp;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

	begin();
	rbi.renderPass = rp;
	rbi.framebuffer = fb;
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &copy);
	t = submit();

	/* the triangle (red) covers the centre; the top corners stay the clear colour (blue). ABGR in memory */
	for (i = 0; i < W * H; i++) {
		red += px[i] == 0xff0000ff;
		blue += px[i] == 0xffff0000;
	}
	centre = px[(H / 2) * W + W / 2];
	corner = px[0];
	snprintf(d, sizeof(d), "centre %08x corner %08x, %u red / %u blue px", centre, corner, red, blue);
	result("draw", t >= 0 && centre == 0xff0000ff && corner == 0xffff0000 && red > 10000 && blue > 10000, t, d);
}

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "q2vktest",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkPhysicalDeviceProperties props;
	VkQueueFamilyProperties qf[8];
	uint32_t n = 1, nq = 8, i;
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci };
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkCommandPool pool;

	if (!lib) {
		printf("FATAL: %s\n", dlerror());
		return 2;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define LI(name) name = (PFN_##name)gipa(inst, #name)
	LI(vkCreateInstance);
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkGetPhysicalDeviceQueueFamilyProperties);
	LI(vkGetPhysicalDeviceMemoryProperties); LI(vkCreateDevice); LI(vkGetDeviceProcAddr);
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pdev) < 0 ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS);
	if (!n) {
		printf("FATAL: no Vulkan device\n");
		return 2;
	}
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s (driver %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
		VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf);
	for (i = 0; i < nq; i++)
		if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
				(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
			break;
	qfam = i;
	qci.queueFamilyIndex = qfam;
	CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
#define LD(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	LD(vkGetDeviceQueue); LD(vkCreateBuffer); LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory);
	LD(vkBindBufferMemory); LD(vkMapMemory); LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers);
	LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer); LD(vkCmdFillBuffer); LD(vkQueueSubmit); LD(vkCreateFence);
	LD(vkWaitForFences); LD(vkResetFences); LD(vkResetCommandBuffer); LD(vkCreateShaderModule);
	LD(vkCreateDescriptorSetLayout); LD(vkCreatePipelineLayout); LD(vkCreateComputePipelines);
	LD(vkCreateDescriptorPool); LD(vkAllocateDescriptorSets); LD(vkUpdateDescriptorSets); LD(vkCmdBindPipeline);
	LD(vkCmdBindDescriptorSets); LD(vkCmdDispatch); LD(vkCmdPipelineBarrier); LD(vkCreateImage);
	LD(vkGetImageMemoryRequirements); LD(vkBindImageMemory); LD(vkCreateImageView); LD(vkCreateRenderPass);
	LD(vkCreateFramebuffer); LD(vkCreateGraphicsPipelines); LD(vkCmdBeginRenderPass); LD(vkCmdEndRenderPass);
	LD(vkCmdDraw); LD(vkCmdCopyImageToBuffer);
	vkGetDeviceQueue(dev, qfam, 0, &queue);
	cpi.queueFamilyIndex = qfam;
	CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
	cai.commandPool = pool;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	test_fill();
	test_compute();
	test_draw();
	printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
	return fails ? 1 : 0;
}
