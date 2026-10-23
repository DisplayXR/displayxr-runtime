// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Child processes, a control channel to one child, cross-process
 *         handle transfer and a latching wake signal (ADR-053 D5: the
 *         conversion-host satellite; roadmap conversion-providers.md §3.4,
 *         §3.5).
 *
 * Implementations: os_process_win32.c (job object with
 * KILL_ON_JOB_CLOSE, a named pipe, DuplicateHandle, auto-reset events) and
 * os_process_posix.c (fork + exec with PR_SET_PDEATHSIG on Linux, a kqueue
 * NOTE_EXIT parent watch on macOS, an AF_UNIX socketpair with SCM_RIGHTS, a
 * socketpair byte as the wake). Not built on Android (its `:conversion`
 * service is a different mechanism, roadmap §4.3).
 *
 * Native handles are widened to uint64_t (a Windows HANDLE, a POSIX fd), as in
 * xrt_conv_sync. OS_NATIVE_HANDLE_INVALID is invalid everywhere; on Windows 0
 * is invalid too.
 *
 * Who holds what (§3.5): the PARENT (the service) holds the child's process
 * handle and so the only cross-process handle right. It pushes handles into
 * the child (os_process_dup_to_child) and pulls the child's handles out
 * (os_process_dup_from_child). The child never receives a handle to the
 * parent process. POSIX has no such right: handles travel both ways as
 * SCM_RIGHTS on the channel (os_channel_send / os_channel_recv), and the
 * dup functions report OS_PROCESS_ERR_UNSUPPORTED.
 *
 * Return convention: 0 = success, OS_PROCESS_ERR_* (< 0) = failure; the wait
 * functions return 1 = done, 0 = timeout, < 0 = failure.
 *
 * This header includes no OS header.
 *
 * @ingroup aux_os
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_NATIVE_HANDLE_INVALID UINT64_MAX

/*! @name Errors @{ */
#define OS_PROCESS_ERR_FAILED (-1)      //!< the OS call failed
#define OS_PROCESS_ERR_INVALID (-2)     //!< bad argument
#define OS_PROCESS_ERR_UNSUPPORTED (-3) //!< not on this platform
#define OS_PROCESS_ERR_CLOSED (-4)      //!< the peer closed the channel / exited
#define OS_PROCESS_ERR_TIMEOUT (-5)     //!< a message started but did not finish in time (stream desynced)
#define OS_PROCESS_ERR_PEER (-6)        //!< the peer is not the expected process
/*! @} */

//! Infinite timeout.
#define OS_PROCESS_WAIT_INFINITE 0xFFFFFFFFu

//! POSIX: the fd the child finds its channel end at.
#define OS_CHANNEL_CHILD_FD 3

//! Exit code reported for a POSIX child killed by a signal it was not sent by os_process_terminate: 128 + signo.
#define OS_PROCESS_EXIT_SIGNAL_BASE 128


/*
 *
 * Processes.
 *
 */

struct os_process;

struct os_process_spawn_info
{
	const char *exe_path;    //!< absolute path, UTF-8
	const char *const *args; //!< arguments AFTER argv[0] (argv[0] is exe_path); UTF-8
	uint32_t arg_count;

	/*!
	 * Kill the child when the parent dies. Windows: the child is assigned to
	 * a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (the job handle is
	 * held by the os_process and dies with the parent; children that break
	 * away are not allowed). Linux: PR_SET_PDEATHSIG(SIGKILL) set between
	 * fork and exec. macOS: nothing parent-side; the child calls
	 * os_process_watch_parent() (kqueue NOTE_EXIT).
	 */
	bool kill_on_parent_exit;

	/*!
	 * POSIX: the child's channel end from os_channel_create_for_child; it is
	 * placed at OS_CHANNEL_CHILD_FD in the child, every other fd >= 3 is
	 * closed. Windows: ignored (the child opens the pipe by name) — pass
	 * OS_NATIVE_HANDLE_INVALID.
	 */
	uint64_t channel_child_handle;

