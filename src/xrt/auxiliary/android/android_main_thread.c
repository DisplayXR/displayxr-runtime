// Copyright 2026, DisplayXR.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of Android main-thread dispatch (NDK self-pipe + ALooper).
 * @ingroup aux_android
 */

#include "android_main_thread.h"

#ifdef XRT_OS_ANDROID

#include "util/u_logging.h"
#include "android/android_globals.h"

#include <jni.h>

#include <android/looper.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum main_thread_task_state
{
	TASK_PENDING = 0, //!< Posted, not yet picked up by the main thread.
	TASK_RUNNING,     //!< The main thread is inside fn().
	TASK_DONE,        //!< fn() returned.
	TASK_ABANDONED,   //!< The caller timed out before fn() started: never run it.
};

/*!
 * One unit of work posted to the main thread. HEAP-allocated and refcounted
 * (#1007): the caller and the main thread each hold one reference, so a caller
 * that gives up on a timeout can return while the pointer is still sitting in
 * the pipe. A task abandoned while PENDING is skipped (its fn/data belong to a
 * caller frame that is gone); one already RUNNING cannot be abandoned — fn is
 * using the caller's data — so the caller keeps waiting for it.
 */
struct main_thread_task
{
	void (*fn)(void *);
	void *data;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	enum main_thread_task_state state;
	int refs;
};

static void
task_unref(struct main_thread_task *task)
{
	pthread_mutex_lock(&task->lock);
	const int refs = --task->refs;
	pthread_mutex_unlock(&task->lock);
	if (refs == 0) {
		pthread_cond_destroy(&task->cond);
		pthread_mutex_destroy(&task->lock);
		free(task);
	}
}

/*!
 * One Looper we can post tasks to: a self-pipe whose read end is registered on
 * that Looper with an fd callback. Two instances exist — the service main
 * thread (@ref g_dispatch) and the runtime-owned looper thread (@ref g_worker).
 */
struct looper_dispatch
{
	bool initialized;
	ALooper *looper;
	int pipe_read;
	int pipe_write;
	pthread_t thread;
	pthread_mutex_t submit_lock;
};

#define LOOPER_DISPATCH_INIT                                                                                           \
	{                                                                                                              \
		.initialized = false, .looper = NULL, .pipe_read = -1, .pipe_write = -1,                               \
		.submit_lock = PTHREAD_MUTEX_INITIALIZER,                                                              \
	}

//! The Android main thread (service / diag targets only).
static struct looper_dispatch g_dispatch = LOOPER_DISPATCH_INIT;

//! The runtime-owned looper thread (lazily started, process lifetime).
static struct looper_dispatch g_worker = LOOPER_DISPATCH_INIT;

/*!
 * ALooper fd callback — runs on the main thread when the pipe becomes readable.
 * Drains every queued task pointer, runs it, and signals its waiter. Returns 1 to
 * stay registered.
 */
static int
on_pipe_readable(int fd, int events, void *data)
{
	(void)events;
	(void)data;

	struct main_thread_task *task = NULL;
	while (true) {
		ssize_t n = read(fd, &task, sizeof(task));
		if (n == (ssize_t)sizeof(task) && task != NULL) {
			pthread_mutex_lock(&task->lock);
			const bool run = task->state == TASK_PENDING;
			if (run) {
				task->state = TASK_RUNNING;
			}
			pthread_mutex_unlock(&task->lock);

			if (run) {
				task->fn(task->data);
				pthread_mutex_lock(&task->lock);
				task->state = TASK_DONE;
				pthread_cond_signal(&task->cond);
				pthread_mutex_unlock(&task->lock);
			} else {
				U_LOG_W("android_main_thread: skipping a task its caller abandoned (#1007)");
			}
			task_unref(task);
			continue;
		}
		// n <= 0 (EAGAIN/empty) or a short read: nothing more to drain.
		break;
	}
	return 1; // keep the fd registered
}

/*!
 * Register a self-pipe on @p looper (owned by the calling thread) so tasks can
 * be posted to it. Returns false and leaves @p d untouched on failure.
 */
