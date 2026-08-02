// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  net_input hub — loopback-TCP-fed motion controllers.
 *
 * Socket plumbing derived from Monado's removed `remote` driver
 * (`r_hub.c`, restored from the pre-strip history for #823); the device
 * model is new: two `khr/simple_controller` motion controllers whose
 * poses arrive from an external feeder process over the versioned wire
 * protocol in `net_input_proto.h` and land in `m_relation_history`, so
 * `get_tracked_pose(at_time_ns)` interpolates/predicts at the requested
 * timestamp instead of returning "latest sample". Haptic output events
 * are written back to the feeder on the same socket.
 *
 * Threading: one hub thread (accept → handshake → read loop, guarded
 * throughout by 500 ms select() timeouts so `os_thread_helper` can stop
 * it). Button/battery state is mutex-copied into the devices'
 * `update_inputs`; `m_relation_history` is internally thread-safe.
 * Haptic sends take the same mutex (socket writes are small and rare).
 *
 * Clock mapping: a non-zero feeder timestamp is translated into the
 * provider's monotonic domain with a latency-floor offset estimate
 * (running minimum of receipt-minus-feeder deltas, with a slow decay to
 * track clock drift); a zero timestamp is stamped on receipt.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 * @ingroup drv_net_input
 */

#include "xrt/xrt_config_os.h"

#if defined(XRT_OS_WINDOWS)
// winsock2.h must precede windows.h (which other headers pull in).
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_input_socket_t;
#define NET_INPUT_INVALID_SOCKET INVALID_SOCKET
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
typedef int net_input_socket_t;
#define NET_INPUT_INVALID_SOCKET (-1)
#endif

#include "xrt/xrt_device.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "math/m_relation_history.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_logging.h"

#include "net_input_proto.h"
#include "net_input_interface.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Structs and defines.
 *
 */

// Indices into base.inputs, matching net_input_inputs_array below.
#define NET_INPUT_SELECT 0
#define NET_INPUT_MENU 1
#define NET_INPUT_GRIP 2
#define NET_INPUT_AIM 3

struct net_input_hub;

struct net_input_device
{
	struct xrt_device base;

	struct net_input_hub *hub;

	//! NET_INPUT_HAND_LEFT / NET_INPUT_HAND_RIGHT.
	uint8_t hand;

	//! Thread-safe pose buffer, fed by the hub thread.
	struct m_relation_history *history;

	//! Guarded by hub->mutex.
	bool active;
	bool btn_select;
	bool btn_menu;
	uint8_t battery_pct;
};

struct net_input_hub
{
	struct os_thread_helper oth;

	//! Guards device button/battery state, the client socket, and the
	//! connected flag.
	struct os_mutex mutex;

	net_input_socket_t accept_fd;
	net_input_socket_t client_fd;
	bool connected;

	uint16_t port;

	//! Latency-floor clock-offset estimate (provider_ns - feeder_ns).
	int64_t clock_offset_ns;
	bool have_clock_offset;

	//! Diagnostics (u_var).
	uint64_t state_msg_count;
	uint64_t haptic_msg_count;

	struct net_input_device *devices[2];

	//! Live device count; the last xrt_device::destroy tears the hub down.
	int device_refcount;
};


/*
 *
 * Platform socket wrappers (from Monado's r_hub.c).
 *
 */

#if defined(XRT_OS_WINDOWS)

static inline void
socket_close(net_input_socket_t id)
{
	closesocket(id);
}

static inline ssize_t
socket_read(net_input_socket_t id, void *ptr, size_t size)
{
	return recv(id, (char *)ptr, (int)size, 0);
}

static inline ssize_t
socket_write(net_input_socket_t id, const void *ptr, size_t size)
{
	return send(id, (const char *)ptr, (int)size, 0);
}

#else

static inline void
socket_close(net_input_socket_t id)
{
	close(id);
}

static inline ssize_t
socket_read(net_input_socket_t id, void *ptr, size_t size)
{
	return read(id, ptr, size);
}

static inline ssize_t
socket_write(net_input_socket_t id, const void *ptr, size_t size)
{
	return write(id, ptr, size);
}

#endif


/*
 *
 * Hub thread.
 *
 */

/*!
 * Wait (≤500 ms slices) until @p sock is readable or the thread is asked
 * to stop. Returns true when readable.
 */
static bool
wait_readable_and_running(struct net_input_hub *hub, net_input_socket_t sock)
{
	if (sock == NET_INPUT_INVALID_SOCKET) {
		return false;
	}

	int ret = 0;
	while (os_thread_helper_is_running(&hub->oth) && ret == 0) {
		struct timeval timeout = {.tv_sec = 0, .tv_usec = 500 * 1000};
		fd_set set;
		FD_ZERO(&set);
		FD_SET(sock, &set);
		ret = select((int)sock + 1, &set, NULL, NULL, &timeout);
	}
	return ret > 0;
}

//! Blocking full-size read with stop-awareness; false on disconnect/stop.
static bool
read_exact(struct net_input_hub *hub, net_input_socket_t sock, void *out, size_t size)
{
	size_t current = 0;
	while (current < size) {
		if (!wait_readable_and_running(hub, sock)) {
			return false;
		}
		ssize_t ret = socket_read(sock, (uint8_t *)out + current, size - current);
		if (ret <= 0) {
			return false;
		}
		current += (size_t)ret;
	}
	return true;
}

static bool
setup_accept_fd(struct net_input_hub *hub)
{
#if defined(XRT_OS_WINDOWS)
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		U_LOG_E("net_input: WSAStartup failed (%d).", WSAGetLastError());
		return false;
	}
#endif

	net_input_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == NET_INPUT_INVALID_SOCKET) {
		U_LOG_E("net_input: socket() failed.");
		return false;
	}

	int flag = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flag, sizeof(flag));

	// Loopback ONLY — the feed is a local-process seam, never a
	// network service (discovery spec §5).
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(hub->port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		U_LOG_E("net_input: bind(127.0.0.1:%u) failed — port in use?", hub->port);
		socket_close(fd);
		return false;
	}
	if (listen(fd, 1) != 0) {
		U_LOG_E("net_input: listen failed.");
		socket_close(fd);
		return false;
	}

	hub->accept_fd = fd;
	return true;
}

