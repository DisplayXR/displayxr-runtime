// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common server side code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"

#include "util/u_logging.h"

#include "shared/ipc_protocol.h"
#include "shared/ipc_message_channel.h"

#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Logging
 *
 */

#define IPC_TRACE(d, ...) U_LOG_IFL_T(d->log_level, __VA_ARGS__)
#define IPC_DEBUG(d, ...) U_LOG_IFL_D(d->log_level, __VA_ARGS__)
#define IPC_INFO(d, ...) U_LOG_IFL_I(d->log_level, __VA_ARGS__)
#define IPC_WARN(d, ...) U_LOG_IFL_W(d->log_level, __VA_ARGS__)
#define IPC_ERROR(d, ...) U_LOG_IFL_E(d->log_level, __VA_ARGS__)

#define IPC_CHK_AND_RET(S, ...) U_LOG_CHK_AND_RET((S)->log_level, __VA_ARGS__)
#define IPC_CHK_WITH_GOTO(S, ...) U_LOG_CHK_WITH_GOTO((S)->log_level, __VA_ARGS__)
#define IPC_CHK_WITH_RET(S, ...) U_LOG_CHK_WITH_RET((S)->log_level, __VA_ARGS__)
#define IPC_CHK_ONLY_PRINT(S, ...) U_LOG_CHK_ONLY_PRINT((S)->log_level, __VA_ARGS__)
#define IPC_CHK_ALWAYS_RET(S, ...) U_LOG_CHK_ALWAYS_RET((S)->log_level, __VA_ARGS__)


/*
 *
 * Structs
 *
 */

#define IPC_MAX_CLIENT_SEMAPHORES 8
#define IPC_MAX_CLIENT_SWAPCHAINS (XRT_MAX_LAYERS * 2)
#define IPC_MAX_CLIENT_SPACES 128

struct xrt_instance;
struct ipc_server_stereo_camera;
struct xrt_compositor;
struct xrt_compositor_native;


/*!
 * Information about a single swapchain.
 *
 * @ingroup ipc_server
 */
struct ipc_swapchain_data
{
	uint32_t width;
	uint32_t height;
	uint64_t format;
	uint32_t image_count;

	bool active;
};

/*!
 * Holds the state for a single client.
 *
 * @ingroup ipc_server
 */
struct ipc_client_state
{
	//! Link back to the main server.
	struct ipc_server *server;

	//! Session for this client.
	struct xrt_session *xs;

	//! Compositor for this client.
	struct xrt_compositor *xc;

	//! Is the inputs and outputs active.
	bool io_active;

	//! Number of swapchains in use by client
	uint32_t swapchain_count;

	//! Ptrs to the swapchains
	struct xrt_swapchain *xscs[IPC_MAX_CLIENT_SWAPCHAINS];

	//! Data for the swapchains.
	struct ipc_swapchain_data swapchain_data[IPC_MAX_CLIENT_SWAPCHAINS];

	//! Number of compositor semaphores in use by client
	uint32_t compositor_semaphore_count;

	//! Ptrs to the semaphores.
	struct xrt_compositor_semaphore *xcsems[IPC_MAX_CLIENT_SEMAPHORES];

	struct
	{
		uint32_t root;
		uint32_t local;
		uint32_t stage;
		uint32_t unbounded;
	} semantic_spaces;

	//! Number of spaces.
	uint32_t space_count;
	//! Index of localspace in ipc client.
	uint32_t local_space_index;
	//! Index of localspace in space overseer.
	uint32_t local_space_overseer_index;
	//! Index of localfloorspace in ipc client.
	uint32_t local_floor_space_index;
	//! Index of localfloorspace in space overseer.
	uint32_t local_floor_space_overseer_index;

	//! Ptrs to the spaces.
	struct xrt_space *xspcs[IPC_MAX_CLIENT_SPACES];

	//! Which of the references spaces is the client using.
	bool ref_space_used[XRT_SPACE_REFERENCE_TYPE_COUNT];

	//! Which of the device features is the client using.
	bool device_feature_used[XRT_DEVICE_FEATURE_MAX_ENUM];

	//! Socket fd used for client comms
	struct ipc_message_channel imc;

	struct ipc_app_state client_state;


	uint64_t plane_detection_size;
	uint64_t plane_detection_count;