static bool
looper_dispatch_attach(struct looper_dispatch *d, ALooper *looper, const char *name)
{
	int fds[2];
	if (pipe(fds) != 0) {
		U_LOG_E("android_main_thread: %s: pipe() failed: %s", name, strerror(errno));
		return false;
	}

	// Read end non-blocking so the drain loop terminates cleanly on EAGAIN.
	int flags = fcntl(fds[0], F_GETFL, 0);
	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	ALooper_acquire(looper);
	int ret = ALooper_addFd(looper, fds[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_pipe_readable, NULL);
	if (ret != 1) {
		U_LOG_E("android_main_thread: %s: ALooper_addFd failed (%d)", name, ret);
		ALooper_release(looper);
		close(fds[0]);
		close(fds[1]);
		return false;
	}

	d->looper = looper;
	d->pipe_read = fds[0];
	d->pipe_write = fds[1];
	d->thread = pthread_self();
	d->initialized = true;
	return true;
}

void
android_main_thread_dispatch_init(void)
{
	if (g_dispatch.initialized) {
		return;
	}

	ALooper *looper = ALooper_forThread();
	if (looper == NULL) {
		U_LOG_W(
		    "android_main_thread: no Looper on the calling thread; main-thread "
		    "dispatch disabled (callbacks will run on the runtime looper thread)");
		return;
	}

	if (looper_dispatch_attach(&g_dispatch, looper, "main")) {
		U_LOG_W("android_main_thread: main-thread dispatch ready (pipe r=%d w=%d)", g_dispatch.pipe_read,
		        g_dispatch.pipe_write);
	}
}

bool
android_main_thread_dispatch_is_initialized(void)
{
	return g_dispatch.initialized;
}


/*
 *
 * Runtime-owned looper thread (#2490).
 *
 * In-process there is no service JNI entry point to install main-thread
 * dispatch, so a display processor was created on whatever thread asked for
 * it — often an app render thread or the compositor worker, neither of which
 * owns an ALooper. A vendor DP that posts its async init to the calling
 * thread's Looper then never came up, silently. This thread owns an ALooper
 * and pumps it for the life of the process, so such an init (and any later
 * callback the DP posts to the same Looper) always gets serviced.
 *
 */

struct worker_start
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
	bool ok;
};

static void *
looper_worker_main(void *ptr)
{
	struct worker_start *start = (struct worker_start *)ptr;

	pthread_setname_np(pthread_self(), "dxr-dp-looper");

	/*
	 * Attach to the VM if the process published one: a display processor's
	 * init may make JNI calls on this thread. A natively attached thread
	 * resolves classes through the system class loader, so anything that
	 * needs application classes must cache them up front (JNI_OnLoad) — the
	 * same constraint as any other runtime worker thread.
	 */
	JavaVM *vm = (JavaVM *)android_globals_get_vm();
	if (vm != NULL) {
		JNIEnv *env = NULL;
		JavaVMAttachArgs args = {.version = JNI_VERSION_1_6, .name = "dxr-dp-looper", .group = NULL};
		if ((*vm)->AttachCurrentThread(vm, &env, &args) != JNI_OK) {
			U_LOG_W("android_main_thread: looper thread could not attach to the VM; continuing unattached");
		}
	}

	ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
	bool ok = looper != NULL && looper_dispatch_attach(&g_worker, looper, "looper-thread");

	pthread_mutex_lock(&start->lock);
	start->ok = ok;
	start->done = true;
	pthread_cond_signal(&start->cond);
	pthread_mutex_unlock(&start->lock);
	// `start` belongs to the starter's frame: not touched past this point.

	if (!ok) {
		U_LOG_E("android_main_thread: runtime looper thread failed to set up its ALooper");
		return NULL;
	}

	U_LOG_I("android_main_thread: runtime looper thread ready (pipe r=%d w=%d)", g_worker.pipe_read,
	        g_worker.pipe_write);

	// Process lifetime: a display processor may post to this Looper at any
	// time after its init, so it is never torn down.
	for (;;) {
		ALooper_pollOnce(-1, NULL, NULL, NULL);
	}
	return NULL;
}

static pthread_mutex_t g_worker_start_lock = PTHREAD_MUTEX_INITIALIZER;

//! Start the runtime looper thread once. Returns whether it is usable.
static bool
looper_worker_ensure(void)
{
	pthread_mutex_lock(&g_worker_start_lock);
	if (g_worker.initialized) {
		pthread_mutex_unlock(&g_worker_start_lock);
		return true;
	}

	struct worker_start start = {.done = false, .ok = false};
	pthread_mutex_init(&start.lock, NULL);
	pthread_cond_init(&start.cond, NULL);

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int err = pthread_create(&thread, &attr, looper_worker_main, &start);
	pthread_attr_destroy(&attr);

	bool ok = false;
	if (err != 0) {
		U_LOG_E("android_main_thread: pthread_create for the runtime looper thread failed: %s", strerror(err));
	} else {
		pthread_mutex_lock(&start.lock);
		while (!start.done) {
			pthread_cond_wait(&start.cond, &start.lock);
		}
		ok = start.ok;
		pthread_mutex_unlock(&start.lock);
	}

	pthread_cond_destroy(&start.cond);
	pthread_mutex_destroy(&start.lock);
	pthread_mutex_unlock(&g_worker_start_lock);
	return ok;
}

/*!
 * Post @p fn to the Looper behind @p d and wait for it (see the header for the
 * timeout contract). @p d must be initialized and not the calling thread's.
 */
