// SPDX-License-Identifier: Unlicense
/*
 * An implicit Vulkan layer for SteamVR's compositor on a mainline kernel: its swapchains without
 * the storage usage it asks for and never uses.
 *
 * The A650 cannot compress storage images, so Mesa's display WSI would scan out uncompressed
 * images, which costs the compositor about 5 ms of GPU time per frame: 60-66 frames per second
 * instead of 90. Without that usage it resolves straight into compressed scanout images, with
 * Valve's Mesa as it is. Every other process passes through.
 *
 * It also paces the compositor one frame ahead, as the stock path paces it
 * (Q2_FRAMES_AHEAD=N): a present counts as shown once the present N before it is. The
 * compositor then records the next frame while the GPU renders this one. With a lead
 * (Q2_VSYNC_LEAD_US) the counter it paces itself by ticks that much before each vblank,
 * so that each frame starts that much earlier: with Steam's panel in view and rendering a job does
 * not fit between two vblanks on this GPU, and each frame it misses shows as the view jumping during
 * a head turn. The layer publishes the lead for the driver in SteamVR, which predicts its poses that
 * much further, for the real vblank.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))
#define MAX_DEVICES 16
#define POLL_NS 200000
#define PERIOD_NS 11111111 // Until two reads of the counter measure it.
#define SURFACE_COUNTER_TIMESTAMP_VALVE ((VkSurfaceCounterFlagBitsEXT)0x7ffffffd) // Valve's Mesa: count, time.

struct device
{
	VkDevice handle;
	PFN_vkGetDeviceProcAddr next_get_device_proc_addr;
	PFN_vkCreateSwapchainKHR next_create_swapchain;
	PFN_vkWaitForPresentKHR next_wait_for_present;
#ifdef VK_KHR_present_wait2
	PFN_vkWaitForPresent2KHR next_wait_for_present2;
#endif
	PFN_vkGetSwapchainCounterEXT next_get_counter;
	uint64_t read_count;                      //!< Its value at the client's last read.
	bool has_read;
	bool published;                          //!< This device's swapchain published the lead.
	uint64_t last_count, last_ns, period_ns; //!< The newest vblank, and the time between them.
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct device devices[MAX_DEVICES];
static PFN_vkGetInstanceProcAddr next_get_instance_proc_addr;

static bool
is_compositor(void)
{
	return strcmp(program_invocation_short_name, "vrcompositor") == 0;
}

static struct device *
find_device(VkDevice handle)
{
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].handle == handle) {
			return &devices[i];
		}
	}
	return NULL;
}

static void lead_publish(bool on);
static uint64_t frames_ahead(void);

static VKAPI_ATTR VkResult VKAPI_CALL
create_swapchain(VkDevice device,
                 const VkSwapchainCreateInfoKHR *create_info,
                 const VkAllocationCallbacks *allocator,
                 VkSwapchainKHR *swapchain)
{
	pthread_mutex_lock(&lock);
	struct device *d = find_device(device);
	PFN_vkCreateSwapchainKHR next = d != NULL ? d->next_create_swapchain : NULL;
	if (d != NULL && is_compositor()) {
		d->published = true;
	}
	pthread_mutex_unlock(&lock);
	if (next == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkSwapchainCreateInfoKHR info = *create_info;
	if (is_compositor()) {
		info.imageUsage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
		// It asks for two: its acquire then waits for a flip, whatever else this layer does.
		if (frames_ahead() > 0 && info.minImageCount < 4) {
			info.minImageCount = 4;
		}
		lead_publish(true);
	}
	return next(device, &info, allocator, swapchain);
}

static uint64_t
frames_ahead(void)
{
	const char *value = getenv("Q2_FRAMES_AHEAD");
	return value != NULL && is_compositor() ? strtoull(value, NULL, 10) : 0;
}

static uint64_t
now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t
vsync_lead_ns(void)
{
	const char *value = getenv("Q2_VSYNC_LEAD_US");
	return value != NULL && is_compositor() ? strtoull(value, NULL, 10) * 1000 : 0;
}

/*! For the driver in SteamVR: the lead applied here, removed with the device. */
static void
lead_publish(bool on)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	uint64_t lead = vsync_lead_ns();
	if (dir == NULL || lead == 0) {
		return;
	}
	char path[PATH_MAX], tmp[PATH_MAX + 4];
	snprintf(path, sizeof(path), "%s/q2-display-lead-us", dir);
	if (!on) {
		unlink(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "we");
	if (f != NULL) {
		fprintf(f, "%llu\n", (unsigned long long)(lead / 1000));
		if (fclose(f) == 0) {
			rename(tmp, path);
		}
	}
}

