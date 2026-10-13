// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service <-> conversion-host wire: control messages and shm ring
 *         records (ADR-053 D5; roadmap conversion-providers.md §3.4, §3.5).
 *
 * The service drives one `displayxr-conversion-host` per provider DLL over a
 * DEDICATED channel (not a Monado IPC client). Two planes:
 *
 *  - CONTROL: a byte stream (named pipe on Windows, AF_UNIX stream socket on
 *    POSIX; os_process.h creates the pair) carrying framed messages. Every
 *    frame is a conv_host_wire_frame followed by exactly payload_bytes of one
 *    payload struct below. Requests come from the service and carry a non-zero
 *    request_id; the host answers with the same request_id and type |
 *    CONV_HOST_MSG_REPLY_BIT. The host's unsolicited messages (hello, log,
 *    heartbeat, state, job progress, output images) carry request_id 0, except
 *    an output image announced while answering a request, which carries that
 *    request's id.
 *  - DATA: per host and lane, a 64-record command ring + completion ring
 *    (u_shm_ring.h) in one shm section, with one wake signal each way. Ring
 *    records carry IMAGE AND SYNC INDICES, never handles.
 *
 * Wire rules (roadmap §6.6, mirrored from xrt/xrt_conv_wire.h):
 *  1. Every payload starts with `uint32_t struct_size; uint32_t
 *     family_version;`, uses fixed-width types, has no hidden padding (every
 *     gap is a reserved_* field) and embeds no runtime struct: frozen plug-in
 *     ABI blocks travel as the wire twins of xrt_conv_wire.h, and the two
 *     top-level ABI structs the host reports (capability, state) as the twins
 *     defined here, asserted field by field at the end of this header.
 *  2. One copy: the receiver reads a payload with conv_host_wire_receive(),
 *     which copies min(struct_size, sizeof) and zero-extends, so a field
 *     appended at a struct's tail is read as 0 by an older peer.
 *  3. Every size is asserted; every payload fits CONV_HOST_WIRE_MAX_PAYLOAD.
 *  4. Ring records have no header: their size is fixed per ring and announced
 *     in conv_host_wire_lane_open; both sides refuse a size they do not know.
 *
 * Handles. The service holds the only cross-process handle right (§3.5):
 *  - Windows: the service DUPLICATES its handles INTO the host
 *    (os_process_dup_to_child) and writes the host-process values into
 *    frame.handles[]; for a host -> service frame, frame.handles[] are values
 *    valid in the HOST, and the service PULLS them with
 *    os_process_dup_from_child. The host never holds a handle to the service
 *    process.
 *  - POSIX: frame.handles[] are 0 and the descriptors ride the frame as
 *    SCM_RIGHTS ancillary data, in order (os_channel_send / os_channel_recv).
 * frame.handle_count says how many there are either way. Each message below
 * documents its handle order.
 *
 * Trust. The host runs third-party code. The service validates every field it
 * reads from the host (indices against its tables, sizes, counts, the ring
 * cursors) and treats a violation like a crash.
 *
 * Self-contained C11 / C++: xrt_conv_wire.h, xrt_conversion_plugin.h and the C
 * library only; no OS header.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Family constants.
 *
 */

//! First four bytes of every frame: "DXCH" read as a little-endian u32.
#define CONV_HOST_WIRE_MAGIC 0x48435844u

/*!
 * Version of the service <-> host family. An append at a payload's tail does
 * not bump it (struct_size covers that); a reorder, retype or removal does,
 * and the receiver rejects any other value. The git-tag gate at hello is the
 * primary compatibility check (service and host ship together); this is the
 * structural one.
 */
#define CONV_HOST_WIRE_FAMILY_VERSION_1 1u
#define CONV_HOST_WIRE_FAMILY_VERSION_CURRENT CONV_HOST_WIRE_FAMILY_VERSION_1

//! Bytes of the common payload header (struct_size + family_version).
#define CONV_HOST_WIRE_HEADER_BYTES 8u

//! Largest payload after a frame, in bytes.
#define CONV_HOST_WIRE_MAX_PAYLOAD 4096u

//! Handles per frame.
#define CONV_HOST_WIRE_MAX_HANDLES 4u

//! Records per ring (roadmap §3.5). A power of two.
#define CONV_HOST_WIRE_RING_CAPACITY 64u

//! Bytes of a git tag / provider id, NUL-terminated.
#define CONV_HOST_WIRE_TAG_MAX 64u

//! Bytes of a forwarded log line, NUL-terminated.
#define CONV_HOST_WIRE_LOG_MAX 480u

//! The host sends a heartbeat at this period (roadmap §3.5: 1 Hz).
#define CONV_HOST_WIRE_HEARTBEAT_PERIOD_MS 1000u

//! "No index" in any image / sync index field.
#define CONV_HOST_WIRE_NO_INDEX 0xFFFFFFFFu

//! Image roles per completion record (out_image_idx[] order).
#define CONV_HOST_WIRE_OUT_VIEWS 0u
#define CONV_HOST_WIRE_OUT_DEPTH 1u
#define CONV_HOST_WIRE_OUT_CONFIDENCE 2u
#define CONV_HOST_WIRE_OUT_DISPARITY 3u
#define CONV_HOST_WIRE_OUT_COUNT 4u

/*!
 * Control channel name, `displayxr-conv-<provider>-<service pid>` (§3.5). On
 * Windows the pipe path is `\\.\pipe\` + this. With an inherited channel
 * (os_channel_pair_create) the name is diagnostic only.
 */
#define CONV_HOST_WIRE_CHANNEL_FMT "displayxr-conv-%s-%lu"

