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
 *
 * Lock SCOPE (#1905). Desktop default is one mutex per VkQueue (the #902
 * external-synchronization contract). Android defaults to ONE mutex per
 * VkDevice: on Adreno 740 the app's queue (family 0 idx 0) and the runtime's
 * repaint queue (idx 1) share one kernel GSL context, and two concurrent
 * submits on DIFFERENT queues corrupt its timestamp ("Adreno-GSL ctx N: next
 * client ts ... must be greater"; the next idx-0 submit then fails with -3).
 * Per-queue locking cannot see that; per-device locking serializes every
 * vkQueue* call in the process across both queues. Override with
 * -DQL_DEFAULT_PER_DEVICE=0|1 at build time, and on Android at run time with
 * `setprop debug.dxr.ql_scope queue|device` (read at vkCreateDevice) — the
 * A/B knob for the device experiment.
 *
 * Android loader contract (frameworks/native vulkan/libvulkan/
 * layers_extensions.cpp + api.cpp, verified against AOSP main): no JSON
 * manifest and no vkNegotiateLoaderLayerInterfaceVersion — the loader scans
 * its layer search path for files named libVkLayer*.so, dlsym()s
 * vkEnumerateInstanceLayerProperties + vkEnumerateInstanceExtensionProperties
 * (required; the library is rejected without them) and
 * vkEnumerateDeviceLayerProperties + vkEnumerateDeviceExtensionProperties
 * ("optional" — but a layer whose device-layer properties don't memcmp-equal
 * its instance-layer properties is NOT "global" and is left out of every
 * DEVICE chain, so for us they are mandatory), then resolves
 * "<layerName>GetInstanceProcAddr" falling back to "vkGetInstanceProcAddr"
 * (same for GetDeviceProcAddr). The NDK ships no vk_layer.h; the chain-link
 * structs below are Android's vk_layer_interface.h layout (its instance link
 * has no pfnNextGetPhysicalDeviceProcAddr — we never touch that field).
 */

/*
 * VK_NO_PROTOTYPES: this layer calls NO Vulkan function directly (everything
 * goes through chained pointers), and on Windows vulkan.h declares
 * vkGetInstanceProcAddr/vkGetDeviceProcAddr as __declspec(dllimport) — which
 * clashes (MSVC C2375) with this file DEFINING those very entry points.
 */
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>

// Android's vk_layer_interface.h (frameworks/native/vulkan/include/vulkan/),
// which the NDK does not ship. Layout-identical to the Khronos vk_layer.h for
// every field this file reads.
typedef enum VkLayerFunction_
{
	VK_LAYER_LINK_INFO = 0,
	VK_LOADER_DATA_CALLBACK = 1,
} VkLayerFunction;

typedef struct VkLayerInstanceLink_
{
	struct VkLayerInstanceLink_ *pNext;
	PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
} VkLayerInstanceLink;

typedef VkResult(VKAPI_PTR *PFN_vkSetInstanceLoaderData)(VkInstance instance, void *object);
typedef VkResult(VKAPI_PTR *PFN_vkSetDeviceLoaderData)(VkDevice device, void *object);

