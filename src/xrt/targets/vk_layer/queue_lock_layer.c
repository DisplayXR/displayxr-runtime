// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK_LAYER_DXR_queue_lock — per-queue submit serialization (#902).
 *
 * Makes the #868 late-weave repaint safe on GPUs that expose a single
 * graphics-capable queue (every Intel iGPU, AMD RADV): the repaint thread and
 * the app's render thread must both submit to the same VkQueue, and a VkQueue
 * is externally synchronized — the runtime cannot serialize the app's own
 * vkQueueSubmit from outside the call. This layer puts the lock INSIDE the
 * call: one mutex per VkQueue, held for the duration of each queue-access
 * entry point. Vulkan has no D3D11-style shared immediate-context state, so
 * per-call mutual exclusion is exactly the spec's external-synchronization
 * contract, discharged centrally — no cross-call atomicity is needed
 * (submit→present ordering is carried by semaphores).
 *
 * Coverage is total by construction: dispatchable handles route every caller
 * through the layer chain baked into their dispatch table — the app's engine,
 * the runtime's repaint thread, and the vendor display processor's internal
 * submits alike, regardless of how they resolved the function pointer.
 *
 * The runtime injects this layer at xrCreateVulkanInstanceKHR (enable2 puts it
 * in-path; the app never knows) and detects it by resolving the marker entry
 * point "vkGetQueueLockMarkerDXR" via vkGetDeviceProcAddr — the repaint's
 * shared-queue tier engages only when the marker resolves (handshake, not
 * hope). Design of record: docs/roadmap/vk-late-weave-queue-serialization.md.
 *
 * Deliberately dependency-free: no xrt/aux includes, no link against the
 * Vulkan loader (a layer linking the loader can recurse). Plain C + one
 * OS mutex primitive.
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK ql_mutex_t;
#define QL_MUTEX_INIT SRWLOCK_INIT
static inline void
ql_mutex_lock(ql_mutex_t *m)
{
	AcquireSRWLockExclusive(m);
}
static inline void
ql_mutex_unlock(ql_mutex_t *m)
{
	ReleaseSRWLockExclusive(m);
}
static inline void
ql_mutex_init(ql_mutex_t *m)
{
	InitializeSRWLock(m);
}
#define QL_EXPORT __declspec(dllexport)
#else
#include <pthread.h>
typedef pthread_mutex_t ql_mutex_t;
#define QL_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void
ql_mutex_lock(ql_mutex_t *m)
{
	pthread_mutex_lock(m);
}
static inline void
ql_mutex_unlock(ql_mutex_t *m)
{
	pthread_mutex_unlock(m);
}
static inline void
ql_mutex_init(ql_mutex_t *m)
{
	pthread_mutex_init(m, NULL);
}
#define QL_EXPORT __attribute__((visibility("default")))
#endif

#define QL_LAYER_NAME "VK_LAYER_DXR_queue_lock"
//! Keep in sync with the runtime's handshake string (comp_vk_native_compositor.c, #902).
#define QL_MARKER_NAME "vkGetQueueLockMarkerDXR"

#define QL_MAX_INSTANCES 8
#define QL_MAX_DEVICES 8
#define QL_MAX_QUEUES 16

/*
 * Dispatchable handles all begin with a loader-owned dispatch-table pointer;
 * it is identical for an instance and its physical devices, and for a device
 * and its queues — the standard layer "dispatch key".
 */
static inline void *
ql_key(const void *dispatchable)
{
	return *(void **)dispatchable;
}

struct ql_instance
{
	void *key; // NULL = free slot
	VkInstance instance;
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
};

struct ql_queue
{
	VkQueue queue; // NULL = free slot
	ql_mutex_t mutex;
};

struct ql_device
{
	void *key; // NULL = free slot
	VkDevice device;
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkGetDeviceQueue2 GetDeviceQueue2;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueSubmit2 QueueSubmit2;
	PFN_vkQueueSubmit2KHR QueueSubmit2KHR;
	PFN_vkQueuePresentKHR QueuePresentKHR;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkQueueBindSparse QueueBindSparse;
	PFN_vkQueueBeginDebugUtilsLabelEXT QueueBeginDebugUtilsLabelEXT;
	PFN_vkQueueEndDebugUtilsLabelEXT QueueEndDebugUtilsLabelEXT;
	PFN_vkQueueInsertDebugUtilsLabelEXT QueueInsertDebugUtilsLabelEXT;
	struct ql_queue queues[QL_MAX_QUEUES];
};