	//! Optional: append the child's stdout + stderr to this file (UTF-8 path). NULL = inherit / discard.
	const char *log_path;
};

//! Start a child. Its stdin is the null device.
int
os_process_spawn(const struct os_process_spawn_info *info, struct os_process **out_proc);

long
os_process_pid(const struct os_process *proc);

/*!
 * Wait for the child to exit. 1 = exited (*out_exit_code set), 0 = timeout.
 * A POSIX child killed by os_process_terminate reports the code passed there;
 * one killed by any other signal reports OS_PROCESS_EXIT_SIGNAL_BASE + signo.
 * May be called again after it returned 1 (same answer).
 */
int
os_process_wait(struct os_process *proc, uint32_t timeout_ms, int32_t *out_exit_code);

//! Kill the child now (TerminateProcess / SIGKILL). The exit it reports is @p exit_code.
int
os_process_terminate(struct os_process *proc, int32_t exit_code);

/*!
 * Release the os_process. Does NOT kill a running child, except that on
 * Windows closing the last job handle kills it when kill_on_parent_exit was
 * set: call it after the child exited (or was terminated and waited for).
 */
void
os_process_destroy(struct os_process **proc_ptr);

/*!
 * Windows: duplicate @p local (a handle of this process) into the child;
 * *out_remote is the value valid in the child. The source stays open.
 * POSIX: OS_PROCESS_ERR_UNSUPPORTED (use SCM_RIGHTS).
 */
int
os_process_dup_to_child(struct os_process *proc, uint64_t local, uint64_t *out_remote);

/*!
 * Windows: PULL @p remote (a handle value valid in the child) into this
 * process; *out_local is ours. @p close_source also closes it in the child.
 * POSIX: OS_PROCESS_ERR_UNSUPPORTED (use SCM_RIGHTS).
 */
int
os_process_dup_from_child(struct os_process *proc, uint64_t remote, bool close_source, uint64_t *out_local);

//! Close a native handle (NULL-/invalid-safe).
void
os_native_handle_close(uint64_t handle);


/*
 *
 * Child-side helpers.
 *
 */

/*!
 * Child: exit (with @p exit_code) when process @p parent_pid exits. Linux:
 * PR_SET_PDEATHSIG(SIGKILL) again (survives exec, but is re-armed here in
 * case the child was not started by os_process_spawn) and a getppid() check
 * for a parent that died before that. macOS: a detached thread waits on a
 * kqueue EVFILT_PROC NOTE_EXIT for the parent and calls _exit. Windows: no-op
 * (the job object kills the child). 0 = armed.
 */
int
os_process_watch_parent(long parent_pid, int32_t exit_code);

//! True when this process runs elevated (Windows: an elevated token; POSIX: euid 0).
bool
os_process_is_elevated(void);

//! The calling process's id.
long
os_process_current_pid(void);

/*!
 * Does process @p pid exist (and has not exited)? Any process, not only a
 * child: Windows OpenProcess + GetExitCodeProcess (access denied = it
 * exists), POSIX kill(pid, 0) (EPERM = it exists). A recycled pid reads as
 * alive — callers pair it with a heartbeat. false for pid <= 0.
 */
bool
os_process_is_alive(long pid);

/*!
 * Windows: true when the named mutex @p name exists and another process holds
 * it (opened with SYNCHRONIZE, probed with a zero wait and released at once
 * if it was free). The maintenance-mutex check of roadmap §3.6. POSIX: false.
 */
bool
os_named_mutex_held(const char *name);


/*
 *
 * Control channel: a reliable byte stream between a parent and ONE child.
 *
 */

struct os_channel;