typedef struct
{
	VkStructureType sType; // VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
	const void *pNext;
	VkLayerFunction function;
	union {
		VkLayerInstanceLink *pLayerInfo;
		PFN_vkSetInstanceLoaderData pfnSetInstanceLoaderData;
	} u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_
{
	struct VkLayerDeviceLink_ *pNext;
	PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct
{
	VkStructureType sType; // VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
	const void *pNext;
	VkLayerFunction function;
	union {
		VkLayerDeviceLink *pLayerInfo;
		PFN_vkSetDeviceLoaderData pfnSetDeviceLoaderData;
	} u;
} VkLayerDeviceCreateInfo;

#define QL_LOG_TAG "DXR-queue-lock"
#define QL_LOGW(...) __android_log_print(ANDROID_LOG_WARN, QL_LOG_TAG, __VA_ARGS__)
#else
#include <vulkan/vk_layer.h>
#endif

//! Default lock scope: per-DEVICE on Android (#1905, Adreno shared GSL
//! context), per-QUEUE elsewhere (#902). See the file header.
#ifndef QL_DEFAULT_PER_DEVICE
#ifdef __ANDROID__
#define QL_DEFAULT_PER_DEVICE 1
#else
#define QL_DEFAULT_PER_DEVICE 0
#endif
#endif

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
/*
 * No __declspec(dllexport): vk_layer.h declares
 * vkNegotiateLoaderLayerInterfaceVersion with plain linkage, and MSVC rejects
 * a dllexport redefinition (C2375). Exports come from the .def file instead
 * (VkLayer_DXR_queue_lock.def) — PE has no ELF-style interposition, so plain
 * exports are safe here.
 */
#define QL_EXPORT
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
	//! #1905: when set, every queue of this device shares device_mutex.
	bool per_device;
	ql_mutex_t device_mutex;
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
	if (dev->per_device) {
		// #1905: one lock for every queue of the device; the per-queue
		// slots stay unused in this mode.
		return &dev->device_mutex;
	}
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

/*!
 * Resolve the lock scope for a new device: the compile-time default, which
 * Android lets `debug.dxr.ql_scope` (queue|device) override at run time.
 */
static bool
ql_scope_per_device(void)
{
	bool per_device = QL_DEFAULT_PER_DEVICE != 0;
#ifdef __ANDROID__
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.dxr.ql_scope", v) > 0) {
		if (strcmp(v, "queue") == 0) {
			per_device = false;
		} else if (strcmp(v, "device") == 0) {
			per_device = true;
		}
	}
#endif
	return per_device;
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

#ifdef __ANDROID__
	// One-shot proof-of-load for the #1905 device experiment.
	static int s_logged = 0;
	if (!s_logged) {
		s_logged = 1;
		QL_LOGW("VK_LAYER_DXR_queue_lock: loaded into vkCreateInstance (instance %p, default scope %s)",
		        (void *)*pInstance, QL_DEFAULT_PER_DEVICE ? "per-device" : "per-queue");
	}
#endif

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
		d->per_device = ql_scope_per_device();
		ql_mutex_init(&d->device_mutex);
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
	bool per_device = d != NULL && d->per_device;
	ql_mutex_unlock(&g_registry_lock);

#ifdef __ANDROID__
	QL_LOGW("VK_LAYER_DXR_queue_lock: device %p registered, lock scope %s", (void *)dev,
	        d == NULL ? "NONE (registry full - calls pass through UNLOCKED)"
	                  : (per_device ? "per-device (all queues serialized)" : "per-queue"));
#else
	(void)per_device;
#endif

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
 * TRAP (found the hard way, on-box deadlock): the implementations MUST be
 * static, with the exported vkGetInstanceProcAddr/vkGetDeviceProcAddr as thin
 * wrappers. `vkGetInstanceProcAddr` is a global default-visibility symbol that
 * libvulkan.so.1 ALSO exports — and the app loads libvulkan first, so ELF
 * interposition binds a non-static internal reference (including taking the
 * function's address in the negotiate struct) to the LOADER's export, not
 * ours. The loader then believes the layer's GIPA is its own public one,
 * resolves the "first layer CreateInstance" to the public vkCreateInstance,
 * and re-enters itself under its global lock — a self-deadlock inside
 * vkCreateInstance. A static function's address is immune to interposition.
 * (CMake adds -Bsymbolic as belt and braces on ELF.)
 */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ql_GetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ql_GetInstanceProcAddr(VkInstance instance, const char *pName);

/*!
 * For an intercepted device entry point: return our wrapper only when the
 * down-chain actually implements the function — a wrapper over NULL would
 * advertise an extension entry point the device doesn't have.
 */
static PFN_vkVoidFunction
ql_device_intercept(struct ql_device *d, const char *pName)
{
	if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)ql_GetDeviceProcAddr;
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

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ql_GetDeviceProcAddr(VkDevice device, const char *pName)
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

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ql_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
	if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)ql_GetInstanceProcAddr;
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
 * Exports. The manifest-declared entry points are thin wrappers over the
 * static implementations — see the interposition TRAP note above; the
 * loader must receive pointers that can only ever be OURS.
 *
 */

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	return ql_GetInstanceProcAddr(instance, pName);
}

QL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	return ql_GetDeviceProcAddr(device, pName);
}

#ifndef __ANDROID__
/*
 *
 * Loader negotiation (layer interface v2) — desktop Khronos loader only; the
 * Android loader never calls it (it has no negotiation step).
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
	pVersionStruct->pfnGetInstanceProcAddr = ql_GetInstanceProcAddr;
	pVersionStruct->pfnGetDeviceProcAddr = ql_GetDeviceProcAddr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
#endif // !__ANDROID__

#ifdef __ANDROID__
/*
 *
 * Android loader introspection exports (#1905). See the file header for the
 * contract. One layer, no extensions; instance and device properties are the
 * SAME struct so the loader classes us "global" and chains us into devices.
 *
 */

static void
ql_layer_properties(VkLayerProperties *p)
{
	memset(p, 0, sizeof(*p));
	strncpy(p->layerName, QL_LAYER_NAME, sizeof(p->layerName) - 1);
	p->specVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);
	p->implementationVersion = 1;
	strncpy(p->description, "DisplayXR queue-submit serialization (#902/#1905)", sizeof(p->description) - 1);
}

static VkResult
ql_enumerate_layer(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	if (pProperties == NULL) {
		*pPropertyCount = 1;
		return VK_SUCCESS;
	}
	if (*pPropertyCount < 1) {
		return VK_INCOMPLETE;
	}
	ql_layer_properties(&pProperties[0]);
	*pPropertyCount = 1;
	return VK_SUCCESS;
}

QL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	return ql_enumerate_layer(pPropertyCount, pProperties);
}

QL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                 uint32_t *pPropertyCount,
                                 VkLayerProperties *pProperties)
{
	(void)physicalDevice;
	return ql_enumerate_layer(pPropertyCount, pProperties);
}

QL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties)
{
	(void)pProperties;
	if (pLayerName != NULL && strcmp(pLayerName, QL_LAYER_NAME) == 0) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}
	return VK_ERROR_LAYER_NOT_PRESENT;
}

QL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                     const char *pLayerName,
                                     uint32_t *pPropertyCount,
                                     VkExtensionProperties *pProperties)
{
	if (pLayerName != NULL && strcmp(pLayerName, QL_LAYER_NAME) == 0) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}
	// Not about us: the Android loader only ever asks with our name (and a
	// NULL physical device at discovery), but follow the layer convention
	// and pass anything else down the instance chain.
	if (physicalDevice == NULL) {
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	ql_mutex_lock(&g_registry_lock);
	struct ql_instance *inst = ql_instance_find(ql_key(physicalDevice));
	PFN_vkEnumerateDeviceExtensionProperties down = inst != NULL ? inst->EnumerateDeviceExtensionProperties : NULL;
	ql_mutex_unlock(&g_registry_lock);
	if (down == NULL) {
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	return down(physicalDevice, pLayerName, pPropertyCount, pProperties);
}
#endif // __ANDROID__