/*! @name Host command line (§3.4). @{ */
#define CONV_HOST_ARG_PROVIDER "--provider"     //!< provider id
#define CONV_HOST_ARG_BINARY "--binary"         //!< absolute path of the provider DLL / .so
#define CONV_HOST_ARG_CHANNEL "--channel"       //!< inherited channel token (os_channel_child_token)
#define CONV_HOST_ARG_PARENT "--parent"         //!< service pid (diagnostics + POSIX parent-death check)
#define CONV_HOST_ARG_ADAPTER_LUID "--adapter-luid" //!< hex; the service's ingest adapter, 0 = CPU / any
/*! @} */

/*!
 * @name Host exit codes (§3.4; own namespace, unrelated to selftest's 13).
 * Anything not listed is a crash (a minidump was written on Windows).
 * @{
 */
#define CONV_HOST_EXIT_ORDERLY 0
#define CONV_HOST_EXIT_ABI_REJECT 10
#define CONV_HOST_EXIT_NEGOTIATE_FAILED 11 //!< negotiate or describe failed
#define CONV_HOST_EXIT_LICENCE 12
#define CONV_HOST_EXIT_NO_DEVICE 13 //!< no usable device on the given adapter
#define CONV_HOST_EXIT_ELEVATED 14  //!< refused to run elevated
#define CONV_HOST_EXIT_UPDATE 20    //!< closed for update: no backoff, no crash-loop accounting
#define CONV_HOST_EXIT_WATCHDOG 30  //!< set by the supervisor when it kills a hung host
/*!
 * Two host-private codes, raised before there is anyone to talk to:
 *  - 2: bad command line. The supervisor never spawns that; the proxy maps it
 *    to FAILED, non-retryable (respawning cannot fix an argv bug).
 *  - 15: the inherited control channel is unusable. Ordinary crash accounting
 *    (backoff, crash-loop cutoff).
 * 12 (licence) is reserved and not raised in P2: an unlicensed capability is
 * per-capability state (XRT_CONV_STATE_UNLICENSED) reported through DESCRIBE.
 */
#define CONV_HOST_EXIT_USAGE 2
#define CONV_HOST_EXIT_CHANNEL 15
/*! @} */

/*! @name conv_host_wire_hello_ack::reject_reason @{ */
#define CONV_HOST_REJECT_NONE 0u
#define CONV_HOST_REJECT_GIT_TAG 1u  //!< service and host builds differ
#define CONV_HOST_REJECT_FAMILY 2u   //!< family_version mismatch
#define CONV_HOST_REJECT_PROVIDER 3u //!< not the provider this host was spawned for
#define CONV_HOST_REJECT_PEER 4u     //!< the peer is not the process the service spawned
/*! @} */

/*! @name conv_host_wire_hello::flags @{ */
#define CONV_HOST_HELLO_ELEVATED (1u << 0) //!< informational; an elevated host exits 14 instead of saying hello
/*! @} */

/*! @name conv_host_wire_lane_open::lane_kind @{ */
#define CONV_HOST_LANE_STREAM 1u //!< stream converts (submit / complete records below)
/*! @} */

/*! @name conv_host_wire_submit_record::op @{ */
#define CONV_HOST_RING_OP_CONVERT 1u //!< stream_convert
/*! @} */

/*! @name conv_host_wire_sync::direction @{ */
#define CONV_HOST_SYNC_SERVICE_SIGNALS 0u //!< input fence: the service signals, the host waits
#define CONV_HOST_SYNC_HOST_SIGNALS 1u    //!< output fence: the host signals, the service OBSERVES ON THE CPU
/*! @} */


/*
 *
 * Message types (conv_host_wire_frame::type).
 *
 * S -> H: request (request_id != 0); the reply is type | REPLY_BIT.
 * H -> S: unsolicited (request_id 0) unless noted.
 *
 */

#define CONV_HOST_MSG_REPLY_BIT 0x8000u

/* H -> S. */
#define CONV_HOST_MSG_HELLO 0x0001u         //!< conv_host_wire_hello; first frame the host sends
#define CONV_HOST_MSG_LOG 0x0002u           //!< conv_host_wire_log (WARN+ forwarded as [conv:<id>])
#define CONV_HOST_MSG_HEARTBEAT 0x0003u     //!< conv_host_wire_heartbeat, every HEARTBEAT_PERIOD_MS
#define CONV_HOST_MSG_STATE_CHANGED 0x0004u //!< conv_host_wire_state_changed
#define CONV_HOST_MSG_JOB_PROGRESS 0x0005u  //!< conv_host_wire_job_progress
#define CONV_HOST_MSG_OUTPUT_IMAGE 0x0006u  //!< conv_host_wire_output_image; request_id = the ALLOC_OUTPUTS it answers
#define CONV_HOST_MSG_SYNC 0x0007u          //!< conv_host_wire_sync (host-created output fence); request_id as above