static ql_mutex_t g_registry_lock = QL_MUTEX_INIT;
static struct ql_instance g_instances[QL_MAX_INSTANCES];
static struct ql_device g_devices[QL_MAX_DEVICES];

static struct ql_instance *
ql_instance_find(void *key)
{
	for (int i = 0; i < QL_MAX_INSTANCES; i++) {
		if (g_instances[i].key == key) {
			return &g_instances[i];
		}
	}
	return NULL;
}

static struct ql_device *
ql_device_find(void *key)
{
	for (int i = 0; i < QL_MAX_DEVICES; i++) {
		if (g_devices[i].key == key) {
			return &g_devices[i];
		}
	}
	return NULL;
}

/*!
 * Look up (or lazily register) the mutex for @p queue on @p dev.
 *
 * Registration normally happens at vkGetDeviceQueue(2); the lazy path is
 * belt-and-braces for a queue handle we somehow never saw handed out. The
 * registry lock protects the arrays only — the returned per-queue mutex is
 * taken by the caller OUTSIDE the registry lock, so submits on different
 * queues never contend with each other here beyond a pointer lookup.
 */
static ql_mutex_t *
ql_queue_mutex(struct ql_device *dev, VkQueue queue)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_queue *free_slot = NULL;
	for (int i = 0; i < QL_MAX_QUEUES; i++) {
		if (dev->queues[i].queue == queue) {
			ql_mutex_unlock(&g_registry_lock);
			return &dev->queues[i].mutex;
		}
		if (dev->queues[i].queue == NULL && free_slot == NULL) {
			free_slot = &dev->queues[i];
		}
	}
	if (free_slot != NULL) {
		free_slot->queue = queue;
		ql_mutex_init(&free_slot->mutex);
		ql_mutex_unlock(&g_registry_lock);
		return &free_slot->mutex;
	}
	ql_mutex_unlock(&g_registry_lock);
	return NULL; // > QL_MAX_QUEUES distinct queues; caller passes through unlocked.
}

/*
 *
 * Marker (the runtime's handshake).
 *
 */

static VKAPI_ATTR uint32_t VKAPI_CALL
ql_GetQueueLockMarker(void)
{
	return 1;
}

/*
 *
 * Instance chain.
 *
 */

static VkLayerInstanceCreateInfo *
ql_get_instance_chain_info(const VkInstanceCreateInfo *ci)
{
	VkLayerInstanceCreateInfo *info = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (info != NULL && !(info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
	                         info->function == VK_LAYER_LINK_INFO)) {
		info = (VkLayerInstanceCreateInfo *)info->pNext;
	}
	return info;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkInstance *pInstance)
{
	VkLayerInstanceCreateInfo *chain = ql_get_instance_chain_info(pCreateInfo);
	if (chain == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// Advance the chain for the next layer down.
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS) {
		return res;
	}

	ql_mutex_lock(&g_registry_lock);
	struct ql_instance *inst = ql_instance_find(NULL); // free slot
	if (inst != NULL) {
		inst->key = ql_key(*pInstance);
		inst->instance = *pInstance;
		inst->gipa = next_gipa;
		inst->DestroyInstance = (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
		inst->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(
		    *pInstance, "vkEnumerateDeviceExtensionProperties");
	}
	ql_mutex_unlock(&g_registry_lock);

	return res;
}

static VKAPI_ATTR void VKAPI_CALL
ql_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_instance *inst = ql_instance_find(ql_key(instance));
	PFN_vkDestroyInstance down = NULL;
	if (inst != NULL) {
		down = inst->DestroyInstance;
		memset(inst, 0, sizeof(*inst));
	}
	ql_mutex_unlock(&g_registry_lock);
	if (down != NULL) {
		down(instance, pAllocator);
	}
}

/*
 *
 * Device chain.
 *
 */