/*!
 * The vblank count and time as the client sees them: a vblank counts from its lead before it, at
 * the time the newest one and the period predict. The prediction is corrected at each real vblank.
 */
static VkResult
early_counter(struct device *d, VkDevice device, VkSwapchainKHR swapchain, uint64_t value[2])
{
	uint64_t raw[2] = {0, 0};
	VkResult result = d->next_get_counter(device, swapchain, SURFACE_COUNTER_TIMESTAMP_VALVE, raw);
	if (result != VK_SUCCESS) {
		return result;
	}
	if (raw[0] > d->last_count && d->last_count != 0) {
		uint64_t period = (raw[1] - d->last_ns) / (raw[0] - d->last_count);
		if (period > 5000000 && period < 50000000) {
			d->period_ns = period;
		}
	}
	d->last_count = raw[0];
	d->last_ns = raw[1];
	uint64_t period = d->period_ns != 0 ? d->period_ns : PERIOD_NS;
	uint64_t lead = vsync_lead_ns();
	if (now_ns() >= raw[1] + period - lead) {
		value[0] = raw[0] + 1;
		value[1] = raw[1] + period - lead;
	} else {
		value[0] = raw[0];
		value[1] = raw[1] - lead;
	}
	return VK_SUCCESS;
}

/*!
 * Shown means a vblank since the client last read the counter: vrcompositor reads it right after
 * the wait and spins for a second if it has not moved since its previous read.
 *
 * With @p blocked, the wait had to wait for its flip. A flip comes after the tick of its vblank, so a
 * frame that starts now would end just before the next flip and be shown only by the one after: a
 * queue that holds every frame a refresh longer, for good. The next tick lets it drain. A job that
 * ran past its tick finds its flip done, and the next frame starts at once: that tick has passed.
 */