/* S -> H (requests). */
#define CONV_HOST_MSG_HELLO_ACK 0x0101u        //!< conv_host_wire_hello_ack; request_id 0, no reply
#define CONV_HOST_MSG_DESCRIBE 0x0102u         //!< conv_host_wire_index -> conv_host_wire_describe_reply
#define CONV_HOST_MSG_GET_STATE 0x0103u        //!< conv_host_wire_cap_request -> conv_host_wire_state_reply
#define CONV_HOST_MSG_WARM 0x0104u             //!< conv_host_wire_cap_request -> conv_host_wire_status (when done)
#define CONV_HOST_MSG_EVICT 0x0105u            //!< conv_host_wire_cap_request -> conv_host_wire_status
#define CONV_HOST_MSG_CANCEL 0x0106u           //!< conv_host_wire_cancel -> – (the cancelled request still replies)
#define CONV_HOST_MSG_STREAM_OPEN 0x0107u      //!< conv_host_wire_stream_open -> conv_host_wire_status
#define CONV_HOST_MSG_STREAM_RESET 0x0108u     //!< conv_host_wire_stream_ref -> conv_host_wire_status
#define CONV_HOST_MSG_STREAM_CLOSE 0x0109u     //!< conv_host_wire_stream_ref -> conv_host_wire_status
#define CONV_HOST_MSG_GET_OUTPUT_DESC 0x010Au  //!< conv_host_wire_output_desc_request -> conv_host_wire_output_desc_reply
#define CONV_HOST_MSG_REGISTER_IMAGE 0x010Bu   //!< conv_host_wire_register_image -> conv_host_wire_status
#define CONV_HOST_MSG_UNREGISTER_IMAGE 0x010Cu //!< conv_host_wire_index -> conv_host_wire_status
#define CONV_HOST_MSG_REGISTER_SYNC 0x010Du    //!< conv_host_wire_sync -> conv_host_wire_status
#define CONV_HOST_MSG_ALLOC_OUTPUTS 0x010Eu    //!< conv_host_wire_alloc_outputs -> OUTPUT_IMAGE/SYNC..., then conv_host_wire_status
#define CONV_HOST_MSG_LANE_OPEN 0x010Fu        //!< conv_host_wire_lane_open -> conv_host_wire_status
#define CONV_HOST_MSG_LANE_CLOSE 0x0110u       //!< conv_host_wire_index -> conv_host_wire_status
#define CONV_HOST_MSG_JOB_SUBMIT 0x0111u       //!< conv_host_wire_job_submit -> conv_host_wire_job_done (when done)
#define CONV_HOST_MSG_JOB_CANCEL 0x0112u       //!< conv_host_wire_job_ref -> – (the job still replies)
#define CONV_HOST_MSG_SHUTDOWN 0x0113u         //!< conv_host_wire_shutdown -> conv_host_wire_status, then exit 0
#define CONV_HOST_MSG_JOB_RELEASE 0x0114u      //!< conv_host_wire_job_release -> – (no reply)
#define CONV_HOST_MSG_GET_PROVIDER_STATUS 0x0115u //!< conv_host_wire_index (0) -> conv_host_wire_provider_status


/*
 *
 * Framing.
 *
 */

/*!
 * Precedes every payload on the control channel. FROZEN: sizeof == 56.
 * handles[] semantics: see the file comment (Windows values, POSIX 0 +
 * SCM_RIGHTS).
 */
struct conv_host_wire_frame
{
	uint32_t magic;         //!< CONV_HOST_WIRE_MAGIC
	uint32_t type;          //!< CONV_HOST_MSG_*
	uint32_t payload_bytes; //!< bytes that follow; <= CONV_HOST_WIRE_MAX_PAYLOAD
	uint32_t request_id;    //!< service-chosen, echoed in the reply; 0 = unsolicited
	uint32_t handle_count;  //!< <= CONV_HOST_WIRE_MAX_HANDLES
	uint32_t reserved_0;
	uint64_t handles[CONV_HOST_WIRE_MAX_HANDLES];
};


/*
 *
 * Twins of the two top-level ABI structs the host reports.
 *
 */

/*!
 * Twin of xrt_conv_capability: byte-identical up to est_ram_mb (asserted).
 * struct_size here is the ABI struct's size as the HOST saw it. FROZEN at the
 * ABI's current size: a field appended to xrt_conv_capability goes at the
 * tail of conv_host_wire_describe_reply, not in here. sizeof == 360.
 */
struct conv_host_wire_capability
{
	uint32_t struct_size;
	uint32_t cap_id;
	uint32_t task;
	uint32_t lanes;
	uint32_t input_kinds;
	uint32_t outputs;
	uint32_t flags;
	uint32_t import_memory_kinds;
	uint32_t import_sync_kinds;
	uint32_t input_formats;
	uint32_t depth_output_format;
	uint32_t min_views, max_views;
	uint32_t max_streams;
	uint32_t max_concurrent;
	uint32_t max_input_edge;
	uint32_t preferred_input_edge;
	uint32_t depth_kind;
	uint32_t depth_measure;
	uint32_t intrinsics_source;
	uint32_t splat_formats;
	uint32_t licence_class;
	uint32_t commercial_ok;
	char licence_spdx[48];
	char model_id[XRT_CONV_NAME_MAX];
	char model_version[32];
	char backend[32];
	char display_name[XRT_CONV_NAME_MAX];
	uint32_t reserved_0;
	uint64_t device_luid;
	uint32_t est_warmup_ms;
	uint32_t est_latency_ms;
	uint32_t est_vram_mb;
	uint32_t est_ram_mb;
};

//! Twin of xrt_conv_state, byte-identical (asserted). sizeof == 208.
struct conv_host_wire_state
{
	uint32_t struct_size;
	uint32_t state;
	uint32_t failure_reason;
	uint32_t retryable;
	float progress;
	uint32_t reserved_0;
	uint64_t eta_ns;
	char version[48];
	char detail[XRT_CONV_DETAIL_MAX];
};


/*
 *
 * Control payloads.
 *
 */

//! Generic reply. sizeof == 16.
struct conv_host_wire_status
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result; //!< xrt_result_t
	uint32_t reserved_0;
};

//! One u32 argument (DESCRIBE index, UNREGISTER_IMAGE index, LANE_CLOSE lane). sizeof == 16.
struct conv_host_wire_index
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t index;
	uint32_t reserved_0;
};

/*!
 * HELLO (H -> S, first frame). The service checks: the peer is the process it
 * spawned (os_channel_peer_pid), family_version, git_tag == its own,
 * provider_id == the one it spawned. sizeof == 168.
 */