	//! Array of plane detection ids with plane_detection_size entries.
	uint64_t *plane_detection_ids;

	//! Array of xrt_devices with plane_detection_size entries.
	struct xrt_device **plane_detection_xdev;

	int server_thread_index;

	//! Set as the client thread's last act (after common_shutdown), so
	//! the server can wait for it without blocking its main thread (#1815).
	bool thread_done;

	xrt_shmem_handle_t ism_handle;

	//! #954: OS-derived peer identity, set once at accept. peer_pid is the
	//! authoritative PID (client_state.pid mirrors it); 0 = could not derive
	//! (privileged gates must fail closed). peer_create_ns defends against PID
	//! reuse (Windows; 0 elsewhere / low-integrity peer).
	long peer_pid;
	uint64_t peer_create_ns;

	//! #960: set once describe_client has verified client_state.client_class.
	//! Until then the client counts against no quota and holds no privilege.
	bool class_verified;

	//! browser#103 RC-1: the OS-derived pid of the process that OPENED this
	//! connection. Equal to peer_pid until a peer declaration is accepted, after
	//! which peer_pid becomes the DECLARED target and this stays the authoriser.
	long opener_pid;

	//! browser#103 RC-1: a peer declaration has been settled on this connection.
	//! Settled ONCE — a second declaration is refused, so no client can move its
	//! identity after gates have run against it.
	bool peer_declared;

	//! browser#103 RC-1: a handle has already been duplicated to this client
	//! (instance_get_shm_fd ran). A declaration arriving after this point is too
	//! late to be honoured and is refused.
	bool handles_sent;

#ifdef XRT_OS_LINUX_DESKTOP
	/*!
	 * XR_DXR_weave v10 (#1699): fds this process handed to a weave reply and
	 * must close AFTER that reply is on the wire.
	 *
	 * The generated server dispatch sends a handler's out handles with
	 * ipc_send_fds, which (SCM_RIGHTS) installs a copy in the client and does
	 * NOT close ours — and it has no post-send hook. weave_submit_dmabuf's
	 * per-frame release sync_file and weave_get_output_dmabuf's per-call output
	 * dup are both fresh fds the handler owns, so each would leak one fd per
	 * call. Instead the handler parks them here, and the NEXT weave dma-buf
	 * handler on this client (which necessarily runs after the previous reply
	 * was sent — one client thread, strictly request/reply) closes them before
	 * doing anything else; client teardown closes whatever is left. Count-based,
	 * so a zeroed client state is empty (0 is a valid fd).
	 */
	int weave_deferred_close_fds[4];
	uint32_t weave_deferred_close_count;
#endif

	/*!
	 * XR_DXR_lift (ADR-042): this connection's lift-stream owner token, taken
	 * from a process-wide counter on first lift use (0 = never used lift). A
	 * token, not the ics pointer: the thread slot is reused by later
	 * connections, and a stale stream must never resolve for them. Released —
	 * every stream it owns destroyed — at client teardown.
	 */
	uint64_t lift_owner;

	/*!
	 * XR_DXR_lift v3: EXPLICIT viewpoints of lifted weave rects, staged by
	 * lift_weave_rect_viewpoints and consumed (cleared) by the next
	 * lift_weave_rects on this connection — they do not fit that message.
	 */
	struct ipc_arg_lift_rect_viewpoints lift_staged_vps[IPC_LIFT_WEAVE_RECTS_MAX];
	uint32_t lift_staged_vps_count;

	/*!
	 * XR_DXR_stereo_camera (ADR-043): this connection's stream-owner token,
	 * taken from the camera manager on first stream create (0 = never used a
	 * camera). A token, not the ics pointer: the thread slot is reused by later
	 * connections. Every stream it owns is destroyed at client teardown.
	 */
	uint64_t stereo_camera_owner;
};

enum ipc_thread_state
{
	IPC_THREAD_READY,
	IPC_THREAD_STARTING,
	IPC_THREAD_RUNNING,
	IPC_THREAD_STOPPING,
};

struct ipc_thread
{
	struct os_thread thread;
	volatile enum ipc_thread_state state;
	volatile struct ipc_client_state ics;
};


/*!
 *
 */
struct ipc_device
{
	//! The actual device.
	struct xrt_device *xdev;