static VkLayerDeviceCreateInfo *
ql_get_device_chain_info(const VkDeviceCreateInfo *ci)
{
	VkLayerDeviceCreateInfo *info = (VkLayerDeviceCreateInfo *)ci->pNext;
	while (info != NULL &&
	       !(info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && info->function == VK_LAYER_LINK_INFO)) {
		info = (VkLayerDeviceCreateInfo *)info->pNext;
	}
	return info;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkDevice *pDevice)
{
	VkLayerDeviceCreateInfo *chain = ql_get_device_chain_info(pCreateInfo);
	if (chain == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	// A physical device shares its dispatch key with its instance.
	ql_mutex_lock(&g_registry_lock);
	struct ql_instance *inst = ql_instance_find(ql_key(physicalDevice));
	VkInstance instance = inst != NULL ? inst->instance : NULL;
	ql_mutex_unlock(&g_registry_lock);

	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(instance, "vkCreateDevice");
	if (next_create == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// Advance the chain for the next layer down.
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS) {
		return res;
	}

	VkDevice dev = *pDevice;
	ql_mutex_lock(&g_registry_lock);
	struct ql_device *d = ql_device_find(NULL); // free slot
	if (d != NULL) {
		memset(d, 0, sizeof(*d));
		d->key = ql_key(dev);
		d->device = dev;
		d->gdpa = next_gdpa;
		d->DestroyDevice = (PFN_vkDestroyDevice)next_gdpa(dev, "vkDestroyDevice");
		d->GetDeviceQueue = (PFN_vkGetDeviceQueue)next_gdpa(dev, "vkGetDeviceQueue");
		d->GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)next_gdpa(dev, "vkGetDeviceQueue2");
		d->QueueSubmit = (PFN_vkQueueSubmit)next_gdpa(dev, "vkQueueSubmit");
		d->QueueSubmit2 = (PFN_vkQueueSubmit2)next_gdpa(dev, "vkQueueSubmit2");
		d->QueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)next_gdpa(dev, "vkQueueSubmit2KHR");
		d->QueuePresentKHR = (PFN_vkQueuePresentKHR)next_gdpa(dev, "vkQueuePresentKHR");
		d->QueueWaitIdle = (PFN_vkQueueWaitIdle)next_gdpa(dev, "vkQueueWaitIdle");
		d->QueueBindSparse = (PFN_vkQueueBindSparse)next_gdpa(dev, "vkQueueBindSparse");
		d->QueueBeginDebugUtilsLabelEXT =
		    (PFN_vkQueueBeginDebugUtilsLabelEXT)next_gdpa(dev, "vkQueueBeginDebugUtilsLabelEXT");
		d->QueueEndDebugUtilsLabelEXT =
		    (PFN_vkQueueEndDebugUtilsLabelEXT)next_gdpa(dev, "vkQueueEndDebugUtilsLabelEXT");
		d->QueueInsertDebugUtilsLabelEXT =
		    (PFN_vkQueueInsertDebugUtilsLabelEXT)next_gdpa(dev, "vkQueueInsertDebugUtilsLabelEXT");
	}
	ql_mutex_unlock(&g_registry_lock);

	return res;
}

static VKAPI_ATTR void VKAPI_CALL
ql_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_device *d = ql_device_find(ql_key(device));
	PFN_vkDestroyDevice down = NULL;
	if (d != NULL) {
		down = d->DestroyDevice;
		memset(d, 0, sizeof(*d));
	}
	ql_mutex_unlock(&g_registry_lock);
	if (down != NULL) {
		down(device, pAllocator);
	}
}

static VKAPI_ATTR void VKAPI_CALL
ql_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_device *d = ql_device_find(ql_key(device));
	ql_mutex_unlock(&g_registry_lock);
	if (d == NULL) {
		return;
	}
	d->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
	if (*pQueue != NULL) {
		(void)ql_queue_mutex(d, *pQueue); // register
	}
}

static VKAPI_ATTR void VKAPI_CALL
ql_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_device *d = ql_device_find(ql_key(device));
	ql_mutex_unlock(&g_registry_lock);
	if (d == NULL || d->GetDeviceQueue2 == NULL) {
		return;
	}
	d->GetDeviceQueue2(device, pQueueInfo, pQueue);
	if (*pQueue != NULL) {
		(void)ql_queue_mutex(d, *pQueue); // register
	}
}

/*
 *
 * The locked queue-access surface. One shape: look up device by the queue's
 * dispatch key, take the per-queue mutex across the down-chain call.
 *
 */

static struct ql_device *
ql_device_of_queue(VkQueue queue)
{
	ql_mutex_lock(&g_registry_lock);
	struct ql_device *d = ql_device_find(ql_key(queue));
	ql_mutex_unlock(&g_registry_lock);
	return d;
}