struct conv_host_wire_hello
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t host_pid;
	uint32_t provider_api_version; //!< negotiated XRT_CONVERSION_PLUGIN_API_VERSION_*
	uint32_t flags;                //!< CONV_HOST_HELLO_*
	uint32_t capability_count;     //!< describe_cap count; the service DESCRIBEs each
	uint64_t generation;           //!< the provider's describe generation
	uint64_t adapter_luid;         //!< the adapter the host opened (0 = CPU only)
	char git_tag[CONV_HOST_WIRE_TAG_MAX];
	char provider_id[CONV_HOST_WIRE_TAG_MAX];
};

//! HELLO_ACK (S -> H). A host told accepted == 0 exits CONV_HOST_EXIT_ABI_REJECT. sizeof == 80.
struct conv_host_wire_hello_ack
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t accepted;
	uint32_t reject_reason; //!< CONV_HOST_REJECT_*
	char git_tag[CONV_HOST_WIRE_TAG_MAX];
};

//! DESCRIBE reply. sizeof == 384.
struct conv_host_wire_describe_reply
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result; //!< xrt_result_t of describe_cap
	uint32_t count; //!< describe_cap's *out_count
	uint64_t generation;
	struct conv_host_wire_capability capability;
};

//! GET_STATE / WARM / EVICT request. sizeof == 16.
struct conv_host_wire_cap_request
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t cap_id;
	uint32_t arg; //!< WARM: the variant (quality hint); else 0
};

//! GET_STATE reply. sizeof == 224.
struct conv_host_wire_state_reply
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result;
	uint32_t cap_id;
	struct conv_host_wire_state state;
};

/*!
 * STATE_CHANGED (H -> S): one capability's readiness moved, or (cap_id 0) the
 * describe generation moved and the service re-DESCRIBEs. The service caches
 * both, so [C] calls answer from memory. sizeof == 232.
 */
struct conv_host_wire_state_changed
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t cap_id; //!< 0 = generation only
	uint32_t reserved_0;
	uint64_t generation;
	struct conv_host_wire_state state;
};

//! CANCEL: cooperative cancel of an in-flight WARM (is_cancelled turns true). sizeof == 16.
struct conv_host_wire_cancel
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t request_id; //!< the WARM's frame request_id
	uint32_t reserved_0;
};

/*!
 * STREAM_OPEN: stream_create on the host. @c stream is the SERVICE's id for
 * the stream; the host maps it to the provider's handle and every later
 * message and ring record uses the service id. The xrt_conv_stream_info
 * fields follow in its order. sizeof == 56.
 */
struct conv_host_wire_stream_open
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t cap_id;
	uint32_t reserved_0;
	uint64_t stream;
	uint32_t input_kind;
	uint32_t outputs;
	uint32_t content_hint;
	uint32_t view_count;
	uint32_t views_layout;
	uint32_t max_input_width, max_input_height;
	uint32_t reserved_1;
};

//! STREAM_RESET / STREAM_CLOSE. sizeof == 16.
struct conv_host_wire_stream_ref
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream;
};

//! GET_OUTPUT_DESC request. sizeof == 24.
struct conv_host_wire_output_desc_request
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream;
	uint32_t width, height; //!< input extent
};

//! GET_OUTPUT_DESC reply: xrt_conv_output_desc's fields in its order. sizeof == 64.
struct conv_host_wire_output_desc_reply
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result;
	uint32_t reserved_0;
	uint32_t views_width, views_height;
	uint32_t views_format;
	uint32_t depth_width, depth_height;
	uint32_t depth_format;
	uint32_t confidence_width, confidence_height, confidence_format;
	uint32_t disparity_width, disparity_height, disparity_format;
};

/*!
 * REGISTER_IMAGE (S -> H): an input slot, opened once per slot lifetime.
 * Handles: [0] the image memory (absent for MEM_CPU), then [1] its sync
 * object when image.sync_kind has one. sizeof == 112.
 *
 * MEM_CPU convention: the bytes live in a LANE's shm section (LANE_OPEN
 * handles[0]), image.shm_offset bytes from its start; the record carries no
 * lane. The host resolves the offset against the shm of the lane the submit
 * record arrives on, at convert time (bounds-checked against shm_bytes); a
 * JOB_SUBMIT, which arrives on no lane, resolves it against the
 * lowest-numbered open lane. So a CPU input slot is valid on every lane whose
 * shm holds its bytes at that offset; the service lays them out accordingly.
 */
struct conv_host_wire_register_image
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t index; //!< service-chosen slot index; referenced by ring records
	uint32_t reserved_0;
	struct xrt_conv_wire_image image;
};

/*!
 * REGISTER_SYNC (S -> H: an input fence) or SYNC (H -> S: an output fence the
 * host created). Handles: [0] the shared fence. Fence VALUES travel per ring
 * record. The service never enqueues a GPU wait on a host-signalled fence: it
 * observes host fences from the CPU, with a timeout (§3.5 fence rule).
 * sizeof == 32.
 */
struct conv_host_wire_sync
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t index;     //!< sync index (namespace per direction)
	uint32_t kind;      //!< XRT_CONV_SYNC_*
	uint32_t direction; //!< CONV_HOST_SYNC_*
	uint32_t reserved_0;
	uint64_t initial_value;
};

/*!
 * ALLOC_OUTPUTS (S -> H): allocate @c set_count output sets for a stream at
 * this input extent, on the host's adapter. The host answers with one
 * OUTPUT_IMAGE per image (and one SYNC per output fence), all carrying this
 * request's id, then a conv_host_wire_status. Any earlier set of the stream
 * is released by the host after this reply. sizeof == 32.
 */
struct conv_host_wire_alloc_outputs
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream;
	uint32_t width, height;
	uint32_t set_count;
	uint32_t reserved_0;
};