	//! Is the IO suppressed for this device.
	bool io_active;
};

/*!
 * Platform-specific mainloop object for the IPC server.
 *
 * Contents are essentially implementation details, but are listed in full here so they may be included by value in the
 * main ipc_server struct.
 *
 * @see ipc_design
 *
 * @ingroup ipc_server
 */
struct ipc_server_mainloop
{

#if defined(XRT_OS_ANDROID) || defined(XRT_OS_LINUX) || defined(XRT_DOXYGEN)
	//! For waiting on various events in the main thread.
	int epoll_fd;
#endif

#if defined(XRT_OS_ANDROID) || defined(XRT_DOXYGEN)
	/*!
	 * @name Android Mainloop Members
	 * @{
	 */

	//! File descriptor for the read end of our pipe for submitting new clients
	int pipe_read;

	/*!
	 * File descriptor for the write end of our pipe for submitting new clients
	 *
	 * Must hold client_push_mutex while writing.
	 */
	int pipe_write;

	/*!
	 * Mutex for being able to register oneself as a new client.
	 *
	 * Locked only by threads in `ipc_server_mainloop_add_fd()`.
	 *
	 * This must be locked first, and kept locked the entire time a client is attempting to register and wait for
	 * confirmation. It ensures no acknowledgements of acceptance are lost and moves the overhead of ensuring this
	 * to the client thread.
	 */
	pthread_mutex_t client_push_mutex;


	/*!
	 * The last client fd we accepted, to acknowledge client acceptance.
	 *
	 * Also used as a sentinel during shutdown.
	 *
	 * Must hold accept_mutex while writing.
	 */
	int last_accepted_fd;

	/*!
	 * Condition variable for accepting clients.
	 *
	 * Signalled when @ref last_accepted_fd is updated.
	 *
	 * Associated with @ref accept_mutex
	 */
	pthread_cond_t accept_cond;

	/*!
	 * Mutex for accepting clients.
	 *
	 * Locked by both clients and server: that is, by threads in `ipc_server_mainloop_add_fd()` and in the
	 * server/compositor thread in an implementation function called from `ipc_server_mainloop_poll()`.
	 *
	 * Exists to operate in conjunction with @ref accept_cond - it exists to make sure that the client can be woken
	 * when the server accepts it.
	 */
	pthread_mutex_t accept_mutex;


	/*! @} */
#define XRT_IPC_GOT_IMPL
#endif

#if (defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)) || defined(XRT_DOXYGEN)
	/*!
	 * @name Desktop Linux Mainloop Members
	 * @{
	 */

	//! Socket that we accept connections on.
	int listen_socket;

	//! Were we launched by socket activation, instead of explicitly?
	bool launched_by_socket;

	//! The socket filename we bound to, if any.
	char *socket_filename;

	//! Self-pipe the SIGTERM/SIGINT handler writes to; read end is in epoll (#1744).
	int signal_pipe[2];

	//! Is @ref signal_pipe open? (The struct starts zeroed, and 0 is a valid fd.)
	bool signal_pipe_valid;

	/*! @} */

#define XRT_IPC_GOT_IMPL
#endif

#if defined(XRT_OS_MACOS) || defined(XRT_DOXYGEN)
	/*!
	 * @name macOS Mainloop Members
	 * @{
	 */

	//! kqueue file descriptor for event notification.
	int kqueue_fd;

	//! Socket that we accept connections on.
	int listen_socket;

	//! The socket filename we bound to, if any.
	char *socket_filename;

	//! Self-pipe the SIGTERM/SIGINT handler writes to; read end is in epoll (#1744).
	int signal_pipe[2];

	//! Is @ref signal_pipe open? (The struct starts zeroed, and 0 is a valid fd.)
	bool signal_pipe_valid;

	/*! @} */

#define XRT_IPC_GOT_IMPL
#endif

#if defined(XRT_OS_WINDOWS) || defined(XRT_DOXYGEN)
	/*!
	 * @name Desktop Windows Mainloop Members
	 * @{
	 */

	//! Named Pipe that we accept connections on.
	HANDLE pipe_handle;

	//! Name of the Pipe that we accept connections on.
	char *pipe_name;