static xrt_result_t
looper_dispatch_run(struct looper_dispatch *d, void (*fn)(void *), void *data, int64_t timeout_ns)
{

	struct main_thread_task *task = calloc(1, sizeof(*task));
	if (task == NULL) {
		U_LOG_E("android_main_thread: out of memory; running inline");
		fn(data);
		return XRT_SUCCESS;
	}
	task->fn = fn;
	task->data = data;
	task->state = TASK_PENDING;
	task->refs = 2; // caller + main thread
	pthread_mutex_init(&task->lock, NULL);
	pthread_condattr_t cattr;
	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	pthread_cond_init(&task->cond, &cattr);
	pthread_condattr_destroy(&cattr);

	// Serialize submissions so two posters can't interleave their pointer writes.
	pthread_mutex_lock(&d->submit_lock);
	ssize_t n = write(d->pipe_write, &task, sizeof(task));
	pthread_mutex_unlock(&d->submit_lock);

	if (n != (ssize_t)sizeof(task)) {
		U_LOG_E("android_main_thread: failed to post task (%zd, %s); running inline", n,
		        strerror(errno));
		task->refs = 1; // the main thread never saw it
		task_unref(task);
		fn(data);
		return XRT_SUCCESS;
	}

	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	if (timeout_ns > 0) {
		int64_t ns = (int64_t)deadline.tv_nsec + timeout_ns;
		deadline.tv_sec += (time_t)(ns / 1000000000LL);
		deadline.tv_nsec = (long)(ns % 1000000000LL);
	}

	xrt_result_t ret = XRT_SUCCESS;
	pthread_mutex_lock(&task->lock);
	while (task->state != TASK_DONE) {
		if (timeout_ns <= 0) {
			pthread_cond_wait(&task->cond, &task->lock);
			continue;
		}
		int err = pthread_cond_timedwait(&task->cond, &task->lock, &deadline);
		if (err != ETIMEDOUT) {
			continue;
		}
		if (task->state == TASK_PENDING) {
			// The main Looper never got to it. Abandon: the main thread
			// will skip it when it finally drains the pipe.
			task->state = TASK_ABANDONED;
			ret = XRT_TIMEOUT;
			U_LOG_E(
			    "android_main_thread: target Looper did not pick up the task within %.0f ms - "
			    "abandoned (#1007)",
			    (double)timeout_ns / 1e6);
			break;
		}
		// RUNNING: fn is using the caller's data, so we cannot leave. Keep
		// waiting, but say so (once per extra timeout period).
		U_LOG_W(
		    "android_main_thread: task still running after %.0f ms (cannot abandon a running "
		    "task; waiting)",
		    (double)timeout_ns / 1e6);
		int64_t ns = (int64_t)deadline.tv_nsec + timeout_ns;
		deadline.tv_sec += (time_t)(ns / 1000000000LL);
		deadline.tv_nsec = (long)(ns % 1000000000LL);
	}
	pthread_mutex_unlock(&task->lock);

	task_unref(task);
	return ret;
}

xrt_result_t
android_run_on_main_thread_timeout(void (*fn)(void *), void *data, int64_t timeout_ns)
{
	if (fn == NULL) {
		return XRT_SUCCESS;
	}

	// Not initialized, or we're already on the main thread → run inline (no
	// marshaling needed, and avoids deadlocking on ourselves).
	if (!g_dispatch.initialized || pthread_equal(pthread_self(), g_dispatch.thread)) {
		fn(data);
		return XRT_SUCCESS;
	}
	return looper_dispatch_run(&g_dispatch, fn, data, timeout_ns);
}

xrt_result_t
android_run_on_looper_thread_timeout(void (*fn)(void *), void *data, int64_t timeout_ns)
{
	if (fn == NULL) {
		return XRT_SUCCESS;
	}

	// 1. Service / diag: the Java main Looper, as before (#510 M2).
	if (g_dispatch.initialized) {
		if (pthread_equal(pthread_self(), g_dispatch.thread)) {
			fn(data);
			return XRT_SUCCESS;
		}
		return looper_dispatch_run(&g_dispatch, fn, data, timeout_ns);
	}

	// Already on the runtime looper thread (nested call): inline.
	if (g_worker.initialized && pthread_equal(pthread_self(), g_worker.thread)) {
		fn(data);
		return XRT_SUCCESS;
	}

	// 2. In-process, and the caller already owns a Looper (native_app_glue's
	//    android_main, an Activity's UI thread): keep running inline there —
	//    that is the configuration that has always worked.
	if (ALooper_forThread() != NULL) {
		fn(data);
		return XRT_SUCCESS;
	}

	// 3. In-process from a thread with no Looper (#2490): hop to the
	//    runtime-owned looper thread.
	if (!looper_worker_ensure()) {
		U_LOG_E(
		    "android_main_thread: no Looper on the calling thread and the runtime looper thread is "
		    "unavailable");
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	U_LOG_I("android_main_thread: calling thread has no ALooper; running on the runtime looper thread");
	return looper_dispatch_run(&g_worker, fn, data, timeout_ns);
}

xrt_result_t
android_run_on_main_thread_blocking(void (*fn)(void *), void *data)
{
	return android_run_on_main_thread_timeout(fn, data, 0);
}

#endif // XRT_OS_ANDROID