/*!
 * OUTPUT_IMAGE (H -> S): one host-allocated output. Handles: [0] the image
 * memory, a value in the HOST's process on Windows (the service pulls it).
 * sizeof == 120.
 */
struct conv_host_wire_output_image
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t stream;
	uint32_t index; //!< host-chosen output index; referenced by completion records
	uint32_t role;  //!< CONV_HOST_WIRE_OUT_*
	struct xrt_conv_wire_image image;
};

/*!
 * LANE_OPEN (S -> H): attach a data lane. The shm holds the command ring at
 * submit_ring_offset and the completion ring at complete_ring_offset, each
 * laid out by u_shm_ring_init with the record sizes given here. Handles: [0]
 * the shm section, [1] the wake signal service -> host (command ring
 * non-empty), [2] the wake signal host -> service (completion ring
 * non-empty). sizeof == 56.
 */
struct conv_host_wire_lane_open
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t lane;      //!< service-chosen lane index
	uint32_t lane_kind; //!< CONV_HOST_LANE_*
	uint32_t ring_capacity;
	uint32_t submit_record_bytes;   //!< sizeof(conv_host_wire_submit_record)
	uint32_t complete_record_bytes; //!< sizeof(conv_host_wire_complete_record)
	uint32_t reserved_0;
	uint64_t shm_bytes;
	uint64_t submit_ring_offset;
	uint64_t complete_ring_offset;
};

/*!
 * JOB_SUBMIT (S -> H): job_execute on registered input images. The reply,
 * JOB_SUBMIT | REPLY_BIT, is a conv_host_wire_job_done sent when the job
 * finishes. sizeof == 280.
 */
struct conv_host_wire_job_submit
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
	uint32_t cap_id;
	uint32_t input_kind;
	uint32_t image_count;
	uint32_t requested_blob_format;
	uint32_t image_idx[XRT_CONV_MAX_FRAME_IMAGES];
	uint32_t image_role[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_wire_params params;
};

//! JOB_CANCEL. sizeof == 16.
struct conv_host_wire_job_ref
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
};

/*!
 * JOB_RELEASE (S -> H, no reply): the service is done with job @c job_id's
 * result (it pulled the blob handle of the JOB_SUBMIT reply on Windows, or
 * received it as SCM_RIGHTS on POSIX). The host closes its own copy of the
 * blob section. Without it the host keeps the last few results open and
 * closes the oldest (a fallback, not the contract). sizeof == 16.
 */
struct conv_host_wire_job_release
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
};

/*!
 * GET_PROVIDER_STATUS reply: the provider's get_provider_status, as the host
 * process reads it (platform state, FALLBACK / REFERENCE flags, hint), so a
 * satellite provider ranks and reports like an in-service one. @c reported
 * 0 = the provider has no get_provider_status (or it returned false).
 * sizeof == 152.
 */
struct conv_host_wire_provider_status
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result;
	uint32_t reported;
	uint32_t state; //!< xrt_conv_provider_status::state
	uint32_t flags; //!< XRT_CONV_PROVIDER_FLAG_*
	char hint[128];
};

//! JOB_PROGRESS (H -> S). sizeof == 56.
struct conv_host_wire_job_progress
{
	uint32_t struct_size;
	uint32_t family_version;
	uint64_t job_id;
	float fraction;
	uint32_t reserved_0;
	char stage[XRT_CONV_WIRE_STAGE_MAX];
};

/*!
 * JOB_SUBMIT reply. Handles: [0] a host-created shm holding the blob
 * (blob_bytes long) when blob_bytes > 0; a HOST value on Windows (pulled).
 * sizeof == 368.
 */
struct conv_host_wire_job_done
{
	uint32_t struct_size;
	uint32_t family_version;
	int32_t result;
	uint32_t written; //!< XRT_CONV_OUT_*
	uint64_t job_id;
	uint32_t blob_format;
	uint32_t reserved_0;
	uint64_t blob_bytes;
	struct xrt_conv_wire_splat_info splat_info;
	struct xrt_conv_wire_depth_info depth_info;
	struct xrt_conv_wire_views_info views_info;
	uint32_t reserved_1;
};

//! SHUTDOWN. Orderly: cancel, drain, destroy the provider, reply, exit 0 within 3 s. sizeof == 16.
struct conv_host_wire_shutdown
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t orderly;
	uint32_t reserved_0;
};

//! LOG (H -> S). sizeof == 496.
struct conv_host_wire_log
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t level; //!< enum u_logging_level value
	uint32_t reserved_0;
	char text[CONV_HOST_WIRE_LOG_MAX];
};

/*!
 * HEARTBEAT (H -> S), every CONV_HOST_WIRE_HEARTBEAT_PERIOD_MS: the
 * conv_host_heartbeat view, plus the host clock (its own monotonic ns; only
 * differences are meaningful). The supervisor's watchdog keys off
 * oldest_since_ns and the arrival of heartbeats. sizeof == 64.
 */
struct conv_host_wire_heartbeat
{
	uint32_t struct_size;
	uint32_t family_version;
	uint32_t in_flight;
	uint32_t queued;
	uint32_t oldest_op;     //!< CONV_OP_* (conv_host_iface.h); 0 = none
	uint32_t oldest_cap_id;
	uint64_t oldest_running_ns; //!< how long the oldest call has run (0 = none)
	uint64_t calls_total;
	uint64_t host_time_ns;
	uint32_t vram_used_mb;
	uint32_t exec_threads;
	uint32_t bound_tasks; //!< capabilities with an open stream, a job or a warm pin
	uint32_t reserved_0;
};


/*
 *
 * Ring records (fixed size per ring; no header).
 *
 */

/*!
 * Command ring record (S -> H): one stream_convert. Input images and the input
 * fence are INDICES into what REGISTER_IMAGE / REGISTER_SYNC announced. The
 * host waits for in_fence_value on in_sync_idx before reading the inputs.
 * sizeof == 424.
 */