#define QL_LOCKED_CALL(queue, expr_down)                                                                               \
	do {                                                                                                           \
		struct ql_device *_d = ql_device_of_queue(queue);                                                      \
		if (_d == NULL) {                                                                                      \
			return VK_ERROR_DEVICE_LOST;                                                                   \
		}                                                                                                      \
		ql_mutex_t *_m = ql_queue_mutex(_d, queue);                                                            \
		if (_m != NULL) {                                                                                      \
			ql_mutex_lock(_m);                                                                             \
		}                                                                                                      \
		VkResult _res = (expr_down);                                                                           \
		if (_m != NULL) {                                                                                      \
			ql_mutex_unlock(_m);                                                                           \
		}                                                                                                      \
		return _res;                                                                                           \
	} while (0)

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	QL_LOCKED_CALL(queue, _d->QueueSubmit(queue, submitCount, pSubmits, fence));
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence)
{
	QL_LOCKED_CALL(queue, _d->QueueSubmit2(queue, submitCount, pSubmits, fence));
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence)
{
	QL_LOCKED_CALL(queue, _d->QueueSubmit2KHR(queue, submitCount, pSubmits, fence));
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	QL_LOCKED_CALL(queue, _d->QueuePresentKHR(queue, pPresentInfo));
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueueWaitIdle(VkQueue queue)
{
	QL_LOCKED_CALL(queue, _d->QueueWaitIdle(queue));
}

static VKAPI_ATTR VkResult VKAPI_CALL
ql_QueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo, VkFence fence)
{
	QL_LOCKED_CALL(queue, _d->QueueBindSparse(queue, bindInfoCount, pBindInfo, fence));
}

static VKAPI_ATTR void VKAPI_CALL
ql_QueueBeginDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT *pLabelInfo)
{
	struct ql_device *d = ql_device_of_queue(queue);
	if (d == NULL || d->QueueBeginDebugUtilsLabelEXT == NULL) {
		return;
	}
	ql_mutex_t *m = ql_queue_mutex(d, queue);
	if (m != NULL) {
		ql_mutex_lock(m);
	}
	d->QueueBeginDebugUtilsLabelEXT(queue, pLabelInfo);
	if (m != NULL) {
		ql_mutex_unlock(m);
	}
}

static VKAPI_ATTR void VKAPI_CALL
ql_QueueEndDebugUtilsLabelEXT(VkQueue queue)
{
	struct ql_device *d = ql_device_of_queue(queue);
	if (d == NULL || d->QueueEndDebugUtilsLabelEXT == NULL) {
		return;
	}
	ql_mutex_t *m = ql_queue_mutex(d, queue);
	if (m != NULL) {
		ql_mutex_lock(m);
	}
	d->QueueEndDebugUtilsLabelEXT(queue);
	if (m != NULL) {
		ql_mutex_unlock(m);
	}
}

static VKAPI_ATTR void VKAPI_CALL
ql_QueueInsertDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT *pLabelInfo)
{
	struct ql_device *d = ql_device_of_queue(queue);
	if (d == NULL || d->QueueInsertDebugUtilsLabelEXT == NULL) {
		return;
	}
	ql_mutex_t *m = ql_queue_mutex(d, queue);
	if (m != NULL) {
		ql_mutex_lock(m);
	}
	d->QueueInsertDebugUtilsLabelEXT(queue, pLabelInfo);
	if (m != NULL) {
		ql_mutex_unlock(m);
	}
}

/*
 *
 * Proc-addr dispatch.
 *
 */

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName);

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);

/*!
 * For an intercepted device entry point: return our wrapper only when the
 * down-chain actually implements the function — a wrapper over NULL would
 * advertise an extension entry point the device doesn't have.
 */