//! Map a feeder timestamp into the provider clock; see file docstring.
static int64_t
map_feeder_timestamp(struct net_input_hub *hub, int64_t feeder_ns, int64_t recv_ns)
{
	if (feeder_ns == 0) {
		return recv_ns;
	}

	int64_t inst = recv_ns - feeder_ns;
	if (!hub->have_clock_offset || inst < hub->clock_offset_ns) {
		hub->clock_offset_ns = inst; // latency floor
		hub->have_clock_offset = true;
	} else {
		// Slow decay toward the instantaneous estimate so genuine
		// clock drift doesn't pin us to a stale minimum.
		hub->clock_offset_ns += (inst - hub->clock_offset_ns) / 1024;
	}
	return feeder_ns + hub->clock_offset_ns;
}

static void
handle_state_msg(struct net_input_hub *hub, const struct net_input_state_msg *msg, int64_t recv_ns)
{
	if (msg->hand > NET_INPUT_HAND_RIGHT) {
		return;
	}
	struct net_input_device *dev = hub->devices[msg->hand];
	if (dev == NULL) {
		return;
	}

	os_mutex_lock(&hub->mutex);
	dev->active = msg->active != 0;
	dev->btn_select = (msg->buttons & NET_INPUT_BUTTON_SELECT_BIT) != 0;
	dev->btn_menu = (msg->buttons & NET_INPUT_BUTTON_MENU_BIT) != 0;
	dev->battery_pct = msg->battery_pct;
	hub->state_msg_count++;
	int64_t ts = map_feeder_timestamp(hub, msg->timestamp_ns, recv_ns);
	os_mutex_unlock(&hub->mutex);

	if (msg->active == 0) {
		return;
	}

	struct xrt_space_relation rel = {0};
	rel.pose.position.x = msg->position[0];
	rel.pose.position.y = msg->position[1];
	rel.pose.position.z = msg->position[2];
	rel.pose.orientation.x = msg->orientation[0];
	rel.pose.orientation.y = msg->orientation[1];
	rel.pose.orientation.z = msg->orientation[2];
	rel.pose.orientation.w = msg->orientation[3];
	rel.linear_velocity.x = msg->linear_velocity[0];
	rel.linear_velocity.y = msg->linear_velocity[1];
	rel.linear_velocity.z = msg->linear_velocity[2];
	rel.angular_velocity.x = msg->angular_velocity[0];
	rel.angular_velocity.y = msg->angular_velocity[1];
	rel.angular_velocity.z = msg->angular_velocity[2];
	rel.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

	m_relation_history_push(dev->history, &rel, ts);
}