struct conv_host_wire_submit_record
{
	uint64_t seq;    //!< service-assigned, strictly increasing per lane
	uint64_t stream; //!< service stream id (STREAM_OPEN)
	uint32_t op;     //!< CONV_HOST_RING_OP_*
	uint32_t image_count;
	uint32_t in_image_idx[XRT_CONV_MAX_FRAME_IMAGES];
	uint32_t in_image_role[XRT_CONV_MAX_FRAME_IMAGES]; //!< XRT_CONV_IMG_*
	uint32_t in_sync_idx; //!< CONV_HOST_WIRE_NO_INDEX = inputs complete at hand-over
	uint32_t reserved_0;
	uint64_t in_fence_value;
	uint64_t submit_time_ns; //!< service clock; echoed nowhere, for host-side stats
	struct xrt_conv_wire_params params; //!< incl. the viewpoints (viewpoints_xyz)
	struct xrt_conv_wire_depth_info depth_in_info;
};

/*!
 * Completion ring record (H -> S). A completion says only that the host ISSUED
 * the signal of out_fence_value on out_sync_idx; the service observes the
 * fence from the CPU before it consumes any out image (§3.5 fence rule).
 *
 * Ordering: the host has ONE output fence. Values are reserved in submit order
 * per host, but with several stream lanes converting at once a later value can
 * be signalled before an earlier one, so "fence >= v" does not imply every
 * earlier completion is done. Correct for P2's one stream lane per host; a
 * multi-lane host needs a fence per lane (or per-lane value ranges).
 * sizeof == 320.
 */
struct conv_host_wire_complete_record
{
	uint64_t seq;   //!< the submit's
	int32_t status; //!< xrt_result_t of stream_convert
	uint32_t written; //!< XRT_CONV_OUT_*
	uint32_t out_image_idx[CONV_HOST_WIRE_OUT_COUNT]; //!< by CONV_HOST_WIRE_OUT_*; NO_INDEX = not written
	uint32_t out_sync_idx; //!< CONV_HOST_WIRE_NO_INDEX = outputs complete at hand-over
	uint32_t reserved_0;
	uint64_t out_fence_value;
	uint64_t start_ns, end_ns; //!< host clock around the provider call
	struct xrt_conv_wire_views_info views_info;
	uint32_t reserved_1;
	struct xrt_conv_wire_depth_info depth_info;
};


/*
 *
 * Helpers.
 *
 */

//! Fill a payload's common header.
static inline void
conv_host_wire_init(void *payload, uint32_t size)
{
	uint32_t hdr[2];
	memset(payload, 0, size);
	hdr[0] = size;
	hdr[1] = CONV_HOST_WIRE_FAMILY_VERSION_CURRENT;
	memcpy(payload, hdr, sizeof(hdr));
}

/*!
 * Read a received payload into @p dst (sizeof @p dst_size): validates the
 * header (struct_size >= 8 and <= src_bytes, family_version current), copies
 * min(struct_size, dst_size) and zero-extends; dst's struct_size field is set
 * to dst_size. false = malformed (dst zeroed).
 */
static inline bool
conv_host_wire_receive(void *dst, uint32_t dst_size, const void *src, uint32_t src_bytes)
{
	uint32_t hdr[2];
	uint32_t n;

	if (dst == NULL || dst_size < CONV_HOST_WIRE_HEADER_BYTES) {
		return false;
	}
	memset(dst, 0, dst_size);
	if (src == NULL || src_bytes < CONV_HOST_WIRE_HEADER_BYTES) {
		return false;
	}
	memcpy(hdr, src, sizeof(hdr));
	if (hdr[0] < CONV_HOST_WIRE_HEADER_BYTES || hdr[0] > src_bytes ||
	    hdr[1] != CONV_HOST_WIRE_FAMILY_VERSION_CURRENT) {
		return false;
	}
	n = hdr[0] < dst_size ? hdr[0] : dst_size;
	memcpy(dst, src, n);
	hdr[0] = dst_size;
	memcpy(dst, hdr, sizeof(hdr[0]));
	return true;
}

//! Validate a received frame header (magic, sizes, handle count).
static inline bool
conv_host_wire_frame_valid(const struct conv_host_wire_frame *f)
{
	return f != NULL && f->magic == CONV_HOST_WIRE_MAGIC && f->payload_bytes <= CONV_HOST_WIRE_MAX_PAYLOAD &&
	       f->handle_count <= CONV_HOST_WIRE_MAX_HANDLES;
}

//! Bounded copy into a fixed char field, always NUL-terminated.
static inline void
conv_host_wire_set_str(char *dst, size_t dst_size, const char *src)
{
	size_t n = 0;
	if (dst_size == 0) {
		return;
	}
	if (src != NULL) {
		while (n + 1 < dst_size && src[n] != '\0') {
			n++;
		}
		memcpy(dst, src, n);
	}
	memset(dst + n, 0, dst_size - n);
}