	/*! @} */

#define XRT_IPC_GOT_IMPL
#endif

#ifndef XRT_IPC_GOT_IMPL
#error "Need port"
#endif
};

/*!
 * De-initialize the mainloop object.
 * @public @memberof ipc_server_mainloop
 */
void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml);

/*!
 * Initialize the mainloop object.
 *
 * @return <0 on error.
 * @public @memberof ipc_server_mainloop
 */
int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml);

/*!
 * @brief Poll the mainloop.
 *
 * Any errors are signalled by calling ipc_server_handle_failure()
 * @public @memberof ipc_server_mainloop
 */
void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml);

/*!
 * Main IPC object for the server.
 *
 * @ingroup ipc_server
 */
struct ipc_server
{
	struct xrt_instance *xinst;

	//! Handle for the current process, e.g. pidfile on linux
	struct u_process *process;

	struct u_debug_gui *debug_gui;

	//! The @ref xrt_iface level system.
	struct xrt_system *xsys;

	//! System devices.
	struct xrt_system_devices *xsysd;

	//! Space overseer.
	struct xrt_space_overseer *xso;

	//! System compositor.
	struct xrt_system_compositor *xsysc;

	struct ipc_device idevs[XRT_SYSTEM_MAX_DEVICES];
	struct xrt_tracking_origin *xtracks[XRT_SYSTEM_MAX_DEVICES];

	struct ipc_shared_memory *isms[IPC_MAX_CLIENTS];

	struct ipc_server_mainloop ml;

	// Is the mainloop supposed to run.
	volatile bool running;

	// Should we exit when a client disconnects.
	bool exit_on_disconnect;

	// Should we exit when no clients are connected.
	bool exit_when_idle;

	// Timestamp when last client disconnected (for exit_when_idle delay)
	uint64_t last_client_disconnect_ns;

	// How long to wait after all clients disconnect before exiting (in nanoseconds)
	uint64_t exit_when_idle_delay_ns;

	enum u_logging_level log_level;

	//! Workspace mode: multi-compositor with shared window for all clients.
	bool workspace_mode;

	//! PID of the IPC client that activated workspace mode. Captured at
	//! ipc_handle_workspace_activate; checked in common_shutdown so that
	//! abrupt controller-process death (e.g. orchestrator killing the
	//! shell on Disable) tears down workspace state. Zero when no
	//! controller is active.
	unsigned long workspace_controller_pid;

	struct ipc_thread threads[IPC_MAX_CLIENTS];

	//! #959: runtime-admitted client cap, <= IPC_MAX_CLIENTS. Set at start from
	//! DXR_MAX_CLIENTS / a RAM heuristic. The last slot is reserved for the
	//! workspace controller so it can always connect under load.
	uint32_t max_clients;

	volatile uint32_t current_slot_index;

	//! Generator for IDs.
	uint32_t id_generator;

	struct
	{
		int active_client_index;
		int last_active_client_index;

		// Counter for total number of connected clients
		uint32_t connected_client_count;

		struct os_mutex lock;
	} global_state;

	//! XR_DXR_stereo_camera (ADR-043): the camera manager (never NULL once
	//! the server is initialised; it may expose zero cameras).
	struct ipc_server_stereo_camera *stereo_camera;

	//! ADR-051 D3: the display status snapshot + generation counters
	//! (`ipc_server_status.c`); NULL only if its allocation failed.
	struct ipc_server_status *status;
};


/*!
 * Get the current state of a client.
 *
 * @ingroup ipc_server
 */
xrt_result_t
ipc_server_get_client_app_state(struct ipc_server *s, uint32_t client_id, struct ipc_app_state *out_ias);

/*!
 * Look up a client's xrt_compositor by client_id.
 *
 * Used by handlers that need to query per-client compositor state for an
 * arbitrary (non-calling) client — e.g. system_get_client_window_metrics,
 * which the WebXR bridge calls with the target Chrome client's id.
 *
 * Returns XRT_SUCCESS with *out_xc == NULL if the client exists but has
 * no session/compositor (e.g. a headless relay client).
 * Returns XRT_ERROR_IPC_FAILURE if client_id does not resolve.
 *
 * @ingroup ipc_server
 */
xrt_result_t
ipc_server_get_client_xc(struct ipc_server *s, uint32_t client_id, struct xrt_compositor **out_xc);