static VkResult
wait_for_new_count(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, bool blocked)
{
	uint64_t start = now_ns();
	uint64_t passed = 0;
	bool first = true;
	for (;;) {
		uint64_t value[2] = {0, 0};
		pthread_mutex_lock(&lock);
		struct device *d = find_device(device);
		bool has_read = d != NULL && d->has_read;
		uint64_t read_count = has_read ? d->read_count : 0;
		VkResult result = has_read ? early_counter(d, device, swapchain, value) : VK_SUCCESS;
		pthread_mutex_unlock(&lock);
		if (!has_read || result != VK_SUCCESS) {
			return VK_SUCCESS;
		}
		if (first) {
			passed = blocked && value[0] > read_count ? value[0] : read_count;
			first = false;
		}
		if (value[0] > passed) {
			return VK_SUCCESS;
		}
		if (now_ns() - start >= timeout) {
			return VK_TIMEOUT;
		}
		const struct timespec pause = {0, POLL_NS};
		nanosleep(&pause, NULL);
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL
wait_for_present(VkDevice device, VkSwapchainKHR swapchain, uint64_t present_id, uint64_t timeout)
{
	pthread_mutex_lock(&lock);
	const struct device *d = find_device(device);
	PFN_vkWaitForPresentKHR next = d != NULL ? d->next_wait_for_present : NULL;
	pthread_mutex_unlock(&lock);
	if (next == NULL) {
		return VK_ERROR_DEVICE_LOST;
	}
	uint64_t ahead = frames_ahead();
	if (present_id <= ahead) {
		return wait_for_new_count(device, swapchain, timeout, false);
	}
	VkResult result = next(device, swapchain, present_id - ahead, 0);
	bool blocked = result == VK_TIMEOUT;
	if (blocked && timeout > 0) {
		result = next(device, swapchain, present_id - ahead, timeout);
	}
	return result == VK_SUCCESS ? wait_for_new_count(device, swapchain, timeout, blocked) : result;
}
#ifdef VK_KHR_present_wait2

static VKAPI_ATTR VkResult VKAPI_CALL
wait_for_present2(VkDevice device, VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR *info)
{
	pthread_mutex_lock(&lock);
	const struct device *d = find_device(device);
	PFN_vkWaitForPresent2KHR next = d != NULL ? d->next_wait_for_present2 : NULL;
	pthread_mutex_unlock(&lock);
	if (next == NULL) {
		return VK_ERROR_DEVICE_LOST;
	}
	uint64_t ahead = frames_ahead();
	if (info->presentId <= ahead) {
		return wait_for_new_count(device, swapchain, info->timeout, false);
	}
	VkPresentWait2InfoKHR earlier = *info;
	earlier.presentId = info->presentId - ahead;
	earlier.timeout = 0;
	VkResult result = next(device, swapchain, &earlier);
	bool blocked = result == VK_TIMEOUT;
	if (blocked && info->timeout > 0) {
		earlier.timeout = info->timeout;
		result = next(device, swapchain, &earlier);
	}
	return result == VK_SUCCESS ? wait_for_new_count(device, swapchain, info->timeout, blocked) : result;
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL
get_counter(VkDevice device, VkSwapchainKHR swapchain, VkSurfaceCounterFlagBitsEXT counter, uint64_t *value)
{
	uint64_t early[2] = {0, 0};
	pthread_mutex_lock(&lock);
	struct device *d = find_device(device);
	VkResult result = d != NULL && d->next_get_counter != NULL ? early_counter(d, device, swapchain, early)
	                                                           : VK_ERROR_DEVICE_LOST;
	if (result == VK_SUCCESS) {
		d->read_count = early[0];
		d->has_read = true;
		value[0] = early[0];
		if (counter == SURFACE_COUNTER_TIMESTAMP_VALVE) {
			value[1] = early[1];
		}
	}
	pthread_mutex_unlock(&lock);
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_instance(const VkInstanceCreateInfo *create_info, const VkAllocationCallbacks *allocator, VkInstance *instance)
{
	VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)create_info->pNext;
	while (chain != NULL && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
	                          chain->function == VK_LAYER_LINK_INFO)) {
		chain = (VkLayerInstanceCreateInfo *)chain->pNext;
	}
	if (chain == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	PFN_vkGetInstanceProcAddr next = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance");
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	pthread_mutex_lock(&lock);
	next_get_instance_proc_addr = next;
	pthread_mutex_unlock(&lock);
	return next_create(create_info, allocator, instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_device(VkPhysicalDevice physical_device,
              const VkDeviceCreateInfo *create_info,
              const VkAllocationCallbacks *allocator,
              VkDevice *device)
{
	VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)create_info->pNext;
	while (chain != NULL && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
	                          chain->function == VK_LAYER_LINK_INFO)) {
		chain = (VkLayerDeviceCreateInfo *)chain->pNext;
	}
	if (chain == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	PFN_vkGetInstanceProcAddr next_instance = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_device = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_instance(VK_NULL_HANDLE, "vkCreateDevice");
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkResult result = next_create(physical_device, create_info, allocator, device);
	if (result != VK_SUCCESS) {
		return result;
	}
	pthread_mutex_lock(&lock);
	struct device *d = find_device(VK_NULL_HANDLE);
	if (d != NULL) {
		d->handle = *device;
		d->next_get_device_proc_addr = next_device;
		d->next_create_swapchain = (PFN_vkCreateSwapchainKHR)next_device(*device, "vkCreateSwapchainKHR");
		d->next_wait_for_present = (PFN_vkWaitForPresentKHR)next_device(*device, "vkWaitForPresentKHR");
#ifdef VK_KHR_present_wait2
		d->next_wait_for_present2 = (PFN_vkWaitForPresent2KHR)next_device(*device, "vkWaitForPresent2KHR");
#endif
		d->next_get_counter = (PFN_vkGetSwapchainCounterEXT)next_device(*device, "vkGetSwapchainCounterEXT");
	}
	pthread_mutex_unlock(&lock);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_device(VkDevice device, const VkAllocationCallbacks *allocator)
{
	pthread_mutex_lock(&lock);
	struct device *d = find_device(device);
	PFN_vkDestroyDevice next = NULL;
	if (d != NULL) {
		next = (PFN_vkDestroyDevice)d->next_get_device_proc_addr(device, "vkDestroyDevice");
		if (d->published) {
			lead_publish(false);
		}
		memset(d, 0, sizeof(*d));
	}
	pthread_mutex_unlock(&lock);
	if (next != NULL) {
		next(device, allocator);
	}
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char *name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
get_instance_proc_addr(VkInstance instance, const char *name)
{
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)get_instance_proc_addr;
	}
	if (strcmp(name, "vkCreateInstance") == 0) {
		return (PFN_vkVoidFunction)create_instance;
	}
	if (strcmp(name, "vkCreateDevice") == 0) {
		return (PFN_vkVoidFunction)create_device;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)get_device_proc_addr;
	}
	pthread_mutex_lock(&lock);
	PFN_vkGetInstanceProcAddr next = next_get_instance_proc_addr;
	pthread_mutex_unlock(&lock);
	return next != NULL ? next(instance, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
get_device_proc_addr(VkDevice device, const char *name)
{
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)get_device_proc_addr;
	}
	if (strcmp(name, "vkDestroyDevice") == 0) {
		return (PFN_vkVoidFunction)destroy_device;
	}
	pthread_mutex_lock(&lock);
	const struct device *d = find_device(device);
	PFN_vkGetDeviceProcAddr next = d != NULL ? d->next_get_device_proc_addr : NULL;
	bool has_swapchain = d != NULL && d->next_create_swapchain != NULL;
	bool has_wait = d != NULL && d->next_wait_for_present != NULL;
#ifdef VK_KHR_present_wait2
	bool has_wait2 = d != NULL && d->next_wait_for_present2 != NULL;
#endif
	bool has_counter = d != NULL && d->next_get_counter != NULL;
	pthread_mutex_unlock(&lock);
	if (strcmp(name, "vkCreateSwapchainKHR") == 0 && has_swapchain) {
		return (PFN_vkVoidFunction)create_swapchain;
	}
	if (frames_ahead() > 0) {
		if (strcmp(name, "vkWaitForPresentKHR") == 0 && has_wait) {
			return (PFN_vkVoidFunction)wait_for_present;
		}
#ifdef VK_KHR_present_wait2
		if (strcmp(name, "vkWaitForPresent2KHR") == 0 && has_wait2) {
			return (PFN_vkVoidFunction)wait_for_present2;
		}
#endif
		if (strcmp(name, "vkGetSwapchainCounterEXT") == 0 && has_counter) {
			return (PFN_vkVoidFunction)get_counter;
		}
	}
	return next != NULL ? next(device, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
q2_vr_pacing_negotiate(VkNegotiateLayerInterface *version)
{
	if (version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || version->loaderLayerInterfaceVersion < 2) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	version->loaderLayerInterfaceVersion = 2;
	version->pfnGetInstanceProcAddr = get_instance_proc_addr;
	version->pfnGetDeviceProcAddr = get_device_proc_addr;
	version->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
