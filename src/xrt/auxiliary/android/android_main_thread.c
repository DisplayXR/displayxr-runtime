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
#include <string.h>
#include <unistd.h>

/*!
 * One unit of work posted to the main thread. Allocated by the caller (on its
 * stack); its pointer is written down the pipe. The caller blocks on @ref cond
 * until the main-thread callback sets @ref done.
 */
struct main_thread_task
{
	void (*fn)(void *);
	void *data;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
};

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
			task->fn(task->data);

			pthread_mutex_lock(&task->lock);
			task->done = true;
			pthread_cond_signal(&task->cond);
			pthread_mutex_unlock(&task->lock);
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
android_run_on_main_thread_blocking(void (*fn)(void *), void *data)
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

	struct main_thread_task task = {
	    .fn = fn,
	    .data = data,
	    .lock = PTHREAD_MUTEX_INITIALIZER,
	    .cond = PTHREAD_COND_INITIALIZER,
	    .done = false,
	};
	struct main_thread_task *task_ptr = &task;

	// Serialize submissions so two posters can't interleave their pointer writes.
	pthread_mutex_lock(&g_dispatch.submit_lock);
	ssize_t n = write(g_dispatch.pipe_write, &task_ptr, sizeof(task_ptr));
	pthread_mutex_unlock(&g_dispatch.submit_lock);

	if (n != (ssize_t)sizeof(task_ptr)) {
		U_LOG_E("android_main_thread: failed to post task (%zd, %s); running inline", n,
		        strerror(errno));
		fn(data);
		return XRT_SUCCESS;
	}

	pthread_mutex_lock(&task.lock);
	while (!task.done) {
		pthread_cond_wait(&task.cond, &task.lock);
	}
	pthread_mutex_unlock(&task.lock);

	return XRT_SUCCESS;
}

#endif // XRT_OS_ANDROID