/*!
 * Set the new active client.
 *
 * @ingroup ipc_server
 */
xrt_result_t
ipc_server_set_active_client(struct ipc_server *s, uint32_t client_id);

/*!
 * Toggle the io for this client.
 *
 * @ingroup ipc_server
 */
xrt_result_t
ipc_server_toggle_io_client(struct ipc_server *s, uint32_t client_id);

/*!
 * Called by client threads to set a session to active.
 *
 * @ingroup ipc_server
 */
void
ipc_server_activate_session(volatile struct ipc_client_state *ics);

/*!
 * Called by client threads to set a session to deactivate.
 *
 * @ingroup ipc_server
 */
void
ipc_server_deactivate_session(volatile struct ipc_client_state *ics);

/*!
 * #962: compute a freshly-created session's initial visible/focused state from
 * the focus authority (default policy or the controller's compositor focus) and
 * push it to the compositor. Replaces the old unconditional visible+focused.
 *
 * @ingroup ipc_server
 */
void
ipc_server_client_initial_state(volatile struct ipc_client_state *ics, bool *out_visible, bool *out_focused);

/*!
 * Thread function for the client side dispatching.
 *
 * @ingroup ipc_server
 */
void *
ipc_server_client_thread(void *_ics);

/*!
 * This destroys the native compositor for this client and any extra objects
 * created from it, like all of the swapchains.
 */
void
ipc_server_client_destroy_session_and_compositor(volatile struct ipc_client_state *ics);

#ifdef XRT_OS_LINUX_DESKTOP
/*!
 * Close every fd parked in ics->weave_deferred_close_fds (XR_DXR_weave v10,
 * #1699). Called at the top of the weave dma-buf handlers and at client
 * teardown. Only safe once the reply that carried those fds has been sent.
 */
void
ipc_server_client_weave_flush_deferred_fds(volatile struct ipc_client_state *ics);
#endif

/*!
 * XR_DXR_lift (ADR-042): destroy every lift stream this client created. Called
 * once at client teardown; a no-op for a client that never used lift.
 */
void
ipc_server_client_lift_release(volatile struct ipc_client_state *ics);

/*!
 * @defgroup ipc_server_internals Server Internals
 * @brief These are only called by the platform-specific mainloop polling code.
 * @ingroup ipc_server
 * @{
 */
/*!
 * Called when a client has connected, it takes the client's ipc handle.
 * Handles all things needed to be done for a client connecting, like starting
 * it's thread.
 *
 * @param vs         The IPC server.
 * @param ipc_handle Handle to communicate over.
 * @memberof ipc_server
 */
void
ipc_server_handle_client_connected(struct ipc_server *vs, xrt_ipc_handle_t ipc_handle);

/*!
 * Perform whatever needs to be done when the mainloop polling encounters a failure.
 * @memberof ipc_server
 */
void
ipc_server_handle_failure(struct ipc_server *vs);

/*!
 * Perform whatever needs to be done when the mainloop polling identifies that the server should be shut down.
 *
 * Does something like setting a flag or otherwise signalling for shutdown: does not itself explicitly exit.
 * @memberof ipc_server
 */
void
ipc_server_handle_shutdown_signal(struct ipc_server *vs);

xrt_result_t
ipc_server_get_system_properties(struct ipc_server *vs, struct xrt_system_properties *out_properties);
//! @}

/*!
 * Function pointer the IPC server calls to learn the PID of the
 * orchestrator-spawned workspace controller, for `workspace_activate`
 * authentication. Returns 0 if no orchestrator-managed workspace is
 * running (= manual mode → first-claim wins).
 */
typedef unsigned long (*ipc_server_workspace_pid_provider_fn)(void);

/*!
 * Register the workspace-PID provider. The standalone service registers
 * this from main() after orchestrator init. Other consumers of ipc_server
 * (sdl_test, the Android service module) that don't run an orchestrator
 * leave it unset — workspace_activate then operates in manual mode.
 *
 * Pass NULL to clear the registration (e.g. on shutdown).
 */
void
ipc_server_set_workspace_pid_provider(ipc_server_workspace_pid_provider_fn fn);