//! One feeder session: handshake, then the state-message read loop.
static void
run_connection(struct net_input_hub *hub, net_input_socket_t client)
{
	// Handshake: both sides send immediately, then validate the peer's.
	struct net_input_hello ours = {.magic = NET_INPUT_MAGIC, .version = NET_INPUT_PROTO_VERSION};
	if (socket_write(client, &ours, sizeof(ours)) != (ssize_t)sizeof(ours)) {
		U_LOG_W("net_input: hello write failed.");
		return;
	}
	struct net_input_hello theirs = {0};
	if (!read_exact(hub, client, &theirs, sizeof(theirs))) {
		return;
	}
	if (theirs.magic != NET_INPUT_MAGIC || theirs.version != NET_INPUT_PROTO_VERSION) {
		U_LOG_W("net_input: feeder handshake mismatch (magic=0x%08x version=%u) — dropping.", theirs.magic,
		        theirs.version);
		return;
	}

	os_mutex_lock(&hub->mutex);
	hub->client_fd = client;
	hub->connected = true;
	hub->have_clock_offset = false;
	os_mutex_unlock(&hub->mutex);

	U_LOG_W("net_input: feeder connected (proto v%u).", theirs.version);

	for (;;) {
		struct net_input_state_msg msg;
		if (!read_exact(hub, client, &msg, sizeof(msg))) {
			break;
		}
		int64_t recv_ns = (int64_t)os_monotonic_get_ns();
		if (msg.type != NET_INPUT_MSG_STATE) {
			U_LOG_W("net_input: unexpected message type %u — dropping connection.", msg.type);
			break;
		}
		handle_state_msg(hub, &msg, recv_ns);
	}

	// Disconnected: mark devices inactive, clear the socket.
	os_mutex_lock(&hub->mutex);
	hub->connected = false;
	hub->client_fd = NET_INPUT_INVALID_SOCKET;
	for (int i = 0; i < 2; i++) {
		if (hub->devices[i] != NULL) {
			hub->devices[i]->active = false;
		}
	}
	os_mutex_unlock(&hub->mutex);

	U_LOG_W("net_input: feeder disconnected.");
}

static void *
hub_thread(void *ptr)
{
	struct net_input_hub *hub = (struct net_input_hub *)ptr;

	while (os_thread_helper_is_running(&hub->oth)) {
		if (!wait_readable_and_running(hub, hub->accept_fd)) {
			continue; // stop-flag re-checked by the while
		}

		struct sockaddr_in addr = {0};
		socklen_t addr_len = (socklen_t)sizeof(addr);
		net_input_socket_t client = accept(hub->accept_fd, (struct sockaddr *)&addr, &addr_len);
		if (client == NET_INPUT_INVALID_SOCKET) {
			continue;
		}

		run_connection(hub, client);
		socket_close(client);
	}

	return NULL;
}


/*
 *
 * Hub lifetime.
 *
 */

static void
hub_destroy(struct net_input_hub *hub)
{
	os_thread_helper_stop_and_wait(&hub->oth);

	if (hub->accept_fd != NET_INPUT_INVALID_SOCKET) {
		socket_close(hub->accept_fd);
		hub->accept_fd = NET_INPUT_INVALID_SOCKET;
	}
	if (hub->client_fd != NET_INPUT_INVALID_SOCKET) {
		socket_close(hub->client_fd);
		hub->client_fd = NET_INPUT_INVALID_SOCKET;
	}
#if defined(XRT_OS_WINDOWS)
	WSACleanup();
#endif

	u_var_remove_root(hub);
	os_thread_helper_destroy(&hub->oth);
	os_mutex_destroy(&hub->mutex);
	free(hub);
}