/*!
 * Parent: create the channel for a child about to be spawned.
 *
 * Windows: a single-instance named pipe `\\.\pipe\<name_hint>-<random>`
 * (FILE_FLAG_FIRST_PIPE_INSTANCE so no one can squat the name,
 * PIPE_REJECT_REMOTE_CLIENTS, the default DACL: the creating user + SYSTEM +
 * administrators); *out_token is the pipe name, *out_child_handle INVALID.
 * POSIX: an AF_UNIX stream socketpair (CLOEXEC); *out_token is
 * "OS_CHANNEL_CHILD_FD" as text and *out_child_handle the child's end, to
 * pass as os_process_spawn_info::channel_child_handle.
 *
 * Pass the token to the child (CONV_HOST_ARG_CHANNEL); then
 * os_channel_accept_child().
 */
int
os_channel_create_for_child(const char *name_hint,
                            struct os_channel **out_parent,
                            char *out_token,
                            size_t token_size,
                            uint64_t *out_child_handle);

/*!
 * Parent, after os_process_spawn: Windows waits up to @p timeout_ms for the
 * child to connect and checks the connected client's pid is @p child's
 * (OS_PROCESS_ERR_PEER otherwise). POSIX: closes the parent's copy of
 * @p child_handle (the child has its own) and returns 0.
 */
int
os_channel_accept_child(struct os_channel *parent,
                        const struct os_process *child,
                        uint64_t child_handle,
                        uint32_t timeout_ms);

//! Child: open the channel named by @p token.
int
os_channel_open_child(const char *token, struct os_channel **out_channel);

/*!
 * Send @p len bytes (all of them, or fail). POSIX: @p handles (fds of this
 * process, borrowed) ride the first byte as SCM_RIGHTS. Windows:
 * @p handle_count must be 0 (handles go through os_process_dup_*).
 * Thread-safe against a concurrent os_channel_recv, not against another send.
 */
int
os_channel_send(struct os_channel *ch, const void *buf, size_t len, const uint64_t *handles, uint32_t handle_count);

/*!
 * Receive exactly @p len bytes. 1 = done; 0 = nothing arrived within
 * @p timeout_ms; OS_PROCESS_ERR_TIMEOUT = a message started but did not
 * complete within a further @p timeout_ms (the stream is desynced: treat as
 * broken); OS_PROCESS_ERR_CLOSED = the peer closed. POSIX: received
 * descriptors go to @p handles (capacity *inout_handle_count on entry, count
 * on return; extra ones are closed); the caller owns them. Windows:
 * *inout_handle_count is set to 0. @p handles may be NULL when
 * inout_handle_count is NULL.
 */
int
os_channel_recv(struct os_channel *ch,
                void *buf,
                size_t len,
                uint64_t *handles,
                uint32_t *inout_handle_count,
                uint32_t timeout_ms);

void
os_channel_destroy(struct os_channel **ch_ptr);


/*
 *
 * Wake signal: one direction, latching (a signal sent before the wait still
 * wakes it; several signals before a wait coalesce into one wake).
 *
 */

struct os_wake
{
	uint64_t native; //!< Windows: auto-reset event; POSIX: one end of a nonblocking socketpair
};

/*!
 * Create a wake object. *local is this process's end; *out_remote_native is
 * the native handle for the OTHER process: Windows the same event (push it
 * with os_process_dup_to_child; do not close it separately — it is
 * local->native), POSIX the other socketpair end (send it with SCM_RIGHTS,
 * then close it here).
 */
int
os_wake_create_pair(struct os_wake *local, uint64_t *out_remote_native);

//! Adopt a received native handle as this process's end (takes ownership).
void
os_wake_from_native(struct os_wake *w, uint64_t native);

int
os_wake_signal(struct os_wake *w);

//! 1 = signalled (drained), 0 = timeout, OS_PROCESS_ERR_CLOSED = the other end is gone (POSIX).
int
os_wake_wait(struct os_wake *w, uint32_t timeout_ms);

void
os_wake_destroy(struct os_wake *w);

#ifdef __cplusplus
}
#endif