/*!
 * #959: the orchestrator-spawned workspace controller pid (0 if none). Read by
 * the accept path to reserve the last client slot for the controller.
 */
unsigned long
ipc_server_get_orchestrator_workspace_pid(void);

/*!
 * #960: class-verification provider. The IPC server verifies a client's declared
 * class (enum xrt_client_class) at describe_client. Two claims need facts only
 * the service target owns (registry / install dir), so they are delegated:
 *   - CONTROLLER: is @p peer_exe_path a registered workspace-controller binary
 *     (HKLM WorkspaceControllers\*\Binary, POSIX manifests, or the orchestrator's
 *     dev-override entry)?
 *   - DIAG: does @p peer_exe_path live in the runtime's own install directory
 *     (displayxr-cli, the WebXR bridge's introspection connection)?
 * @p peer_exe_path may be "" when the OS would not tell us (Low-IL peer). Return
 * true to accept the claim, false to demote it to APP. Never called for classes
 * the server can verify by itself (orchestrator-spawned pid) or by use
 * (RELAY / PRESENT_OWNER).
 */
typedef bool (*ipc_server_client_class_verify_fn)(long peer_pid, const char *peer_exe_path, uint32_t declared_class);

/*!
 * Register the class-verification provider. Same lifetime model as
 * `ipc_server_set_workspace_pid_provider`. With no provider registered, only the
 * built-in checks (orchestrator pid, DXR_ALLOW_UNVERIFIED_CONTROLLER=1) can verify
 * a CONTROLLER claim and no DIAG claim can be verified.
 */
void
ipc_server_set_client_class_verify_provider(ipc_server_client_class_verify_fn fn);

/*!
 * #960: short upper-case name of an enum xrt_client_class value ("APP",
 * "CONTROLLER", ...) for logs and telemetry.
 */
const char *
ipc_server_client_class_str(uint32_t client_class);

/*!
 * One connected slot's IPC-layer health facts — what one `[HEALTH] slot=…` log
 * line prints (#951), kept as data so the log and the status snapshot
 * (ADR-051 D3, phase 2) read the same struct and cannot drift.
 */
struct ipc_service_health_slot
{
	uint32_t slot;                            //!< Server thread slot index.
	uint32_t id;                              //!< Client id.
	int64_t pid;                              //!< Client process id.
	uint32_t client_class;                    //!< Verified `enum xrt_client_class`.
	bool class_verified;                      //!< false = "unverified" (describe_client not run yet).
	char name[XRT_MAX_APPLICATION_NAME_SIZE]; //!< Application name ("" = unknown).
	bool session_active;                      //!< Session flags.
	bool session_visible;                     //!< See @ref session_active.
	bool session_focused;                     //!< See @ref session_active.
	bool io_active;                           //!< Input routed to this client.
	bool primary_application;                 //!< Primary application.
	uint32_t swapchain_count;                 //!< Live swapchains.
	uint32_t space_count;                     //!< Live spaces.
};

//! Who holds the panel lease (#961).
enum ipc_service_lease_kind
{
	IPC_SERVICE_LEASE_NONE = 0,       //!< Nobody.
	IPC_SERVICE_LEASE_CONTROLLER = 1, //!< The connected workspace controller.
	IPC_SERVICE_LEASE_SLOT = 2,       //!< The focused client's slot (default policy).
};

/*!
 * The service's health summary — the `[HEALTH]` log lines as data (#951,
 * #961, #1002). Filled under `global_state.lock`.
 */
struct ipc_service_health
{
	uint32_t slot_count;                                   //!< Valid entries in @ref slots.
	struct ipc_service_health_slot slots[IPC_MAX_CLIENTS]; //!< Connected slots, in slot order.
	uint32_t max_clients;                                  //!< Runtime-admitted client cap.
	int32_t active_idx;                                    //!< Active client slot (-1 = none).
	enum ipc_service_lease_kind lease;                     //!< Panel lease holder kind.
	int32_t lease_slot;                                    //!< Slot holding the lease (LEASE_SLOT only).
	bool device_removed;                                   //!< The service's D3D11 device is REMOVED.
	bool dp_state_known;                                   //!< A service compositor reports DP backend state.
	uint32_t dp_backend_state;                             //!< `XRT_DP_BACKEND_STATE_*` (iff @ref dp_state_known).
};