/*
 *
 * Device member functions.
 *
 */

static inline struct net_input_device *
net_input_device(struct xrt_device *xdev)
{
	return (struct net_input_device *)xdev;
}

static void
net_input_device_destroy(struct xrt_device *xdev)
{
	struct net_input_device *dev = net_input_device(xdev);
	struct net_input_hub *hub = dev->hub;

	os_mutex_lock(&hub->mutex);
	hub->devices[dev->hand] = NULL;
	int remaining = --hub->device_refcount;
	os_mutex_unlock(&hub->mutex);

	m_relation_history_destroy(&dev->history);
	u_device_free(&dev->base);

	if (remaining == 0) {
		hub_destroy(hub);
	}
}

static xrt_result_t
net_input_device_update_inputs(struct xrt_device *xdev)
{
	struct net_input_device *dev = net_input_device(xdev);
	int64_t now = (int64_t)os_monotonic_get_ns();

	os_mutex_lock(&dev->hub->mutex);
	bool active = dev->active;
	bool select = dev->btn_select;
	bool menu = dev->btn_menu;
	os_mutex_unlock(&dev->hub->mutex);

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = active;
		xdev->inputs[i].timestamp = now;
	}
	if (!active) {
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			U_ZERO(&xdev->inputs[i].value);
		}
		return XRT_SUCCESS;
	}
	xdev->inputs[NET_INPUT_SELECT].value.boolean = select;
	xdev->inputs[NET_INPUT_MENU].value.boolean = menu;

	return XRT_SUCCESS;
}

static xrt_result_t
net_input_device_get_tracked_pose(struct xrt_device *xdev,
                                  enum xrt_input_name name,
                                  int64_t at_timestamp_ns,
                                  struct xrt_space_relation *out_relation)
{
	struct net_input_device *dev = net_input_device(xdev);

	switch (name) {
	case XRT_INPUT_SIMPLE_GRIP_POSE:
	case XRT_INPUT_SIMPLE_AIM_POSE: break;
	default:
		// Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT — see the
		// matching comment in sim_input_device.c (plug-in link vs the
		// aux export surface).
		U_LOG_E("net_input: unsupported input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation rel = {0};
	enum m_relation_history_result res = m_relation_history_get(dev->history, at_timestamp_ns, &rel);
	if (res == M_RELATION_HISTORY_RESULT_INVALID) {
		// No samples yet (feeder never connected / hand inactive).
		out_relation->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
		out_relation->relation_flags = 0;
		return XRT_SUCCESS;
	}

	*out_relation = rel;
	return XRT_SUCCESS;
}

static xrt_result_t
net_input_device_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct net_input_device *dev = net_input_device(xdev);
	struct net_input_hub *hub = dev->hub;

	if (name != XRT_OUTPUT_NAME_SIMPLE_VIBRATION) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct net_input_haptic_msg msg = {0};
	msg.type = NET_INPUT_MSG_HAPTIC;
	msg.hand = dev->hand;
	msg.amplitude = value->vibration.amplitude;
	msg.frequency = value->vibration.frequency;
	msg.duration_ns = value->vibration.duration_ns;

	os_mutex_lock(&hub->mutex);
	if (hub->connected && hub->client_fd != NET_INPUT_INVALID_SOCKET) {
		(void)socket_write(hub->client_fd, &msg, sizeof(msg));
		hub->haptic_msg_count++;
	}
	os_mutex_unlock(&hub->mutex);

	return XRT_SUCCESS;
}


/*
 *
 * khr/simple_controller data arrays.
 *
 */

static enum xrt_input_name net_input_inputs_array[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK,
    XRT_INPUT_SIMPLE_MENU_CLICK,
    XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,
};

static enum xrt_output_name net_input_outputs_array[] = {
    XRT_OUTPUT_NAME_SIMPLE_VIBRATION,
};

static struct net_input_device *
create_device(struct net_input_hub *hub, uint8_t hand)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	const uint32_t input_count = ARRAY_SIZE(net_input_inputs_array);
	const uint32_t output_count = ARRAY_SIZE(net_input_outputs_array);
	bool is_left = hand == NET_INPUT_HAND_LEFT;

