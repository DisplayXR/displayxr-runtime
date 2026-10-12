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

static struct
{
	bool initialized;
	ALooper *looper;
	int pipe_read;
	int pipe_write;
	pthread_t main_thread;
	pthread_mutex_t submit_lock;
} g_dispatch = {
    .initialized = false,
    .looper = NULL,
    .pipe_read = -1,
    .pipe_write = -1,
    .submit_lock = PTHREAD_MUTEX_INITIALIZER,
};

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

void
android_main_thread_dispatch_init(void)
{
	if (g_dispatch.initialized) {
		return;
	}

	ALooper *looper = ALooper_forThread();
	if (looper == NULL) {
		U_LOG_W("android_main_thread: no Looper on the calling thread; main-thread "
		        "dispatch disabled (callbacks will run inline)");
		return;
	}

	int fds[2];
	if (pipe(fds) != 0) {
		U_LOG_E("android_main_thread: pipe() failed: %s", strerror(errno));
		return;
	}

	// Read end non-blocking so the drain loop terminates cleanly on EAGAIN.
	int flags = fcntl(fds[0], F_GETFL, 0);
	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	ALooper_acquire(looper);
	int ret = ALooper_addFd(looper, fds[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
	                        on_pipe_readable, NULL);
	if (ret != 1) {
		U_LOG_E("android_main_thread: ALooper_addFd failed (%d)", ret);
		ALooper_release(looper);
		close(fds[0]);
		close(fds[1]);
		return;
	}

	g_dispatch.looper = looper;
	g_dispatch.pipe_read = fds[0];
	g_dispatch.pipe_write = fds[1];
	g_dispatch.main_thread = pthread_self();
	g_dispatch.initialized = true;

	U_LOG_W("android_main_thread: main-thread dispatch ready (pipe r=%d w=%d)", fds[0], fds[1]);
}

xrt_result_t
android_run_on_main_thread_timeout(void (*fn)(void *), void *data, int64_t timeout_ns)
{
	if (fn == NULL) {
		return XRT_SUCCESS;
	}

	// Not initialized, or we're already on the main thread → run inline (no
	// marshaling needed, and avoids deadlocking on ourselves).
	if (!g_dispatch.initialized || pthread_equal(pthread_self(), g_dispatch.main_thread)) {
		fn(data);
		return XRT_SUCCESS;
	}

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
	pthread_mutex_lock(&g_dispatch.submit_lock);
	ssize_t n = write(g_dispatch.pipe_write, &task, sizeof(task));
	pthread_mutex_unlock(&g_dispatch.submit_lock);

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
			    "android_main_thread: main thread did not pick up the task within %.0f ms - "
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
android_run_on_main_thread_blocking(void (*fn)(void *), void *data)
{
	return android_run_on_main_thread_timeout(fn, data, 0);
}

#endif // XRT_OS_ANDROID