/*!
 * ADR-051: copy the current health summary into @p out, under
 * `global_state.lock` (the lock the `[HEALTH]` emitter holds). For the
 * phase-2 status RPCs; safe from any thread.
 */
void
ipc_server_get_health(struct ipc_server *s, struct ipc_service_health *out);

/*!
 * ADR-051 D3: what only the service TARGET can fill into a status snapshot
 * (runtime identity, plug-ins, the screen rows — the loader lives in
 * targets/common, above this library). Registered by the service's main;
 * same lifetime model as the other `ipc_server_set_*_provider` hooks.
 */
struct ipc_server_status_provider
{
	//! `target_status_snapshot_build_service`: the whole snapshot from the
	//! service's instance + DP registry + the live facts gathered here.
	void (*build)(struct xrt_instance *xi,
	              const struct xrt_dp_factory_registry *reg,
	              const struct xrt_status_live *live,
	              struct xrt_status_snapshot *out);
	//! `target_status_snapshot_change_key`: moves with the plug-ins' load /
	//! platform state and vendor change counters (folded into `topology`).
	uint64_t (*change_key)(const struct xrt_dp_factory_registry *reg);
};

//! Register (or, with NULL, clear) the status provider.
void
ipc_server_set_status_provider(const struct ipc_server_status_provider *provider);

/*!
 * @name Display status snapshot (ADR-051 D3; `ipc_server_status.c`)
 *
 * The service-side snapshot behind the three session-free DIAG RPCs. The
 * `topology` / `status` counters move on events (bump sites below, plus what a
 * status read finds changed), with a 2 s floor; the snapshot is rebuilt lazily
 * on the first piece read after a counter moved. Nothing here runs unless a
 * DIAG consumer reads, and nothing logs.
 * @{
 */
//! Allocate the status state (init_all).
void
ipc_server_status_init(struct ipc_server *s);
//! Free it (teardown_all).
void
ipc_server_status_fini(struct ipc_server *s);
/*!
 * Bump a counter from an event site (client connect / disconnect, display
 * re-probe). Takes only a leaf lock: callable while holding anything.
 */
void
ipc_server_status_bump(struct ipc_server *s, bool topology);
//! `system_get_status_generation`.
xrt_result_t
ipc_server_status_get_generation(struct ipc_server *s, struct xrt_status_generation *out);
//! `system_get_status_snapshot(screen_index)`: the head + one screen row (zeroed past the count).
xrt_result_t
ipc_server_status_get_snapshot_piece(struct ipc_server *s,
                                     uint32_t screen_index,
                                     struct xrt_status_head *out_head,
                                     struct xrt_status_screen *out_screen);
//! `system_get_client_segments(client_id)`: that client's row + raw segment table, at the snapshot's generation.
xrt_result_t
ipc_server_status_get_client(struct ipc_server *s,
                             uint32_t client_id,
                             struct xrt_status_client *out_client,
                             struct xrt_segment_metrics *out_metrics,
                             struct xrt_status_generation *out_generation);
/*! @} */

/*!
 * #960: per-class admission quota (ADR-035 D6). CONTROLLER 1, RELAY 1,
 * PRESENT_OWNER 2, DIAG 4, PROVIDER_HOST 2 (outside the app budget), APP =
 * s->max_clients minus the reserved controller slot (0 = unlimited within the cap).
 */
uint32_t
ipc_server_client_class_quota(const struct ipc_server *s, uint32_t client_class);

/*!
 * Function pointer the IPC server calls to learn whether the active
 * workspace controller advertises Tier 1 file-dialog support (registry
 * value `SupportsFileDialog = REG_DWORD 1` under its registration).
 * Returns false if no orchestrator-managed workspace is running or
 * the controller did not opt in. Drives the early fallback in
 * `xrRequestFilePickerDXR` — without it, requests would queue for a
 * controller that has no handler.
 */
typedef bool (*ipc_server_workspace_supports_file_dialog_fn)(void);

/*!
 * Register the file-dialog-capability provider. Same lifetime model
 * as `ipc_server_set_workspace_pid_provider`. Pass NULL to clear.
 */
void
ipc_server_set_workspace_supports_file_dialog_provider(ipc_server_workspace_supports_file_dialog_fn fn);