	struct net_input_device *dev = U_DEVICE_ALLOCATE(struct net_input_device, flags, input_count, output_count);
	dev->base.update_inputs = net_input_device_update_inputs;
	dev->base.get_tracked_pose = net_input_device_get_tracked_pose;
	dev->base.get_hand_tracking = u_device_ni_get_hand_tracking;
	dev->base.get_view_poses = u_device_ni_get_view_poses;
	dev->base.set_output = net_input_device_set_output;
	dev->base.destroy = net_input_device_destroy;
	dev->base.supported.orientation_tracking = true;
	dev->base.supported.position_tracking = true;
	dev->base.supported.hand_tracking = false;
	dev->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	dev->base.device_type = is_left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;

	// 6DOF feed — origin type must not stay TRACKING_TYPE_NONE, or the
	// legacy builder injects per-hand arm-model offsets into every pose
	// (see the matching comment in sim_input_device.c).
	dev->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(dev->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Network Input Feed");

	snprintf(dev->base.str, sizeof(dev->base.str), "Network %s Motion Controller", is_left ? "Left" : "Right");
	snprintf(dev->base.serial, sizeof(dev->base.serial), "NET-INPUT-%s", is_left ? "L" : "R");

	for (uint32_t i = 0; i < input_count; i++) {
		dev->base.inputs[i].active = false; // inactive until the feeder speaks
		dev->base.inputs[i].name = net_input_inputs_array[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		dev->base.outputs[i].name = net_input_outputs_array[i];
	}

	dev->hub = hub;
	dev->hand = hand;
	dev->battery_pct = 255;
	m_relation_history_create(&dev->history);

	return dev;
}


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
net_input_create_devices(uint16_t port, struct xrt_device **out_left, struct xrt_device **out_right)
{
	struct net_input_hub *hub = U_TYPED_CALLOC(struct net_input_hub);
	if (hub == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	hub->port = port != 0 ? port : (uint16_t)NET_INPUT_DEFAULT_PORT;
	hub->accept_fd = NET_INPUT_INVALID_SOCKET;
	hub->client_fd = NET_INPUT_INVALID_SOCKET;

	if (os_mutex_init(&hub->mutex) != 0) {
		free(hub);
		return XRT_ERROR_ALLOCATION;
	}
	if (os_thread_helper_init(&hub->oth) != 0) {
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return XRT_ERROR_ALLOCATION;
	}

	if (!setup_accept_fd(hub)) {
		// Port taken (a second runtime instance?) or no socket API —
		// decline; the builder falls back to qwerty.
		os_thread_helper_destroy(&hub->oth);
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct net_input_device *left = create_device(hub, NET_INPUT_HAND_LEFT);
	struct net_input_device *right = create_device(hub, NET_INPUT_HAND_RIGHT);
	hub->devices[NET_INPUT_HAND_LEFT] = left;
	hub->devices[NET_INPUT_HAND_RIGHT] = right;
	hub->device_refcount = 2;

	if (os_thread_helper_start(&hub->oth, hub_thread, hub) != 0) {
		left->base.destroy(&left->base);   // refcount 2 → 1
		right->base.destroy(&right->base); // refcount 1 → 0, destroys hub
		return XRT_ERROR_ALLOCATION;
	}

	u_var_add_root(hub, "Network Input Hub", true);
	u_var_add_ro_u64(hub, &hub->state_msg_count, "state_msg_count");
	u_var_add_ro_u64(hub, &hub->haptic_msg_count, "haptic_msg_count");
	u_var_add_bool(hub, &hub->connected, "connected");

	U_LOG_W("net_input: listening on 127.0.0.1:%u (proto v%u) — feed with scripts/net_input_feeder.py.", hub->port,
	        NET_INPUT_PROTO_VERSION);

	*out_left = &left->base;
	*out_right = &right->base;
	return XRT_SUCCESS;
}