//! xrt_conv_capability -> wire (every field; struct_size = what the host saw).
static inline void
conv_host_wire_pack_capability(const struct xrt_conv_capability *in, struct conv_host_wire_capability *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = in->struct_size;
	out->cap_id = in->cap_id;
	out->task = in->task;
	out->lanes = in->lanes;
	out->input_kinds = in->input_kinds;
	out->outputs = in->outputs;
	out->flags = in->flags;
	out->import_memory_kinds = in->import_memory_kinds;
	out->import_sync_kinds = in->import_sync_kinds;
	out->input_formats = in->input_formats;
	out->depth_output_format = in->depth_output_format;
	out->min_views = in->min_views;
	out->max_views = in->max_views;
	out->max_streams = in->max_streams;
	out->max_concurrent = in->max_concurrent;
	out->max_input_edge = in->max_input_edge;
	out->preferred_input_edge = in->preferred_input_edge;
	out->depth_kind = in->depth_kind;
	out->depth_measure = in->depth_measure;
	out->intrinsics_source = in->intrinsics_source;
	out->splat_formats = in->splat_formats;
	out->licence_class = in->licence_class;
	out->commercial_ok = in->commercial_ok;
	memcpy(out->licence_spdx, in->licence_spdx, sizeof(out->licence_spdx));
	memcpy(out->model_id, in->model_id, sizeof(out->model_id));
	memcpy(out->model_version, in->model_version, sizeof(out->model_version));
	memcpy(out->backend, in->backend, sizeof(out->backend));
	memcpy(out->display_name, in->display_name, sizeof(out->display_name));
	out->licence_spdx[sizeof(out->licence_spdx) - 1] = '\0';
	out->model_id[sizeof(out->model_id) - 1] = '\0';
	out->model_version[sizeof(out->model_version) - 1] = '\0';
	out->backend[sizeof(out->backend) - 1] = '\0';
	out->display_name[sizeof(out->display_name) - 1] = '\0';
	out->reserved_0 = in->reserved_0;
	out->device_luid = in->device_luid;
	out->est_warmup_ms = in->est_warmup_ms;
	out->est_latency_ms = in->est_latency_ms;
	out->est_vram_mb = in->est_vram_mb;
	out->est_ram_mb = in->est_ram_mb;
}

//! wire -> xrt_conv_capability (zeroed, struct_size = its sizeof; strings re-terminated).
static inline void
conv_host_wire_unpack_capability(const struct conv_host_wire_capability *in, struct xrt_conv_capability *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->cap_id = in->cap_id;
	out->task = in->task;
	out->lanes = in->lanes;
	out->input_kinds = in->input_kinds;
	out->outputs = in->outputs;
	out->flags = in->flags;
	out->import_memory_kinds = in->import_memory_kinds;
	out->import_sync_kinds = in->import_sync_kinds;
	out->input_formats = in->input_formats;
	out->depth_output_format = in->depth_output_format;
	out->min_views = in->min_views;
	out->max_views = in->max_views;
	out->max_streams = in->max_streams;
	out->max_concurrent = in->max_concurrent;
	out->max_input_edge = in->max_input_edge;
	out->preferred_input_edge = in->preferred_input_edge;
	out->depth_kind = in->depth_kind;
	out->depth_measure = in->depth_measure;
	out->intrinsics_source = in->intrinsics_source;
	out->splat_formats = in->splat_formats;
	out->licence_class = in->licence_class;
	out->commercial_ok = in->commercial_ok;
	memcpy(out->licence_spdx, in->licence_spdx, sizeof(out->licence_spdx));
	memcpy(out->model_id, in->model_id, sizeof(out->model_id));
	memcpy(out->model_version, in->model_version, sizeof(out->model_version));
	memcpy(out->backend, in->backend, sizeof(out->backend));
	memcpy(out->display_name, in->display_name, sizeof(out->display_name));
	out->licence_spdx[sizeof(out->licence_spdx) - 1] = '\0';
	out->model_id[sizeof(out->model_id) - 1] = '\0';
	out->model_version[sizeof(out->model_version) - 1] = '\0';
	out->backend[sizeof(out->backend) - 1] = '\0';
	out->display_name[sizeof(out->display_name) - 1] = '\0';
	out->reserved_0 = in->reserved_0;
	out->device_luid = in->device_luid;
	out->est_warmup_ms = in->est_warmup_ms;
	out->est_latency_ms = in->est_latency_ms;
	out->est_vram_mb = in->est_vram_mb;
	out->est_ram_mb = in->est_ram_mb;
}

//! xrt_conv_state -> wire (every field).
static inline void
conv_host_wire_pack_state(const struct xrt_conv_state *in, struct conv_host_wire_state *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = in->struct_size;
	out->state = in->state;
	out->failure_reason = in->failure_reason;
	out->retryable = in->retryable;
	out->progress = in->progress;
	out->reserved_0 = in->reserved_0;
	out->eta_ns = in->eta_ns;
	memcpy(out->version, in->version, sizeof(out->version));
	memcpy(out->detail, in->detail, sizeof(out->detail));
	out->version[sizeof(out->version) - 1] = '\0';
	out->detail[sizeof(out->detail) - 1] = '\0';
}

//! wire -> xrt_conv_state (zeroed, struct_size = its sizeof).
static inline void
conv_host_wire_unpack_state(const struct conv_host_wire_state *in, struct xrt_conv_state *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->state = in->state;
	out->failure_reason = in->failure_reason;
	out->retryable = in->retryable;
	out->progress = in->progress;
	out->reserved_0 = in->reserved_0;
	out->eta_ns = in->eta_ns;
	memcpy(out->version, in->version, sizeof(out->version));
	memcpy(out->detail, in->detail, sizeof(out->detail));
	out->version[sizeof(out->version) - 1] = '\0';
	out->detail[sizeof(out->detail) - 1] = '\0';
}

//! xrt_conv_output_desc -> reply fields (header and result left to the caller).
static inline void
conv_host_wire_pack_output_desc(const struct xrt_conv_output_desc *in, struct conv_host_wire_output_desc_reply *out)
{
	out->views_width = in->views_width;
	out->views_height = in->views_height;
	out->views_format = in->views_format;
	out->depth_width = in->depth_width;
	out->depth_height = in->depth_height;
	out->depth_format = in->depth_format;
	out->confidence_width = in->confidence_width;
	out->confidence_height = in->confidence_height;
	out->confidence_format = in->confidence_format;
	out->disparity_width = in->disparity_width;
	out->disparity_height = in->disparity_height;
	out->disparity_format = in->disparity_format;
}