/*!
 * Function pointer the IPC server calls to summon (spawn-if-absent) the
 * registered workspace controller on demand. The standalone service registers
 * the orchestrator's spawn entry point here so UI surfaces inside ipc_server
 * (the macOS menu-bar status item / Ctrl+Space hotkey, #61) can launch the
 * controller through the same registry-discovery + crash-respawn path as
 * startup, rather than an ad-hoc spawn. Idempotent: a no-op when the controller
 * is already running.
 */
typedef void (*ipc_server_workspace_summon_fn)(void);

/*!
 * Register the workspace-summon provider. Same lifetime model as
 * `ipc_server_set_workspace_pid_provider`. Pass NULL to clear.
 */
void
ipc_server_set_workspace_summon_provider(ipc_server_workspace_summon_fn fn);

/*!
 * Invoke the registered workspace-summon provider, if any. Returns true if a
 * provider was registered and called, false if none is set (caller may fall
 * back to a dev spawn path). Safe to call from the main thread.
 */
bool
ipc_server_request_workspace_summon(void);

/*!
 * Display dashboard phase 8: function pointer the IPC server calls for the
 * DIAG RPC `system_reload_service_config` — re-read the service's persisted
 * config (`service.json`) and apply it live (launch hotkey, per-controller
 * mode) without restarting a running workspace controller. Returns true when
 * the reload was accepted (it may complete asynchronously on the service's own
 * thread). The standalone service registers it; nothing else does.
 */
typedef bool (*ipc_server_service_config_reload_fn)(void);

/*!
 * Register the config-reload provider. Same lifetime model as
 * `ipc_server_set_workspace_pid_provider`. Pass NULL to clear.
 */
void
ipc_server_set_service_config_reload_provider(ipc_server_service_config_reload_fn fn);

/*!
 * Display dashboard phase 8: function pointer the IPC server calls for the
 * DIAG RPC `system_workspace_launch` — spawn workspace controller
 * @p controller_id now, through the same path as the launch hotkey. Returns an
 * `enum ipc_workspace_launch_status` value.
 */
typedef uint32_t (*ipc_server_workspace_launch_fn)(const char *controller_id);

/*!
 * Register the workspace-launch provider. Same lifetime model as
 * `ipc_server_set_workspace_pid_provider`. Pass NULL to clear.
 */
void
ipc_server_set_workspace_launch_provider(ipc_server_workspace_launch_fn fn);

/*!
 * Display dashboard phase 8: function pointer the IPC server calls for the
 * DIAG RPC `system_workspace_hotkey_suspend` — @p suspend true takes the
 * launch-hotkey hook out of the input pipeline (a running controller and the
 * persisted config are untouched; the service itself resumes after a 60 s
 * safety timeout, re-armed by every suspend), false re-installs it from the
 * current config. NOT tied to the caller's connection: the caller is a
 * one-shot CLI process that exits at once. Returns true when accepted.
 */
typedef bool (*ipc_server_workspace_hotkey_suspend_fn)(bool suspend);

/*!
 * Register the hotkey-suspend provider. Same lifetime model as
 * `ipc_server_set_workspace_pid_provider`. Pass NULL to clear.
 */
void
ipc_server_set_workspace_hotkey_suspend_provider(ipc_server_workspace_hotkey_suspend_fn fn);

/*
 *
 * Helpers
 *
 */

/*!
 * Get a xdev with the given device_id.
 */
static inline struct xrt_device *
get_xdev(volatile struct ipc_client_state *ics, uint32_t device_id)
{
	return ics->server->idevs[device_id].xdev;
}

/*!
 * Get a idev with the given device_id.
 */
static inline struct ipc_device *
get_idev(volatile struct ipc_client_state *ics, uint32_t device_id)
{
	return &ics->server->idevs[device_id];
}

/*!
 * Get the data in the shared memory of the given client.
 */
static inline struct ipc_shared_memory *
get_ism(volatile struct ipc_client_state *ics)
{
	return ics->server->isms[ics->server_thread_index];
}

/*!
 * Get the handle for the shared memory of the given client.
 */
static inline xrt_shmem_handle_t
get_ism_handle(volatile struct ipc_client_state *ics)
{
	return ics->ism_handle;
}

#ifdef __cplusplus
}
#endif