static PFN_vkVoidFunction
ql_device_intercept(struct ql_device *d, const char *pName)
{
	if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	}
	if (strcmp(pName, "vkDestroyDevice") == 0) {
		return (PFN_vkVoidFunction)ql_DestroyDevice;
	}
	if (strcmp(pName, "vkGetDeviceQueue") == 0) {
		return (PFN_vkVoidFunction)ql_GetDeviceQueue;
	}
	if (strcmp(pName, "vkGetDeviceQueue2") == 0) {
		return (d == NULL || d->GetDeviceQueue2 != NULL) ? (PFN_vkVoidFunction)ql_GetDeviceQueue2 : NULL;
	}
	if (strcmp(pName, "vkQueueSubmit") == 0) {
		return (PFN_vkVoidFunction)ql_QueueSubmit;
	}
	if (strcmp(pName, "vkQueueSubmit2") == 0) {
		return (d == NULL || d->QueueSubmit2 != NULL) ? (PFN_vkVoidFunction)ql_QueueSubmit2 : NULL;
	}
	if (strcmp(pName, "vkQueueSubmit2KHR") == 0) {
		return (d == NULL || d->QueueSubmit2KHR != NULL) ? (PFN_vkVoidFunction)ql_QueueSubmit2KHR : NULL;
	}
	if (strcmp(pName, "vkQueuePresentKHR") == 0) {
		return (d == NULL || d->QueuePresentKHR != NULL) ? (PFN_vkVoidFunction)ql_QueuePresentKHR : NULL;
	}
	if (strcmp(pName, "vkQueueWaitIdle") == 0) {
		return (PFN_vkVoidFunction)ql_QueueWaitIdle;
	}
	if (strcmp(pName, "vkQueueBindSparse") == 0) {
		return (d == NULL || d->QueueBindSparse != NULL) ? (PFN_vkVoidFunction)ql_QueueBindSparse : NULL;
	}
	if (strcmp(pName, "vkQueueBeginDebugUtilsLabelEXT") == 0) {
		return (d == NULL || d->QueueBeginDebugUtilsLabelEXT != NULL)
		           ? (PFN_vkVoidFunction)ql_QueueBeginDebugUtilsLabelEXT
		           : NULL;
	}
	if (strcmp(pName, "vkQueueEndDebugUtilsLabelEXT") == 0) {
		return (d == NULL || d->QueueEndDebugUtilsLabelEXT != NULL)
		           ? (PFN_vkVoidFunction)ql_QueueEndDebugUtilsLabelEXT
		           : NULL;
	}
	if (strcmp(pName, "vkQueueInsertDebugUtilsLabelEXT") == 0) {
		return (d == NULL || d->QueueInsertDebugUtilsLabelEXT != NULL)
		           ? (PFN_vkVoidFunction)ql_QueueInsertDebugUtilsLabelEXT
		           : NULL;
	}
	if (strcmp(pName, QL_MARKER_NAME) == 0) {
		return (PFN_vkVoidFunction)ql_GetQueueLockMarker;
	}
	return NULL;
}

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	struct ql_device *d = NULL;
	if (device != NULL) {
		ql_mutex_lock(&g_registry_lock);
		d = ql_device_find(ql_key(device));
		ql_mutex_unlock(&g_registry_lock);
	}

	PFN_vkVoidFunction fn = ql_device_intercept(d, pName);
	if (fn != NULL) {
		return fn;
	}

	if (d == NULL || d->gdpa == NULL) {
		return NULL;
	}
	return d->gdpa(device, pName);
}

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	}
	if (strcmp(pName, "vkCreateInstance") == 0) {
		return (PFN_vkVoidFunction)ql_CreateInstance;
	}
	if (strcmp(pName, "vkDestroyInstance") == 0) {
		return (PFN_vkVoidFunction)ql_DestroyInstance;
	}
	if (strcmp(pName, "vkCreateDevice") == 0) {
		return (PFN_vkVoidFunction)ql_CreateDevice;
	}

	// Device-level names must also resolve through GIPA (loader contract).
	// No device data here — offer the wrapper unconditionally; the real
	// availability check happens at vkGetDeviceProcAddr time.
	PFN_vkVoidFunction fn = ql_device_intercept(NULL, pName);
	if (fn != NULL) {
		return fn;
	}

	if (instance == NULL) {
		return NULL;
	}

	ql_mutex_lock(&g_registry_lock);
	struct ql_instance *inst = ql_instance_find(ql_key(instance));
	ql_mutex_unlock(&g_registry_lock);
	if (inst == NULL || inst->gipa == NULL) {
		return NULL;
	}
	return inst->gipa(instance, pName);
}

/*
 *
 * Loader negotiation (layer interface v2).
 *
 */

QL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct == NULL || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	pVersionStruct->loaderLayerInterfaceVersion = 2;
	pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