//! Reply fields -> xrt_conv_output_desc (zeroed, struct_size = its sizeof).
static inline void
conv_host_wire_unpack_output_desc(const struct conv_host_wire_output_desc_reply *in, struct xrt_conv_output_desc *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->views_width = in->views_width;
	out->views_height = in->views_height;
	out->views_format = in->views_format;
	out->depth_width = in->depth_width;
	out->depth_height = in->depth_height;
	out->depth_format = in->depth_format;
	out->confidence_width = in->confidence_width;
	out->confidence_height = in->confidence_height;
	out->confidence_format = in->confidence_format;
	out->disparity_width = in->disparity_width;
	out->disparity_height = in->disparity_height;
	out->disparity_format = in->disparity_format;
}


/*
 *
 * Tripwires.
 *
 */
// clang-format off
#define CONV_HOST_WIRE_TWIN(W, A, F)                                                                \
	XRT_CONV_ABI_ASSERT(offsetof(struct W, F) == offsetof(struct A, F) &&                          \
	                    sizeof(((struct W *)0)->F) == sizeof(((struct A *)0)->F),                  \
	                    #W "." #F " must match " #A "." #F)

XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_capability) == 360, "conv_host_wire_capability is FROZEN");
XRT_CONV_ABI_ASSERT(sizeof(struct xrt_conv_capability) >= sizeof(struct conv_host_wire_capability), "capability twin covers the ABI head");
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, struct_size);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, cap_id);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, task);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, lanes);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, input_kinds);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, outputs);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, flags);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, import_memory_kinds);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, import_sync_kinds);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, input_formats);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, depth_output_format);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, min_views);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, max_views);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, max_streams);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, max_concurrent);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, max_input_edge);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, preferred_input_edge);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, depth_kind);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, depth_measure);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, intrinsics_source);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, splat_formats);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, licence_class);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, commercial_ok);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, licence_spdx);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, model_id);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, model_version);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, backend);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, display_name);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, reserved_0);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, device_luid);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, est_warmup_ms);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, est_latency_ms);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, est_vram_mb);
CONV_HOST_WIRE_TWIN(conv_host_wire_capability, xrt_conv_capability, est_ram_mb);

XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_state) == sizeof(struct xrt_conv_state), "state twin size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_state) == 208, "conv_host_wire_state is FROZEN");
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, struct_size);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, state);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, failure_reason);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, retryable);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, progress);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, reserved_0);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, eta_ns);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, version);
CONV_HOST_WIRE_TWIN(conv_host_wire_state, xrt_conv_state, detail);

/* Exact sizes: no hidden padding. */
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_frame) == 56, "conv_host_wire_frame is FROZEN");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_status) == 16, "status size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_index) == 16, "index size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_hello) == 168, "hello size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_hello_ack) == 80, "hello_ack size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_describe_reply) == 384, "describe reply size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_describe_reply, capability) == 24, "describe reply: capability at 24");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_cap_request) == 16, "cap request size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_state_reply) == 224, "state reply size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_state_reply, state) == 16, "state reply: state at 16");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_state_changed) == 232, "state changed size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_state_changed, state) == 24, "state changed: state at 24");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_cancel) == 16, "cancel size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_stream_open) == 56, "stream open size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_stream_ref) == 16, "stream ref size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_output_desc_request) == 24, "output desc request size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_output_desc_reply) == 64, "output desc reply size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_register_image) == 112, "register image size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_register_image, image) == 16, "register image: image at 16");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_sync) == 32, "sync size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_alloc_outputs) == 32, "alloc outputs size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_output_image) == 120, "output image size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_output_image, image) == 24, "output image: image at 24");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_lane_open) == 56, "lane open size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_submit) == 280, "job submit size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_job_submit, params) == 64, "job submit: params at 64");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_ref) == 16, "job ref size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_release) == 16, "job release size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_job_release, job_id) == 8, "job release layout");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_provider_status) == 152, "provider status size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_provider_status, hint) == 24, "provider status layout");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_progress) == 56, "job progress size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_done) == 368, "job done size");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_job_done, depth_info) == 112, "job done: depth_info at 112");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_shutdown) == 16, "shutdown size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_log) == 496, "log size");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_heartbeat) == 64, "heartbeat size");

XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_submit_record) == 424, "submit record is FROZEN");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_submit_record, params) == 80, "submit record: params at 80");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_submit_record, depth_in_info) == 296, "submit record: depth_in_info at 296");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_complete_record) == 320, "complete record is FROZEN");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_complete_record, views_info) == 64, "complete record: views_info at 64");
XRT_CONV_ABI_ASSERT(offsetof(struct conv_host_wire_complete_record, depth_info) == 192, "complete record: depth_info at 192");
XRT_CONV_ABI_ASSERT((sizeof(struct conv_host_wire_submit_record) % 8) == 0, "ring records are 8-aligned");
XRT_CONV_ABI_ASSERT((sizeof(struct conv_host_wire_complete_record) % 8) == 0, "ring records are 8-aligned");
XRT_CONV_ABI_ASSERT((CONV_HOST_WIRE_RING_CAPACITY & (CONV_HOST_WIRE_RING_CAPACITY - 1)) == 0, "ring capacity is a power of two");

/* Every payload fits one frame. */
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_log) <= CONV_HOST_WIRE_MAX_PAYLOAD, "log > max payload");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_describe_reply) <= CONV_HOST_WIRE_MAX_PAYLOAD, "describe > max payload");
XRT_CONV_ABI_ASSERT(sizeof(struct conv_host_wire_job_done) <= CONV_HOST_WIRE_MAX_PAYLOAD, "job done > max payload");
// clang-format on

#ifdef __cplusplus
}
#endif
