// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-adapter atlas bridge for the output-device split (#918).
 * @ingroup comp_xbridge
 *
 * Mechanics ported from `scripts/hybrid_gpu_bench/xbridge12_bench.cpp`, the tool
 * that established on this hardware that D3D11 has no cross-adapter texture path
 * at all while D3D12 cross-adapter heaps work at 3.04 GB/s — 3x what a
 * per-app-frame atlas needs at 60 Hz. See the header for the topology.
 */

#include "comp_xbridge.h"
#include "comp_xbridge_plane_policy.h"

#include "util/u_logging.h"
#include "util/u_crash_guard.h"
#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <thread>

//! Cross-adapter placed-texture ring. Two is enough: the producer for seq+2
//! cannot start before the app has submitted seq+2, by which time the consumer's
//! seq copy is long retired (present is serialised under the compositor lock and
//! DXGI max frame latency is 1).
#define XB_XA_RING 2
//! Egress ring on the output device. Three gives the weave (and any repaint
//! standing in for it) two full frames of margin before its slot is rewritten.
#define XB_EGRESS_RING 3
//! Command allocators per D3D12 device — one per in-flight seq.
#define XB_ALLOC_RING 3
//! Option II ingress staging ring on the app device.
#define XB_INGRESS_RING 2

/*!
 * #1178 — the atlas format a caller that names none gets. It is a DEFAULT, not
 * the format: the whole atlas chain is now allocated in
 * @ref comp_xbridge_info::atlas_format, because a copy between two DXGI typeless
 * families is silently dropped rather than reported (see the header).
 */
#define XB_FORMAT_DEFAULT DXGI_FORMAT_R8G8B8A8_UNORM

//! #1178 — how often a refused submit may re-log. The first refusal logs at
//! once; after that this bounds an otherwise per-frame U_LOG_E.
#define XB_FMT_REFUSE_LOG_NS (5ull * 1000ull * 1000ull * 1000ull)

//! #1178 content probe — the readback square, and how often it may report.
#define XB_PROBE_EXTENT 64u
#define XB_PROBE_PERIOD_NS (1000ull * 1000ull * 1000ull)

/*!
 * #918 Phase 2a bandwidth gate. The acceptance number for the whole transport,
 * atlas + planes, measured over a rolling second. A session that exceeds it
 * latches its largest PLANE to half rate (one WARN) rather than disabling the
 * split — the split's win is the repaint arm and the local present, neither of
 * which a plane's rate affects.
 */
#define XB_BANDWIDTH_GATE_BYTES_PER_S (2000ull * 1000ull * 1000ull)
/*!
 * #918 review F7 — the gate's only lever is a plane's rate, so it must not fire
 * on a total the planes barely contribute to. Below this plane rate the atlas IS
 * the cost (a 4-view 4K atlas is ~4 GB/s at 120 Hz by itself) and halving a plane
 * would spend 2D update rate to change nothing measurable.
 */
#define XB_GATE_PLANE_FLOOR_BYTES_PER_S (250ull * 1000ull * 1000ull)
//! #918 review F7 — how long the transport must stay inside the gate before a
//! half-rate latch is released. The latch is pressure relief, not a session-long
//! sentence for one busy second.
#define XB_GATE_UNLATCH_NS (5ull * 1000ull * 1000ull * 1000ull)

//! #918 R2: two content-box changes closer together than this are a drag, not a
//! mode switch — stop reallocating the egress ring per size.
#define XB_CHURN_WINDOW_NS (500ull * 1000ull * 1000ull)
//! …and this long without a change is the drag ending: one realloc back to a
//! content-sized ring.
#define XB_SETTLE_NS (500ull * 1000ull * 1000ull)

namespace {

template <class T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

void
safe_close(HANDLE &h)
{
	if (h != nullptr) {
		CloseHandle(h);
		h = nullptr;
	}
}

D3D12_TEXTURE_COPY_LOCATION
sub_loc(ID3D12Resource *res)
{
	D3D12_TEXTURE_COPY_LOCATION l{};
	l.pResource = res;
	l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	l.SubresourceIndex = 0;
	return l;
}

} // namespace

/*!
 * Which ingress flavour is live. Option I is the default and touches nothing on
 * the app's render path; Option II costs one extra app-device copy per frame and
 * exists only so an unshareable atlas cannot brick the split.
 */
enum xb_ingress_mode
{
	XB_INGRESS_DIRECT = 1, //!< Option I — producer reads the renderer atlas itself.
	XB_INGRESS_STAGED = 2, //!< Option II — app-device NT-shared staging ring.
	/*!
	 * #918 Phase 2b PR 6 — both, chosen per frame. The staging ring is
	 * allocated (so any frame may fall back to it) AND a source may be bound
	 * Option-I style. See the header for who wants this and why.
	 */
	XB_INGRESS_ADAPTIVE = 3,
};

/*!
 * Superseded Option-I opens waiting on the producer, under adaptive ingress.
 *
 * Releasing the open of the source the producer's last copy is still reading is
 * a use-after-free on the copy queue, and the existing fix for that (a bounded
 * CPU drain, #918 F2) is not available here: a source change happens on the
 * SERVICE's render thread under `render_mutex`, where an unbounded — or even a
 * 2-second — stall is exactly the wedge class #925 exists for. Deferring the
 * release behind the producer fence instead costs nothing and never blocks. Four
 * is far more than a user can generate: each entry is one focus change, and one
 * frame later the producer has retired it.
 */
#define XB_SRC_RETIRE 4

/*!
 * #918 PR 6 — the REBIND SETTLE window, and why adaptive ingress needs one.
 *
 * The service's Option-I sources are its CROP TEXTURES, and a crop texture is
 * reallocated whenever the content box moves — which during an interactive
 * resize drag is every mouse event. A new allocation is a new ingress key, so
 * the naive rule ("key changed ⟹ re-open") turns a drag into one
 * `OpenSharedHandle` and one retire-list push PER FRAME. The retire ring sweeps
 * on every nomination and normally keeps up, but a producer that falls even
 * slightly behind fills it, and the exhausted-ring fallback is the bounded
 * drain — a 2 s CPU wait on the render thread under `render_mutex`. Dragging a
 * window must not be able to reach that.
 *
 * So a key change RETIRES the old open once and then only ARMS the new key; the
 * re-open happens after the key has held still this long. Through a drag there
 * is simply no bound source and every frame stages, which is exactly what the
 * bridge did before adaptive ingress existed and is the right degrade. Same
 * shape as the R2 egress-ring hysteresis this sits beside (#1091).
 */
#define XB_SRC_SETTLE_NS (250ull * 1000ull * 1000ull)

struct comp_xbridge
{
	/*!
	 * #918 D12-3a — the ends are D3D12, not D3D11. See the header's topology.
	 *
	 * The invariant that makes the branching below tractable: in D3D12 mode every
	 * `ID3D11*` member of this struct stays NULL, and `prod_dev` / `cons_dev` are
	 * AddRef'd ALIASES of `app_dev12` / `out_dev12` rather than devices the bridge
	 * created. Everything between them — the cross-adapter heap ring, the placed
	 * resources, both copy queues, the allocator rings, the fences f_xa_* and
	 * f_out_cons, the watchdog and the bandwidth gate — is bit-for-bit the same
	 * code on both flavours, because the middle of the transport never moves.
	 */
	bool d3d12_ends;

	// --- D3D12 ends (#918 D12-3a) — NULL unless d3d12_ends ---------------
	//! The application's device. `prod_dev` is an alias of this.
	ID3D12Device *app_dev12;
	//! The application's own queue: signalled in submit, GPU-waited in pre_render.
	ID3D12CommandQueue *app_q12;
	//! The runtime's scanout-adapter device. `cons_dev` is an alias of this.
	ID3D12Device *out_dev12;
	//! The queue the weave records onto; gpu_wait_slot's GPU-side wait lands here.
	ID3D12CommandQueue *out_q12;

	// --- D3D11 ends -----------------------------------------------------
	ID3D11Device *app_dev;
	ID3D11Device5 *app_dev5;
	ID3D11DeviceContext *app_ctx;
	ID3D11DeviceContext4 *app_ctx4;
	ID3D11Device *out_dev;
	ID3D11Device5 *out_dev5;
	ID3D11DeviceContext *out_ctx;
	//! Cached at create — the weave path must not QI per call (#918 F9).
	ID3D11DeviceContext4 *out_ctx4;

	// --- D3D12 middle ---------------------------------------------------
	ID3D12Device *prod_dev;
	ID3D12Device *cons_dev;
	ID3D12CommandQueue *prod_q;
	ID3D12CommandQueue *cons_q;
	ID3D12CommandAllocator *prod_alloc[XB_ALLOC_RING];
	ID3D12CommandAllocator *cons_alloc[XB_ALLOC_RING];
	ID3D12GraphicsCommandList *prod_list;
	ID3D12GraphicsCommandList *cons_list;
	uint64_t prod_alloc_seq[XB_ALLOC_RING];
	uint64_t cons_alloc_seq[XB_ALLOC_RING];

	// --- fences (one monotonic seq: the app frame counter) --------------
	//! App D3D11 -> producer queue. Signalled on the app's immediate context.
	//! D3D12 ends: `f_app` / `f_app_h` stay NULL and `f_app_prod` is a plain
	//! unshared fence on the app device (== the producer device), signalled from
	//! `app_q12`. Same fence, same seq, one fewer share.
	ID3D11Fence *f_app;
	ID3D12Fence *f_app_prod;
	HANDLE f_app_h;
	//! Producer -> consumer, the cross-adapter one the watchdog polls.
	ID3D12Fence *f_xa_prod;
	ID3D12Fence *f_xa_cons;
	HANDLE f_xa_h;
	//! Same fence re-opened on the APP D3D11 device (same adapter as the
	//! producer). The back-fence of ingress Option I: the app's renderer passes
	//! GPU-wait on it so frame N cannot overwrite an atlas the producer's copy
	//! of N-1 may still be reading (#918 F6).
	//! D3D12 ends: NULL. The app device IS the producer device, so `f_xa_prod`
	//! itself is the back-fence and `app_back_wait_ok` is unconditionally true —
	//! there is no open that can fail and therefore no unsafe-Option-I case.
	ID3D11Fence *f_xa_app;
	bool app_back_wait_ok;
	uint64_t app_waited_seq;
	//! Consumer completion. Polled CPU-side to pick a slot; ALSO opened on the
	//! output D3D11 device so the weave can take a GPU-side wait (never a CPU
	//! one) on the slot it picked.
	//! D3D12 ends: `f_out_out11` / `f_out_h` stay NULL — the out queue lives on
	//! the consumer device, so it waits on `f_out_cons` directly and
	//! `out_gpu_wait_ok` is unconditionally true.
	ID3D12Fence *f_out_cons;
	ID3D11Fence *f_out_out11;
	HANDLE f_out_h;
	bool out_gpu_wait_ok;

	// --- cross-adapter ring ---------------------------------------------
	ID3D12Heap *xa_heap_prod[XB_XA_RING];
	ID3D12Heap *xa_heap_cons[XB_XA_RING];
	HANDLE xa_heap_h[XB_XA_RING];
	ID3D12Resource *xa_prod[XB_XA_RING];
	ID3D12Resource *xa_cons[XB_XA_RING];

	// --- egress ring (output D3D11, opened by the consumer) --------------
	//! D3D12 ends: `eg_tex` / `eg_srv` / `eg_share` stay NULL and `eg_12` holds
	//! plain committed ALLOW_SIMULTANEOUS_ACCESS resources created straight on
	//! the consumer (== output) device. Same slot indices, same seq/gen stamps,
	//! same release path — `safe_release` / `safe_close` on a NULL is a no-op.
	ID3D11Texture2D *eg_tex[XB_EGRESS_RING];
	ID3D11ShaderResourceView *eg_srv[XB_EGRESS_RING];
	HANDLE eg_share[XB_EGRESS_RING]; //!< NT share handles of eg_tex
	ID3D12Resource *eg_12[XB_EGRESS_RING];
	uint64_t eg_seq[XB_EGRESS_RING];
	/*!
	 * #918 R1: the LAYOUT that produced each slot's content.
	 *
	 * "The atlas recipe IS the mode" — the atlas layout and the DP recipe must
	 * switch atomically. Non-split they do: the weave consumes the atlas
	 * composited by the same frame. Under the split the weave consumes whichever
	 * slot has landed, which across a mode switch can belong to the previous
	 * recipe. Stamping the producing layout on the slot is what lets the weave
	 * tell the difference — refuse a superseded one, and recognise the fresh one
	 * it should wait for instead.
	 */
	uint64_t eg_gen[XB_EGRESS_RING];
	//! Content box that produced the slot — the crop/tile stride to weave it
	//! with, which during a resize is NOT the frame's current content box.
	uint32_t eg_cw[XB_EGRESS_RING], eg_ch[XB_EGRESS_RING];
	uint32_t eg_w, eg_h;
	bool eg_content_sized;
	//! #918 F5: the content dims whose allocation last failed. Retried only when
	//! the request changes, so a persistently-failing size costs one WARN and one
	//! attempt, not a realloc + a WARN every frame.
	uint32_t eg_fail_w, eg_fail_h;
	/*!
	 * #918 R2 resize hysteresis. The content box follows the window, so an
	 * interactive resize asks for a NEW size on every mouse event — and a
	 * per-size rebuild is expensive where it stands: a drain of the consumer
	 * fence, then three NT-shared textures, three SRVs and three D3D12 opens
	 * released and recreated, on the frame path. Measured over a 1.2 s edge
	 * drag: 12 rebuilds, 29 frames delivered against the non-split path's 50.
	 * While the dims are churning the ring therefore stays WORST-CASE sized and
	 * the caller crops on the output device instead; one realloc settles it back.
	 */
	uint32_t req_w, req_h;   //!< last size the caller asked for
	uint64_t dims_change_ns; //!< when that request last CHANGED
	//! Layout generation the last request carried. A size change that comes WITH
	//! a new generation is a mode switch (one step, then still) and must not be
	//! mistaken for the continuous size motion of a drag.
	uint64_t size_gen;
	bool churn;             //!< worst-case ring engaged for a churning size
	uint64_t churn_entries; //!< times churn mode engaged
	uint64_t churn_settles; //!< times it settled back to a content-sized ring
	uint64_t eg_reallocs;   //!< egress ring rebuilds (the thing R2 minimises)

	//! #918 Phase 2a — the composite recipe stamped on each slot, and the one
	//! the next submit will stamp. Read back by the consume half so it never
	//! composites a slot's pixels under a later frame's recipe.
	struct comp_xbridge_recipe eg_recipe[XB_EGRESS_RING];
	struct comp_xbridge_recipe staged_recipe;

	// --- planes (#918 Phase 2a) -------------------------------------------
	//! Panel extent — every plane is allocated here ONCE and never resized.
	uint32_t panel_w, panel_h;
	struct xb_plane
	{
		bool live;    //!< allocated and bound; false ⟹ the feature degrades
		bool failed;  //!< allocation refused once — never retried, never re-WARNed
		uint32_t fmt; //!< DXGI_FORMAT of the whole chain
		uint32_t bpp;
		/*!
		 * Allocated extent of the whole chain (XA ring + egress). NOT always the
		 * panel (#918 review F5): the 2D planes are panel-sized once, on purpose,
		 * so they never enter the R2 churn path — but an authored MASK is created
		 * at the app's own dims and maps stretch-to-region, so a panel-sized mask
		 * plane would transport the mask into a corner of a texture the composite
		 * then stretches whole, sampling a never-written band. Sizing the mask
		 * plane at the MASK makes the full mask the thing that crosses, and makes
		 * an uninitialised band structurally impossible.
		 */
		uint32_t alloc_w, alloc_h;

		/*!
		 * The producer's view of the app-device source, Option-I shaped.
		 *
		 * D3D11 ends: the producer's OPEN of an NT-shared app-device texture.
		 * D3D12 ends (#918 D12-4): the caller's own `ID3D12Resource`, AddRef'd —
		 * there is nothing to open, because the producer IS the app device. Same
		 * field, same copy, one fewer indirection; see
		 * @ref comp_xbridge_bind_plane_resource.
		 */
		ID3D12Resource *src12;
		uint64_t src_gen;
		bool src_bound;
		//! One WARN, ever, for a caller that used the wrong bind flavour for this
		//! bridge's ends. Callers bind every frame, so this may not be per-frame.
		bool bind_flavour_warned;
		//! The source's own extent. NOT always the panel: an authored mask is
		//! created at the app's chosen zone dims, and a copy box snapped out past
		//! them is an out-of-bounds source region on the copy queue.
		uint32_t src_w, src_h;

		//! Own cross-adapter heap + placed ring — per plane, so one plane's
		//! allocation failure cannot take the others (or the atlas) with it.
		ID3D12Heap *heap_prod[XB_XA_RING];
		ID3D12Heap *heap_cons[XB_XA_RING];
		HANDLE heap_h[XB_XA_RING];
		ID3D12Resource *xa_prod[XB_XA_RING];
		ID3D12Resource *xa_cons[XB_XA_RING];

		//! Egress, parallel to the atlas ring: same slot index, same seq.
		ID3D11Texture2D *eg_tex[XB_EGRESS_RING];
		ID3D11ShaderResourceView *eg_srv[XB_EGRESS_RING];
		HANDLE eg_share[XB_EGRESS_RING];
		ID3D12Resource *eg_12[XB_EGRESS_RING];
		//! Content generation each slot's pixels carry (0 = never written).
		uint64_t slot_seq[XB_EGRESS_RING];
		//! Dirty box each slot still owes, accumulated across the frames it was
		//! not the write slot. Empty (w==0) means "already current".
		int32_t pend_x[XB_EGRESS_RING], pend_y[XB_EGRESS_RING];
		uint32_t pend_w[XB_EGRESS_RING], pend_h[XB_EGRESS_RING];

		//! Staged for the next submit.
		uint64_t stage_seq;
		int32_t stage_x, stage_y;
		uint32_t stage_w, stage_h;
		bool staged;
		//! The box the last staged content occupied, folded into every slot's
		//! pending box on a change so vacated pixels are refreshed too.
		int32_t last_x, last_y;
		uint32_t last_w, last_h;

		//! `bytes` is the DIAG window (drained by take_plane_stats); copies and
		//! skips are LIFETIME totals (the compositor diffs them, and quiesce
		//! reports them whole). `gate_bytes` is the bandwidth gate's own window,
		//! deliberately independent of the DIAG cadence — DXR_XBRIDGE_DIAG is off
		//! by default and the gate must still fire.
		uint64_t bytes, copies, skips, gate_bytes;
		//! Bandwidth gate: latched to submit every other frame.
		bool half_rate;
		uint64_t half_rate_parity;
	} plane[COMP_XBRIDGE_PLANE_COUNT];
	uint64_t atlas_bytes;
	/*!
	 * Rolling one-second window for the bandwidth gate (independent of the
	 * compositor's DIAG cadence, which may be off). #918 review F7: atlas and
	 * plane bytes are accumulated SEPARATELY — the over-budget test is on the
	 * total, but the decision to throttle is on the planes, because a plane's
	 * rate is the only thing the gate can change.
	 */
	uint64_t gate_window_ns;
	uint64_t gate_atlas_bytes;
	uint64_t gate_plane_bytes;
	//! When the transport last came back inside the gate — the un-latch clock.
	uint64_t gate_under_since_ns;
	//! One WARN for the over-budget-but-not-the-planes case, ever.
	bool gate_atlas_only_warned;

	// --- ingress ---------------------------------------------------------
	int ingress_mode;
	ID3D12Resource *atlas_12;
	uint64_t atlas_gen;
	ID3D11Texture2D *in_tex[XB_INGRESS_RING];
	HANDLE in_h[XB_INGRESS_RING];
	ID3D12Resource *in_12[XB_INGRESS_RING];

	// --- adaptive ingress (#918 Phase 2b PR 6) ---------------------------
	//! Caller-assigned key of the allocation `atlas_12` is an open of. Unique per
	//! allocation for the life of the process — see the header on why a texture
	//! pointer is not a legal key.
	uint64_t src_key;
	//! The next submit may read that source in place. Cleared by every source
	//! change, so the CHANGE frame stages and the bind takes effect after it.
	bool src_armed;
	//! Producer seq of the last submit that DID read a source in place — the only
	//! value the #918 F6 back-fence needs to cover under adaptive ingress.
	uint64_t prod_direct_seq;
	struct
	{
		ID3D12Resource *res;
		uint64_t prod_seq;
	} src_retire[XB_SRC_RETIRE];
	//! The key the caller has been nominating since @ref src_pending_since_ns,
	//! and when it last CHANGED. The re-open waits for it to hold still — see
	//! XB_SRC_SETTLE_NS.
	uint64_t src_pending_key;
	uint64_t src_pending_since_ns;
	//! Window counters (drained by take_ingress_stats); `ing_rebind` is lifetime.
	uint64_t ing_direct, ing_staged, ing_rebind;
	//! Key changes that never became a re-open because the source kept moving —
	//! a drag, counted rather than acted on.
	uint64_t ing_churn;
	//! Superseded opens DROPPED without release because the retire ring was
	//! full. A tripwire: with the settle hysteresis in place this is
	//! structurally unreachable, and any non-zero value is a bug.
	uint64_t ing_leak;
	//! One WARN for a source whose NT handle the producer refused, ever.
	bool ing_open_warned;

	uint32_t max_w, max_h;

	/*!
	 * #1178 — the DXGI format of the WHOLE atlas chain: the ingress staging ring,
	 * the cross-adapter placed ring and the egress ring, which are three copies
	 * deep and so must every one of them share a typeless family with the source.
	 * Taken from @ref comp_xbridge_info::atlas_format at create and never changed
	 * afterwards — a format change would be a full chain rebuild, and no caller
	 * has one.
	 */
	DXGI_FORMAT fmt;
	//! Bytes per pixel of @ref fmt — the bandwidth gate's and the DIAG line's
	//! multiplier, which used to be a hardcoded 4.
	uint32_t bpp;
	//! #1178 — submits REFUSED because the source's format was not @ref fmt, and
	//! when that was last logged. Any non-zero refusal count is a bug in the
	//! CALLER, not in the bridge; it is counted rather than only logged so the
	//! quiesce summary carries it too.
	uint64_t fmt_refused;
	uint64_t fmt_refuse_log_ns;

	// --- #1178 content probe (DXR_SPLIT_CONTENT_PROBE=1) -------------------
	//! Off unless the env var is set; read once at create.
	bool probe_on;
	//! One WARN when the probe is asked for on a bridge that cannot serve it.
	bool probe_unavailable_warned;
	//! CPU-readable out-device staging square, allocated on first use.
	ID3D11Texture2D *probe_stage;
	//! A copy has been recorded into @ref probe_stage and is awaiting its Map.
	bool probe_pending;
	//! Slot and extent the pending copy was taken from, for the report.
	int32_t probe_slot;
	uint32_t probe_w, probe_h;
	//! When the probe last REPORTED — the once-a-second clock.
	uint64_t probe_last_ns;

	//! Published under the compositor lock: the slot the last app frame wove,
	//! which every repaint re-weaves with zero bridge traffic.
	int32_t weave_slot;

	// --- watchdog + stats -------------------------------------------------
	//! Last seq for which BOTH legs were submitted (the consumer link's target).
	std::atomic<uint64_t> last_submit_seq;
	//! Last seq the PRODUCER queue was told to signal — the drain target for any
	//! release of a producer-side resource, and the producer link's watchdog
	//! target. Diverges from last_submit_seq only on a leg-2 record failure.
	std::atomic<uint64_t> prod_submit_seq;
	//! Every frame the compositor ASKED to bridge, including the ones skipped
	//! because an allocator was still busy. A rising attempt count with a
	//! standing consumer fence is a stall even though nothing was submitted
	//! (#918 F3).
	std::atomic<uint64_t> attempted;
	std::atomic<bool> degraded;
	std::atomic<bool> quit;
	std::thread watchdog;
	//! Set when a teardown drain timed out: the D3D12 side is then LEAKED rather
	//! than released under work that may still be running (#918 F1/F2).
	bool drain_failed;

	int force_depth; //!< DXR_WEAVE_ON_SCANOUT_DEPTH=1 -> deterministic seq-1.
	uint64_t pick_now, pick_prev, pick_older, pick_none;
	//! #918 R1: picks that found landed content but ONLY under a superseded
	//! layout generation, and refused it. One held frame per mode switch.
	uint64_t pick_stale_gen;
	//! #918 R1: transition frames woven from a slot whose consumer copy was still
	//! in flight, ordered by the GPU-side wait. One per mode switch — the
	//! alternative is a presented frame woven for the PREVIOUS optical mode.
	uint64_t pick_inflight;
	uint64_t skip_alloc_busy;
	uint64_t frames;
};


/*
 *
 * Helpers.
 *
 */

/*!
 * The log prefix, which now has to name WHICH bridge is talking (#918 D12-3a).
 *
 * One unit, two flavours of ends, one set of field-log greps. A D3D12-split
 * session emitting `d3d11 xbridge:` would send a reader looking at the wrong
 * compositor. The D3D11 string is unchanged BYTE FOR BYTE — every line that used
 * to be a literal `"d3d11 xbridge: …"` is now `"%s: …"` with this as its first
 * argument, so a D3D11-ends session's log is identical to the one before this
 * change and every existing grep keeps working.
 */
#define XB_TAG(xb) ((xb)->d3d12_ends ? "d3d12 xbridge" : "d3d11 xbridge")

static const char *
xb_hr(HRESULT hr, char *buf, size_t n)
{
	snprintf(buf, n, "0x%08lx", (unsigned long)hr);
	return buf;
}

//! Defined with the rest of the ingress code; create() needs it for the #918 F6
//! fallback (no back-fence on the app device ⟹ Option I is unsafe). @p why names
//! the reason for the one WARN.
static bool
xb_latch_staged_ingress(struct comp_xbridge *xb, const char *why);

//! #918 PR 6 — defer the release of a superseded Option-I open behind the
//! producer fence. Declared here because the staged-ingress latch demotes an
//! adaptive bridge and must retire its source the same way.
static void
xb_retire_source(struct comp_xbridge *xb, ID3D12Resource *res);

//! Bounded CPU wait cap for the structural-transition drains below.
#define XB_DRAIN_TIMEOUT_MS 2000

/*!
 * Wait for @p fence to reach @p target, bounded at #XB_DRAIN_TIMEOUT_MS.
 *
 * **This is NOT the per-frame path.** Every caller is a structural transition —
 * an egress realloc on a mode switch or resize, an atlas re-bind on a generation
 * change, or teardown — where the alternative to a bounded CPU wait is releasing
 * a D3D12 resource an in-flight `CopyTextureRegion` is still reading or writing.
 * D3D12 has no deferred destruction: that is a use-after-free, and it surfaces
 * as `DXGI_ERROR_DEVICE_REMOVED` on whichever adapter lost the race (#918 F1/F2).
 *
 * @return true when the fence reached @p target; false on timeout, in which case
 *         the caller must LEAK the resources rather than free them.
 */
static bool
xb_drain_fence(struct comp_xbridge *xb, ID3D12Fence *fence, uint64_t target, const char *what)
{
	if (fence == nullptr || target == 0 || fence->GetCompletedValue() >= target) {
		return true;
	}

	HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
	if (ev != nullptr) {
		if (SUCCEEDED(fence->SetEventOnCompletion(target, ev))) {
			WaitForSingleObject(ev, XB_DRAIN_TIMEOUT_MS);
		}
		CloseHandle(ev);
	} else {
		// No event to wait on — poll to the same bound rather than skip the
		// drain entirely.
		const uint64_t deadline_ns = os_monotonic_get_ns() + (uint64_t)XB_DRAIN_TIMEOUT_MS * 1000ull * 1000ull;
		while (fence->GetCompletedValue() < target && os_monotonic_get_ns() < deadline_ns) {
			os_nanosleep(1000 * 1000);
		}
	}

	if (fence->GetCompletedValue() >= target) {
		return true;
	}
	U_LOG_W(
	    "%s: %s drain timed out at %llu/%llu after %d ms — LEAKING the resources "
	    "rather than freeing them under an in-flight copy (#918)",
	    XB_TAG(xb), what, (unsigned long long)fence->GetCompletedValue(), (unsigned long long)target,
	    XB_DRAIN_TIMEOUT_MS);
	return false;
}

/*!
 * Release the egress ring (both the D3D11 side and the consumer's opens).
 *
 * The consumer's `CopyTextureRegion` writes `eg_12[]` directly, so nothing here
 * may be freed while a consumer copy is still in flight — drain first. On a
 * drain timeout the pointers are dropped WITHOUT releasing: a leak is
 * recoverable (the session ends and the process reclaims it), a use-after-free
 * on a D3D12 copy queue is not (#918 F1).
 */
static void
xb_release_egress(struct comp_xbridge *xb)
{
	bool in_flight_reachable = false;
	for (int i = 0; i < XB_EGRESS_RING; i++) {
		if (xb->eg_12[i] != nullptr) {
			in_flight_reachable = true;
			break;
		}
	}
	const bool drained =
	    !in_flight_reachable ||
	    xb_drain_fence(xb, xb->f_out_cons, xb->last_submit_seq.load(std::memory_order_acquire), "egress release");
	if (!drained && !xb->degraded.load(std::memory_order_relaxed)) {
		xb->degraded.store(true, std::memory_order_release);
		U_LOG_W(
		    "%s: DEGRADED — the consumer queue would not drain for an egress "
		    "realloc; no further submissions (#918)",
		    XB_TAG(xb));
	}

	for (int i = 0; i < XB_EGRESS_RING; i++) {
		if (drained) {
			safe_release(xb->eg_12[i]);
			safe_close(xb->eg_share[i]);
			safe_release(xb->eg_srv[i]);
			safe_release(xb->eg_tex[i]);
		} else {
			// Deliberate leak — see the comment above.
			xb->eg_12[i] = nullptr;
			xb->eg_share[i] = nullptr;
			xb->eg_srv[i] = nullptr;
			xb->eg_tex[i] = nullptr;
		}
		xb->eg_seq[i] = 0;
		xb->eg_gen[i] = 0;
		xb->eg_cw[i] = 0;
		xb->eg_ch[i] = 0;
	}
	xb->eg_w = 0;
	xb->eg_h = 0;
	xb->eg_content_sized = false;
	xb->weave_slot = -1;
}

/*!
 * ONE egress texture: an output-device D3D11 texture (SRV for whoever samples
 * it) shared by NT handle and opened on the consumer D3D12 device as a copy
 * destination.
 *
 * The single recipe for BOTH rings — the atlas egress and every plane's (#918
 * review D9). They differed only in dims, format and whether the texture also
 * needs an RTV; two copies of a five-call sequence with a share handle in the
 * middle is exactly where the two rings would drift apart.
 *
 * #918 D12-3a: with D3D12 ends the whole share disappears. The consumer device
 * IS the output device, so the copy destination and the thing the display
 * processor samples are one committed resource, created here and handed back in
 * @p out_12 with the three D3D11 outputs left NULL. `out_tex`/`out_srv`/
 * `out_share` may be NULL in that mode.
 *
 * #918 D12-4: the ATLAS ring and every PLANE ring both reach this in that mode.
 * That is the whole reason the plane egress needed no new code — one recipe,
 * already device-flavoured, and a plane slot differs from an atlas slot only in
 * dims, format and the RTV bind. What D12-4 had to add was the plane's INGRESS
 * (bound by pointer rather than by NT handle) and a way to hand the egress back
 * as a resource (@ref comp_xbridge_get_plane_resource).
 *
 * @return the HRESULT of the first failing step (S_OK on success). The caller
 *         owns whatever was written, including on failure — release through its
 *         normal teardown so a partial chain is not leaked.
 */
static HRESULT
xb_make_egress_texture(struct comp_xbridge *xb,
                       uint32_t w,
                       uint32_t h,
                       DXGI_FORMAT fmt,
                       bool want_rtv,
                       ID3D11Texture2D **out_tex,
                       ID3D11ShaderResourceView **out_srv,
                       HANDLE *out_share,
                       ID3D12Resource **out_12)
{
	if (xb->d3d12_ends) {
		/*
		 * ALLOW_SIMULTANEOUS_ACCESS is load-bearing, not a habit. It reproduces
		 * exactly the state discipline the D3D11-ends egress texture gets for
		 * free: a D3D11-created shared texture opened in D3D12 must be left in
		 * COMMON, so the consumer's COPY-queue write and the display processor's
		 * DIRECT-queue read each promote implicitly and decay back, and nobody
		 * owes a barrier.
		 *
		 * A plain committed texture does NOT decay after a read on a direct
		 * queue — the display processor's sample would leave it in
		 * PIXEL_SHADER_RESOURCE, and the next consumer copy would then use a
		 * non-COMMON texture on a COPY queue, which is invalid. Fixing that with
		 * explicit barriers is not available to us: the display processor is a
		 * vendor plug-in (ADR-019) that is free to leave the resources it touches
		 * in any state and offers no way to query one back (the same unsoundness
		 * #747 tracks on the back buffer). Simultaneous access removes the
		 * question instead of guessing at the answer.
		 */
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = w;
		rd.Height = h;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = fmt;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		if (want_rtv) {
			rd.Flags = (D3D12_RESOURCE_FLAGS)(rd.Flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		}

		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;

		return xb->cons_dev->CreateCommittedResource(
		    &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(out_12));
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (want_rtv ? D3D11_BIND_RENDER_TARGET : 0);
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

	HRESULT hr = xb->out_dev->CreateTexture2D(&td, nullptr, out_tex);
	if (SUCCEEDED(hr)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = fmt;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		hr = xb->out_dev->CreateShaderResourceView(*out_tex, &sv, out_srv);
	}
	IDXGIResource1 *dr = nullptr;
	if (SUCCEEDED(hr)) {
		hr = (*out_tex)->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dr));
	}
	if (SUCCEEDED(hr) && dr != nullptr) {
		hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            out_share);
		dr->Release();
	}
	if (SUCCEEDED(hr) && *out_share == nullptr) {
		hr = E_FAIL;
	}
	if (SUCCEEDED(hr)) {
		hr = xb->cons_dev->OpenSharedHandle(*out_share, __uuidof(ID3D12Resource),
		                                    reinterpret_cast<void **>(out_12));
	}
	if (SUCCEEDED(hr) && *out_12 == nullptr) {
		hr = E_FAIL;
	}
	return hr;
}

/*!
 * ONE cross-adapter placed ring slot: a SHARED|SHARED_CROSS_ADAPTER heap created
 * on the producer, opened on the consumer, with a ROW_MAJOR ALLOW_CROSS_ADAPTER
 * placed resource at offset 0 on each side. The second half of #918 review D9's
 * duplication — the atlas ring and every plane ring build the identical chain.
 *
 * @param out_heap_bytes Receives the heap size, for the one-shot log line.
 */
static HRESULT
xb_make_xa_slot(struct comp_xbridge *xb,
                uint32_t w,
                uint32_t h,
                DXGI_FORMAT fmt,
                ID3D12Heap **out_heap_prod,
                ID3D12Heap **out_heap_cons,
                HANDLE *out_heap_h,
                ID3D12Resource **out_prod,
                ID3D12Resource **out_cons,
                uint64_t *out_heap_bytes)
{
	D3D12_RESOURCE_DESC td{};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
	UINT rows = 0;
	UINT64 row_bytes = 0, total = 0;
	xb->prod_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);

	const UINT64 align = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	D3D12_HEAP_DESC hd{};
	hd.SizeInBytes = (total + align - 1) & ~(align - 1);
	hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	hd.Alignment = align;
	hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
	                              D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES);
	if (out_heap_bytes != nullptr) {
		*out_heap_bytes = hd.SizeInBytes;
	}

	HRESULT hr = xb->prod_dev->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void **>(out_heap_prod));
	if (SUCCEEDED(hr)) {
		hr = xb->prod_dev->CreateSharedHandle(*out_heap_prod, nullptr, GENERIC_ALL, nullptr, out_heap_h);
	}
	if (SUCCEEDED(hr)) {
		hr = xb->cons_dev->OpenSharedHandle(*out_heap_h, __uuidof(ID3D12Heap),
		                                    reinterpret_cast<void **>(out_heap_cons));
	}
	if (SUCCEEDED(hr)) {
		hr = xb->prod_dev->CreatePlacedResource(*out_heap_prod, 0, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                        __uuidof(ID3D12Resource), reinterpret_cast<void **>(out_prod));
	}
	if (SUCCEEDED(hr)) {
		hr = xb->cons_dev->CreatePlacedResource(*out_heap_cons, 0, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                        __uuidof(ID3D12Resource), reinterpret_cast<void **>(out_cons));
	}
	return hr;
}

/*!
 * One egress slot of the ATLAS ring. Same texture recipe as a plane's, plus the
 * RTV bind the atlas egress has always carried.
 */
static bool
xb_make_egress_slot(struct comp_xbridge *xb, int i, uint32_t w, uint32_t h, const char **out_reason)
{
	char b[32];
	HRESULT hr = xb_make_egress_texture(xb, w, h, xb->fmt, /*want_rtv=*/true, &xb->eg_tex[i], &xb->eg_srv[i],
	                                    &xb->eg_share[i], &xb->eg_12[i]);
	if (FAILED(hr)) {
		U_LOG_W("%s: egress slot %d (%ux%u) failed %s", XB_TAG(xb), i, w, h, xb_hr(hr, b, sizeof(b)));
		*out_reason = "egress share failed";
		return false;
	}
	return true;
}

/*
 *
 * Planes (#918 Phase 2a).
 *
 */

static const char *
xb_plane_name(uint32_t p)
{
	switch (p) {
	case COMP_XBRIDGE_PLANE_LOCAL2D: return "Local2D";
	case COMP_XBRIDGE_PLANE_BACKDROP: return "backdrop";
	case COMP_XBRIDGE_PLANE_MASK: return "authored mask";
	default: return "?";
	}
}

extern "C" const char *
comp_xbridge_plane_label(uint32_t plane)
{
	// #918 review D8: the DIAG line's names come from HERE, so adding a plane
	// cannot leave the log naming it by an index that no longer means anything.
	switch (plane) {
	case COMP_XBRIDGE_PLANE_LOCAL2D: return "local2d";
	case COMP_XBRIDGE_PLANE_BACKDROP: return "backdrop";
	case COMP_XBRIDGE_PLANE_MASK: return "mask";
	default: return "plane?";
	}
}

static uint32_t
xb_format_bpp(uint32_t fmt)
{
	switch ((DXGI_FORMAT)fmt) {
	case DXGI_FORMAT_R8_UNORM: return 1;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM: return 4;
	default: return 4;
	}
}

/*!
 * #1178 — a DXGI format's name for the log.
 *
 * A format mismatch is reported by NAME and not only by number, because the
 * whole failure mode is that the two look interchangeable: `0x1c` and `0x57`
 * say nothing, `R8G8B8A8_UNORM` beside `B8G8R8A8_UNORM` says everything. The
 * numeric value is printed alongside for a format this list does not know.
 */
static const char *
xb_fmt_name(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
	case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	default: return "?";
	}
}

/*!
 * #1178 — THE SUBMIT GUARD. True when @p src_fmt may legally be copied into the
 * bridge's chain; false, loudly, when it may not.
 *
 * This is the part that matters more than the fix. `CopySubresourceRegion` and
 * `CopyTextureRegion` across two DXGI typeless families do not fail, do not warn
 * and do not set an HRESULT — Windows drops the copy and leaves the destination
 * holding whatever it held. Every counter this unit keeps is incremented by the
 * ACT of issuing the copy, so `xb_kb`, `ing_staged`, `no_slot=0` and
 * `ing_leak=0` all read healthy while the panel is black. That is precisely how
 * #1178 got as far as a maintainer's eyeball.
 *
 * So a mismatch REFUSES the frame instead of transporting nothing: a refused
 * frame leaves the weave holding the last good slot (`no_slot` rises, which is a
 * visible symptom) and prints a line naming both formats, rather than issuing a
 * copy that will succeed at doing nothing.
 *
 * Logged on the first refusal and at most every @ref XB_FMT_REFUSE_LOG_NS
 * afterwards — loud, but never the per-frame U_LOG_E the repo forbids.
 */
static bool
xb_check_source_format(struct comp_xbridge *xb, DXGI_FORMAT chain_fmt, DXGI_FORMAT src_fmt, const char *where)
{
	if (src_fmt == chain_fmt) {
		return true;
	}
	xb->fmt_refused++;
	const uint64_t now = os_monotonic_get_ns();
	if (xb->fmt_refuse_log_ns == 0 || now - xb->fmt_refuse_log_ns >= XB_FMT_REFUSE_LOG_NS) {
		xb->fmt_refuse_log_ns = now;
		U_LOG_E(
		    "%s: ATLAS FORMAT MISMATCH at %s — the source is %s (0x%x) but that chain is %s "
		    "(0x%x). A D3D copy across DXGI typeless families is SILENTLY DROPPED, not failed, so the "
		    "frame is REFUSED rather than transported as nothing. Fix the caller's "
		    "comp_xbridge_info::atlas_format (#1178). %llu refusals so far.",
		    XB_TAG(xb), where, xb_fmt_name(src_fmt), (unsigned)src_fmt, xb_fmt_name(chain_fmt),
		    (unsigned)chain_fmt, (unsigned long long)xb->fmt_refused);
	}
	return false;
}

/*!
 * Invalidate every slot's pixels for @p pl, and make each slot OWE A FULL
 * REFRESH.
 *
 * Seeding the pending box to the whole plane rather than clearing it is the
 * point: a slot marked "no pixels, owes nothing" would take the next frame's
 * DIRTY BOX as its first and only copy — a small sub-rect — and then be stamped
 * valid, leaving everything outside that box at whatever the texture held
 * before. For the RGBA planes that reads as transparent by luck (fresh
 * allocations are zero-filled); for the R8 mask, zero means "2D everywhere",
 * which is the opposite of the all-3D default an unauthored mask is supposed to
 * have.
 */
static void
xb_plane_invalidate_slots(struct comp_xbridge *xb, struct comp_xbridge::xb_plane &pl)
{
	(void)xb;
	uint32_t w = pl.alloc_w;
	uint32_t h = pl.alloc_h;
	if (pl.src_w != 0 && pl.src_w < w) {
		w = pl.src_w;
	}
	if (pl.src_h != 0 && pl.src_h < h) {
		h = pl.src_h;
	}
	for (int i = 0; i < XB_EGRESS_RING; i++) {
		pl.slot_seq[i] = 0;
		pl.pend_x[i] = 0;
		pl.pend_y[i] = 0;
		pl.pend_w[i] = w;
		pl.pend_h[i] = h;
	}
	pl.last_w = 0;
	pl.last_h = 0;
}

/*!
 * Release one plane's GPU side. @p drained says whether the consumer fence has
 * reached the last submitted seq; when it has NOT, the D3D12 objects are dropped
 * WITHOUT releasing, exactly as @ref xb_release_egress does — a leak is
 * recoverable, a use-after-free on a copy queue is not (#918 F1).
 */
static void
xb_release_plane(struct comp_xbridge *xb, uint32_t p, bool drained)
{
	auto &pl = xb->plane[p];
	for (int i = 0; i < XB_EGRESS_RING; i++) {
		if (drained) {
			safe_release(pl.eg_12[i]);
			safe_close(pl.eg_share[i]);
			safe_release(pl.eg_srv[i]);
			safe_release(pl.eg_tex[i]);
		} else {
			pl.eg_12[i] = nullptr;
			pl.eg_share[i] = nullptr;
			pl.eg_srv[i] = nullptr;
			pl.eg_tex[i] = nullptr;
		}
		pl.slot_seq[i] = 0;
		pl.pend_w[i] = 0;
		pl.pend_h[i] = 0;
	}
	for (int i = 0; i < XB_XA_RING; i++) {
		if (drained) {
			safe_release(pl.xa_cons[i]);
			safe_release(pl.xa_prod[i]);
			safe_release(pl.heap_cons[i]);
			safe_release(pl.heap_prod[i]);
			safe_close(pl.heap_h[i]);
		} else {
			pl.xa_cons[i] = nullptr;
			pl.xa_prod[i] = nullptr;
			pl.heap_cons[i] = nullptr;
			pl.heap_prod[i] = nullptr;
			pl.heap_h[i] = nullptr;
		}
	}
	if (drained) {
		safe_release(pl.src12);
	} else {
		pl.src12 = nullptr;
	}
	pl.live = false;
	pl.src_bound = false;
	pl.src_w = 0;
	pl.src_h = 0;
	pl.alloc_w = 0;
	pl.alloc_h = 0;
}

/*!
 * Allocate one plane's whole chain at @p w x @p h: its own cross-adapter heap +
 * placed ring and its own egress textures (D3D11 on the output device, NT-shared
 * and opened by the consumer).
 *
 * **The extent is the CALLER's, not the panel's (#918 review F5).** The two 2D
 * planes ask for the panel deliberately — allocated once, never resized,
 * structurally outside the R2 churn path. The authored MASK does not: an app
 * creates a mask at dims of its own choosing (the docs even recommend a
 * downsampled one) and the mask maps stretch-to-region, so a panel-sized mask
 * plane would transport the mask into a corner of a texture the composite then
 * stretches whole — sampling a band no copy ever wrote, which for R8 reads as
 * "2D everywhere", the exact inverse of an unauthored mask's all-3D default.
 * Sizing the plane at the mask makes the whole mask the thing that crosses. It
 * costs nothing on the churn argument either: the mask plane transports on
 * `author_seq` change only, so its allocation is on-change too.
 *
 * This is also where the R8 cross-adapter question is ANSWERED rather than
 * predicted. `CrossAdapterRowMajorTextureSupported` was measured NOT to predict
 * behaviour on this stack (NVIDIA reports FALSE and samples cleanly), so the
 * only honest probe is the allocation itself — if the driver refuses an R8
 * ROW_MAJOR cross-adapter placed resource, the plane is disabled with one WARN
 * and the feature that needs it degrades. The split is never affected.
 */
static bool
xb_plane_alloc(struct comp_xbridge *xb, uint32_t p, uint32_t fmt, uint32_t w, uint32_t h)
{
	char b[32];
	auto &pl = xb->plane[p];
	if (pl.failed) {
		return false;
	}
	if (pl.live) {
		// The format is a property of the plane, fixed for the session.
		if (pl.fmt != fmt) {
			return false;
		}
		if (pl.alloc_w == w && pl.alloc_h == h) {
			return true;
		}
		/*
		 * A dims change means the app created a differently-sized mask — an
		 * on-change event, never a per-frame one, so a rebuild here cannot
		 * become the R2 churn the 2D planes are protected from (they always
		 * ask for the panel and so never reach this). The consumer may still
		 * be copying into the old egress; drain before releasing, and leak on
		 * timeout exactly as every other structural transition does.
		 */
		const bool drained = xb_drain_fence(
		    xb, xb->f_out_cons, xb->last_submit_seq.load(std::memory_order_acquire), "plane re-size");
		xb_release_plane(xb, p, drained);
	}
	if (w == 0 || h == 0) {
		// No usable extent yet — NOT a permanent failure (a mask can be bound
		// before its dims are known), so do not latch `failed`.
		return false;
	}
	pl.fmt = fmt;
	pl.bpp = xb_format_bpp(fmt);
	pl.alloc_w = w;
	pl.alloc_h = h;

	HRESULT hr = S_OK;
	const char *what = "cross-adapter heap";
	uint64_t heap_bytes = 0;

	for (int i = 0; i < XB_XA_RING && SUCCEEDED(hr); i++) {
		hr = xb_make_xa_slot(xb, w, h, (DXGI_FORMAT)fmt, &pl.heap_prod[i], &pl.heap_cons[i], &pl.heap_h[i],
		                     &pl.xa_prod[i], &pl.xa_cons[i], &heap_bytes);
	}

	// Egress: an output-device D3D11 texture per slot, NT-shared to the consumer.
	for (int i = 0; i < XB_EGRESS_RING && SUCCEEDED(hr); i++) {
		what = "egress";
		hr = xb_make_egress_texture(xb, w, h, (DXGI_FORMAT)fmt, /*want_rtv=*/false, &pl.eg_tex[i],
		                            &pl.eg_srv[i], &pl.eg_share[i], &pl.eg_12[i]);
	}

	if (FAILED(hr)) {
		U_LOG_W(
		    "%s: the %s plane could not allocate its %s at %ux%u fmt=%u %s — THAT FEATURE "
		    "is unavailable under the output-device split for this session; the split itself is "
		    "unaffected (#918 Phase 2a)",
		    XB_TAG(xb), xb_plane_name(p), what, w, h, fmt, xb_hr(hr, b, sizeof(b)));
		xb_release_plane(xb, p, /*drained=*/true);
		pl.failed = true;
		return false;
	}

	pl.live = true;
	// Fresh egress textures hold nothing: every slot owes a full refresh.
	xb_plane_invalidate_slots(xb, pl);
	U_LOG_W("%s: %s plane up — %ux%u fmt=%u, heap %llu bytes x%d + egress x%d (#918 Phase 2a)", XB_TAG(xb),
	        xb_plane_name(p), w, h, fmt, (unsigned long long)heap_bytes, XB_XA_RING, XB_EGRESS_RING);
	return true;
}

extern "C" bool
comp_xbridge_bind_plane(struct comp_xbridge *xb,
                        uint32_t plane,
                        void *nt_handle,
                        uint64_t generation,
                        uint32_t dxgi_format,
                        uint32_t w,
                        uint32_t h)
{
	char b[32];
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT) {
		return false;
	}
	auto &pl = xb->plane[plane];
	/*
	 * A plane that has failed once is never retried. Without this the callers —
	 * which bind unconditionally EVERY FRAME — would re-attempt a failed open and
	 * re-emit its WARN per frame, which the repo's logging law forbids outright.
	 */
	if (pl.failed) {
		return false;
	}

	/*
	 * #918 D12-4 — WRONG BIND FLAVOUR, not an unsupported plane.
	 *
	 * D12-3a refused planes outright here and LATCHED `failed`, because neither
	 * half of a plane's chain existed on a D3D12-ends bridge. Both halves exist
	 * now: the egress is the same device-flavoured recipe the atlas ring already
	 * used (@ref xb_make_egress_texture), and the source is bound BY POINTER
	 * through @ref comp_xbridge_bind_plane_resource. What is still impossible on
	 * this flavour is the NT handle THIS entry point takes — the producer is the
	 * app device, so there is nothing to open and no handle to open it from.
	 *
	 * Refuse the CALL without latching `failed`: latching it would make a caller
	 * that reached the wrong entry point once permanently unable to use the right
	 * one, which is the difference between "you called the wrong function" and
	 * "this plane is dead". One WARN per plane, because the callers bind every
	 * frame.
	 */
	if (xb->d3d12_ends) {
		if (!pl.bind_flavour_warned) {
			pl.bind_flavour_warned = true;
			U_LOG_W(
			    "%s: the %s plane was bound by NT HANDLE on a D3D12-ends bridge — the producer is "
			    "the app device and has nothing to open; use comp_xbridge_bind_plane_resource "
			    "(#918 D12-4)",
			    XB_TAG(xb), xb_plane_name(plane));
		}
		return false;
	}

	/*
	 * Same #918 F2 hazard as the atlas re-bind, and it applies to DROPPING the
	 * binding as much as to replacing it: the producer's copy of the last
	 * submitted seq reads exactly this resource, and the caller is about to
	 * release the texture underneath it. Drain the producer link first; on
	 * timeout LEAK rather than hand the copy queue a freed resource.
	 */
	auto drop_source = [&]() {
		if (pl.src12 == nullptr) {
			return;
		}
		if (!xb_drain_fence(xb, xb->f_xa_prod, xb->prod_submit_seq.load(std::memory_order_acquire),
		                    "plane re-bind")) {
			pl.src12 = nullptr; // deliberate leak — a UAF is not recoverable
		}
		safe_release(pl.src12);
		pl.src_w = 0;
		pl.src_h = 0;
		pl.src_bound = false;
		xb_plane_invalidate_slots(xb, pl);
	};

	if (nt_handle == nullptr) {
		drop_source();
		return false;
	}
	if (!xb_plane_alloc(xb, plane, dxgi_format, w, h)) {
		return false;
	}
	if (pl.src_bound && pl.src_gen == generation) {
		return true;
	}
	drop_source();
	HRESULT hr = xb->prod_dev->OpenSharedHandle(static_cast<HANDLE>(nt_handle), __uuidof(ID3D12Resource),
	                                            reinterpret_cast<void **>(&pl.src12));
	if (FAILED(hr) || pl.src12 == nullptr) {
		U_LOG_W(
		    "%s: the producer could not open the %s plane's source %s — that feature "
		    "degrades for this session (#918 Phase 2a)",
		    XB_TAG(xb), xb_plane_name(plane), xb_hr(hr, b, sizeof(b)));
		pl.failed = true;
		pl.src_bound = false;
		return false;
	}
	{
		// The source's real extent — the copy box is clamped to it, not just to
		// the panel, because an authored mask is created at the app's zone dims.
		D3D12_RESOURCE_DESC sd = pl.src12->GetDesc();
		/*
		 * #1178 — the same guard the atlas gets, for the same reason. A plane's
		 * chain is allocated in the format the CALLER declared, and its producer
		 * copy is a CopyTextureRegion out of the source the caller just opened;
		 * if the two are different typeless families that copy is dropped in
		 * silence and the plane transports nothing, forever, with `copies` rising.
		 * Degrade the plane loudly instead.
		 */
		if (!xb_check_source_format(xb, (DXGI_FORMAT)pl.fmt, sd.Format, xb_plane_name(plane))) {
			safe_release(pl.src12);
			pl.failed = true;
			pl.src_bound = false;
			return false;
		}
		pl.src_w = (uint32_t)sd.Width;
		pl.src_h = sd.Height;
	}
	pl.src_gen = generation;
	pl.src_bound = true;
	// #918 review F7: a re-bind is a new source, so whatever rate history earned
	// the half-rate latch no longer describes this plane. Release it here as well
	// as on the gate's own un-latch clock.
	pl.half_rate = false;
	// A new source texture invalidates every slot's pixels, and each slot now
	// owes a FULL refresh rather than just the next frame's dirty box.
	xb_plane_invalidate_slots(xb, pl);
	return true;
}

extern "C" bool
comp_xbridge_bind_plane_resource(struct comp_xbridge *xb,
                                 uint32_t plane,
                                 void *resource,
                                 uint64_t generation,
                                 uint32_t dxgi_format,
                                 uint32_t w,
                                 uint32_t h)
{
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT || !xb->d3d12_ends) {
		return false;
	}
	auto &pl = xb->plane[plane];
	// Same never-retry rule as the NT-handle bind: the callers bind every frame.
	if (pl.failed) {
		return false;
	}
	auto *res = static_cast<ID3D12Resource *>(resource);

	/*
	 * Dropping a plane's source has the SAME #918 F2 hazard as replacing it — the
	 * producer's copy of the last submitted seq reads exactly this resource — and
	 * it is closed with the STRONGER of the two tools, exactly as
	 * @ref comp_xbridge_bind_atlas_resource does. The NT-handle twin above takes a
	 * bounded CPU drain because it must release its open synchronously or lose
	 * track of it; here the fence-deferred retire ring (#918 PR 6) defers the
	 * release behind the producer fence and costs no wait at all — which matters
	 * more here than it does for the atlas, because a plane re-binds on a scratch
	 * REALLOC and the D3D12 leg reaches this from the app's own render thread
	 * inside xrEndFrame.
	 */
	auto drop_source = [&]() {
		if (pl.src12 == nullptr) {
			return;
		}
		xb_retire_source(xb, pl.src12);
		pl.src12 = nullptr;
		pl.src_w = 0;
		pl.src_h = 0;
		pl.src_bound = false;
		xb_plane_invalidate_slots(xb, pl);
	};

	if (res == nullptr) {
		drop_source();
		return false;
	}
	if (!xb_plane_alloc(xb, plane, dxgi_format, w, h)) {
		return false;
	}
	// Identity AND generation: a caller whose allocator handed back the same
	// address for a new allocation still bumps its generation, and that is what
	// the test is for (the same trap `src_key` documents for adaptive ingress).
	if (pl.src_bound && pl.src12 == res && pl.src_gen == generation) {
		return true;
	}
	drop_source();

	/*
	 * Our reference is not what keeps this alive — the caller owns the scratch and
	 * holds its own. The AddRef is about not RELEASING under a live producer copy,
	 * which is the same invariant the NT-handle path reaches through its open.
	 */
	{
		// #1178 — the guard, before the AddRef: a cross-family producer copy is
		// dropped in silence, so this plane must degrade loudly rather than
		// transport nothing at a perfectly healthy `copies` count.
		const D3D12_RESOURCE_DESC sd = res->GetDesc();
		if (!xb_check_source_format(xb, (DXGI_FORMAT)pl.fmt, sd.Format, xb_plane_name(plane))) {
			pl.failed = true;
			pl.src_bound = false;
			return false;
		}
	}
	res->AddRef();
	pl.src12 = res;
	{
		// The source's real extent — the copy box is clamped to it, not just to
		// the plane's allocation, because an authored mask is created at the app's
		// own zone dims.
		D3D12_RESOURCE_DESC sd = res->GetDesc();
		pl.src_w = (uint32_t)sd.Width;
		pl.src_h = sd.Height;
	}
	pl.src_gen = generation;
	pl.src_bound = true;
	// #918 review F7: a re-bind is a new source, so the rate history that earned
	// the half-rate latch no longer describes this plane.
	pl.half_rate = false;
	xb_plane_invalidate_slots(xb, pl);
	U_LOG_W(
	    "%s: producer bound the %s plane's source resource %p directly (generation %llu) — no share, no open, no "
	    "drain (#918 D12-4)",
	    XB_TAG(xb), xb_plane_name(plane), (void *)res, (unsigned long long)generation);
	return true;
}

extern "C" void
comp_xbridge_invalidate_plane(struct comp_xbridge *xb, uint32_t plane)
{
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT || !xb->plane[plane].live) {
		return;
	}
	xb_plane_invalidate_slots(xb, xb->plane[plane]);
}

//! Slot @p i's pending dirty box, as the policy's box type.
static inline struct xb_plane_box
xb_plane_pend_box(const struct comp_xbridge::xb_plane &pl, int i)
{
	struct xb_plane_box b = {pl.pend_x[i], pl.pend_y[i], pl.pend_w[i], pl.pend_h[i]};
	return b;
}

//! Fold @p (x,y,w,h) into slot @p i's pending dirty box.
static void
xb_plane_pend(struct comp_xbridge::xb_plane &pl, int i, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	struct xb_plane_box acc = xb_plane_pend_box(pl, i);
	const struct xb_plane_box add = {x, y, w, h};
	xb_plane_box_union(&acc, &add);
	pl.pend_x[i] = acc.x;
	pl.pend_y[i] = acc.y;
	pl.pend_w[i] = acc.w;
	pl.pend_h[i] = acc.h;
}

extern "C" void
comp_xbridge_stage_plane(
    struct comp_xbridge *xb, uint32_t plane, uint64_t content_seq, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT) {
		return;
	}
	auto &pl = xb->plane[plane];
	if (!pl.live || !pl.src_bound) {
		return;
	}
	// seq 0 = "this frame does not use the plane". Un-stage rather than carry
	// the previous frame's box forward, so the recipe stamps it invalid and the
	// consume half cannot sample content the frame did not produce.
	if (content_seq == 0) {
		pl.staged = false;
		return;
	}
	// Clamp into the PLANE's own extent (not the panel's — an authored mask
	// plane is mask-sized, #918 review F5). Nothing beyond it can be sampled.
	{
		uint32_t lim_w = 0, lim_h = 0;
		xb_plane_limits(pl.src_w, pl.src_h, pl.alloc_w, pl.alloc_h, &lim_w, &lim_h);
		const struct xb_plane_box in = {x, y, w, h};
		const struct xb_plane_box clipped = xb_plane_box_clamp(&in, lim_w, lim_h);
		x = clipped.x;
		y = clipped.y;
		w = clipped.w;
		h = clipped.h;
	}

	/*
	 * Content changed: every slot now owes the union of what the plane used to
	 * cover and what it covers now — the first refreshes pixels the content has
	 * VACATED, the second the pixels it moved onto. Accumulating per slot is what
	 * lets a slot that sat out several changes catch up in one copy instead of
	 * being left partially updated.
	 */
	if (content_seq != pl.stage_seq || !pl.staged) {
		for (int i = 0; i < XB_EGRESS_RING; i++) {
			xb_plane_pend(pl, i, pl.last_x, pl.last_y, pl.last_w, pl.last_h);
			xb_plane_pend(pl, i, x, y, w, h);
		}
		pl.last_x = x;
		pl.last_y = y;
		pl.last_w = w;
		pl.last_h = h;
	}
	pl.stage_seq = content_seq;
	pl.stage_x = x;
	pl.stage_y = y;
	pl.stage_w = w;
	pl.stage_h = h;
	pl.staged = true;
}

extern "C" void
comp_xbridge_stage_recipe(struct comp_xbridge *xb, const struct comp_xbridge_recipe *recipe)
{
	if (xb == nullptr || recipe == nullptr) {
		return;
	}
	xb->staged_recipe = *recipe;
}

extern "C" bool
comp_xbridge_slot_recipe(struct comp_xbridge *xb, int32_t slot, struct comp_xbridge_recipe *out)
{
	if (xb == nullptr || out == nullptr || slot < 0 || slot >= XB_EGRESS_RING || xb->eg_seq[slot] == 0) {
		return false;
	}
	*out = xb->eg_recipe[slot];
	return true;
}

extern "C" void *
comp_xbridge_get_plane_srv(struct comp_xbridge *xb, int32_t slot, uint32_t plane, uint64_t want_seq)
{
	if (xb == nullptr || slot < 0 || slot >= XB_EGRESS_RING || plane >= COMP_XBRIDGE_PLANE_COUNT) {
		return nullptr;
	}
	auto &pl = xb->plane[plane];
	if (!pl.live || pl.slot_seq[slot] == 0) {
		return nullptr;
	}
	/*
	 * #918 review F3 — the STALE-PLANE test, and the reader `plane_seq` was
	 * documented to have and did not. The caller passes the generation the
	 * slot's recipe advertises; if the slot no longer holds it, a later submit
	 * has rewritten the plane under this weave and these are not the pixels the
	 * recipe describes. Refusing costs the 2D band one frame (the caller falls
	 * back to the previous publish, or skips); compositing anyway puts one
	 * frame's 2D over another frame's 3D.
	 */
	if (want_seq != 0 && pl.slot_seq[slot] != want_seq) {
		return nullptr;
	}
	return pl.eg_srv[slot];
}

extern "C" void *
comp_xbridge_get_plane_resource(struct comp_xbridge *xb, int32_t slot, uint32_t plane, uint64_t want_seq)
{
	if (xb == nullptr || !xb->d3d12_ends || slot < 0 || slot >= XB_EGRESS_RING ||
	    plane >= COMP_XBRIDGE_PLANE_COUNT) {
		return nullptr;
	}
	auto &pl = xb->plane[plane];
	if (!pl.live || pl.slot_seq[slot] == 0) {
		return nullptr;
	}
	// The same #918 review F3 stale-plane test the SRV twin runs, and for the
	// same reason — a later submit rewriting the plane under this weave would
	// otherwise put one frame's 2D over another frame's 3D.
	if (want_seq != 0 && pl.slot_seq[slot] != want_seq) {
		return nullptr;
	}
	return pl.eg_12[slot];
}

extern "C" bool
comp_xbridge_plane_extent(struct comp_xbridge *xb, uint32_t plane, uint32_t *out_w, uint32_t *out_h)
{
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT || !xb->plane[plane].live) {
		return false;
	}
	if (out_w != nullptr) {
		*out_w = xb->plane[plane].alloc_w;
	}
	if (out_h != nullptr) {
		*out_h = xb->plane[plane].alloc_h;
	}
	return true;
}

extern "C" void
comp_xbridge_take_plane_stats(struct comp_xbridge *xb,
                              uint32_t plane,
                              uint64_t *out_bytes,
                              uint64_t *out_copies,
                              uint64_t *out_skips,
                              bool *out_half_rate)
{
	uint64_t bytes = 0, copies = 0, skips = 0;
	bool half = false;
	if (xb != nullptr && plane < COMP_XBRIDGE_PLANE_COUNT) {
		auto &pl = xb->plane[plane];
		bytes = pl.bytes;
		copies = pl.copies;
		skips = pl.skips;
		half = pl.half_rate;
		pl.bytes = 0; // window; copies/skips stay lifetime totals
	}
	if (out_bytes != nullptr) {
		*out_bytes = bytes;
	}
	if (out_copies != nullptr) {
		*out_copies = copies;
	}
	if (out_skips != nullptr) {
		*out_skips = skips;
	}
	if (out_half_rate != nullptr) {
		*out_half_rate = half;
	}
}

extern "C" uint64_t
comp_xbridge_take_atlas_bytes(struct comp_xbridge *xb)
{
	if (xb == nullptr) {
		return 0;
	}
	const uint64_t v = xb->atlas_bytes;
	xb->atlas_bytes = 0;
	return v;
}

/*!
 * Record both legs of one plane into the command lists the atlas is already
 * using, so the plane lands on the SAME seq and the SAME fence pair — the
 * atomicity the whole per-slot design rests on.
 *
 * @return the bytes copied (0 when skipped).
 */
static uint64_t
xb_record_plane(struct comp_xbridge *xb, uint32_t p, uint64_t seq, int xa, int eg)
{
	auto &pl = xb->plane[p];
	if (!pl.live || !pl.src_bound || !pl.staged || pl.src12 == nullptr) {
		return 0;
	}
	/*
	 * Decide on what the slot OWES, before folding this frame's staged box in.
	 * The order is load-bearing: a slot that already holds this generation and
	 * owes nothing is a change-skip, and folding the staged box first would make
	 * every such slot look dirty — turning the skip into a copy on every single
	 * frame, which is the entire bandwidth argument inverted.
	 */
	const struct xb_plane_box owed = xb_plane_pend_box(pl, eg);
	const struct xb_plane_box staged = {pl.stage_x, pl.stage_y, pl.stage_w, pl.stage_h};
	if (xb_plane_decide(pl.slot_seq[eg], pl.stage_seq, &owed, &staged, pl.half_rate, seq, pl.half_rate_parity) !=
	    XB_PLANE_COPY) {
		/*
		 * Skipped. Leave `slot_seq` ALONE in every skip case: advancing it would
		 * mark the slot as carrying this generation's pixels when no copy ever
		 * landed, which is exactly the "old pixels, silently" failure the
		 * change-skip design exists to make impossible — and the recipe stamp
		 * reads slot_seq to decide validity (#918 review F3).
		 */
		pl.skips++;
		return 0;
	}

	// Now fold this frame's box in, and transport the union.
	xb_plane_pend(pl, eg, pl.stage_x, pl.stage_y, pl.stage_w, pl.stage_h);
	const struct xb_plane_box pend = xb_plane_pend_box(pl, eg);
	uint32_t lim_w = 0, lim_h = 0;
	xb_plane_limits(pl.src_w, pl.src_h, pl.alloc_w, pl.alloc_h, &lim_w, &lim_h);
	const struct xb_plane_box b = xb_plane_box_snap_clamp(&pend, lim_w, lim_h);
	if (xb_plane_box_empty(&b)) {
		pl.skips++;
		return 0;
	}

	D3D12_BOX box{};
	box.left = (UINT)b.x;
	box.top = (UINT)b.y;
	box.front = 0;
	box.right = (UINT)b.x + b.w;
	box.bottom = (UINT)b.y + b.h;
	box.back = 1;

	{
		D3D12_TEXTURE_COPY_LOCATION dst = sub_loc(pl.xa_prod[xa]);
		D3D12_TEXTURE_COPY_LOCATION src = sub_loc(pl.src12);
		xb->prod_list->CopyTextureRegion(&dst, (UINT)b.x, (UINT)b.y, 0, &src, &box);
	}
	{
		D3D12_TEXTURE_COPY_LOCATION dst = sub_loc(pl.eg_12[eg]);
		D3D12_TEXTURE_COPY_LOCATION src = sub_loc(pl.xa_cons[xa]);
		xb->cons_list->CopyTextureRegion(&dst, (UINT)b.x, (UINT)b.y, 0, &src, &box);
	}

	pl.slot_seq[eg] = pl.stage_seq;
	pl.pend_w[eg] = 0;
	pl.pend_h[eg] = 0;
	pl.copies++;
	const uint64_t bytes = (uint64_t)b.w * b.h * pl.bpp;
	pl.bytes += bytes;
	pl.gate_bytes += bytes;
	return bytes;
}

/*!
 * #918 Phase 2a bandwidth gate, rewritten for #918 review F7.
 *
 * The gate's only lever is a PLANE's rate, so it must aim at plane-attributable
 * bytes. It used to meter the whole transport — atlas included — and then
 * throttle the largest plane, which meant a 4-view 4K atlas (3.98 GB/s at 120 Hz
 * on its own, before any 2D content exists) latched every plane in turn, one a
 * second, for a total the planes did not cause and halving them cannot fix. The
 * standing comment "the atlas alone is over, which it never is" was the
 * assumption that made that look safe.
 *
 * Three rules now:
 *  - the OVER-BUDGET test is on the transport TOTAL, which is what the
 *    acceptance number is about;
 *  - throttling only happens when the PLANES contribute meaningfully to it
 *    (@ref XB_GATE_PLANE_FLOOR_BYTES_PER_S), so an over-budget atlas alone never
 *    throttles a plane;
 *  - the latch is NOT permanent — it clears when the transport has stayed inside
 *    budget for @ref XB_GATE_UNLATCH_NS, and a plane re-bind clears its own.
 */
static void
xb_bandwidth_gate(struct comp_xbridge *xb, uint64_t atlas_bytes, uint64_t plane_bytes, uint64_t seq)
{
	const uint64_t now = os_monotonic_get_ns();
	xb->gate_atlas_bytes += atlas_bytes;
	xb->gate_plane_bytes += plane_bytes;
	if (xb->gate_window_ns == 0) {
		xb->gate_window_ns = now;
		return;
	}
	const uint64_t elapsed = now - xb->gate_window_ns;
	if (elapsed < 1000ull * 1000ull * 1000ull) {
		return;
	}
	const double inv = 1.0e9 / (double)elapsed;
	const uint64_t plane_rate = (uint64_t)((double)xb->gate_plane_bytes * inv);
	const uint64_t total_rate = (uint64_t)((double)(xb->gate_atlas_bytes + xb->gate_plane_bytes) * inv);
	xb->gate_atlas_bytes = 0;
	xb->gate_plane_bytes = 0;
	xb->gate_window_ns = now;

	uint32_t worst = COMP_XBRIDGE_PLANE_COUNT;
	uint64_t worst_bytes = 0;
	bool any_latched = false;
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		if (xb->plane[p].half_rate) {
			any_latched = true;
		} else if (xb->plane[p].live && xb->plane[p].gate_bytes > worst_bytes) {
			worst_bytes = xb->plane[p].gate_bytes;
			worst = p;
		}
		xb->plane[p].gate_bytes = 0;
	}

	if (total_rate <= XB_BANDWIDTH_GATE_BYTES_PER_S) {
		// UN-LATCH. A latched plane runs at half rate for as long as the
		// pressure lasts, not for the rest of the session — a resize or a
		// fullscreen burst must not permanently halve the 2D update rate of a
		// session that spends the next hour idle.
		if (xb->gate_under_since_ns == 0) {
			xb->gate_under_since_ns = now;
		} else if (any_latched && now - xb->gate_under_since_ns >= XB_GATE_UNLATCH_NS) {
			for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
				xb->plane[p].half_rate = false;
			}
			xb->gate_under_since_ns = 0;
			U_LOG_W(
			    "%s: transport has been inside the %.1f GB/s gate for %llu s — "
			    "releasing the half-rate latch on every plane (#918 Phase 2a)",
			    XB_TAG(xb), (double)XB_BANDWIDTH_GATE_BYTES_PER_S / 1.0e9,
			    (unsigned long long)(XB_GATE_UNLATCH_NS / 1000000000ull));
		}
		return;
	}
	xb->gate_under_since_ns = 0;

	if (plane_rate < XB_GATE_PLANE_FLOOR_BYTES_PER_S) {
		// Over budget, but not because of the planes. Halving one would cost
		// the 2D update rate and buy nothing measurable.
		if (!xb->gate_atlas_only_warned) {
			xb->gate_atlas_only_warned = true;
			U_LOG_W(
			    "%s: transport measured %.2f GB/s, above the %.1f GB/s acceptance gate, "
			    "but the planes account for only %.2f GB/s of it — the atlas is the cost and the "
			    "gate has no lever on it, so nothing is throttled (#918 Phase 2a)",
			    XB_TAG(xb), (double)total_rate / 1.0e9, (double)XB_BANDWIDTH_GATE_BYTES_PER_S / 1.0e9,
			    (double)plane_rate / 1.0e9);
		}
		return;
	}
	if (worst >= COMP_XBRIDGE_PLANE_COUNT) {
		return; // every live plane is already latched
	}
	xb->plane[worst].half_rate = true;
	xb->plane[worst].half_rate_parity = seq & 1ull;
	U_LOG_W(
	    "%s: transport measured %.2f GB/s over the last second (planes %.2f GB/s), above "
	    "the %.1f GB/s acceptance gate — latching the %s plane to HALF RATE until the transport "
	    "settles. The split stays on (#918 Phase 2a)",
	    XB_TAG(xb), (double)total_rate / 1.0e9, (double)plane_rate / 1.0e9,
	    (double)XB_BANDWIDTH_GATE_BYTES_PER_S / 1.0e9, xb_plane_name(worst));
}


/*!
 * #1017 posture: the cross-adapter fence is the one place a hybrid stack has
 * historically wedged with no diagnostic. Model taken from the present watchdog
 * in comp_d3d11_target.cpp — 4 Hz, escalating WARNs, and diagnosis only, except
 * that at 5 s this one also latches `degraded` so the panel keeps showing the
 * last good frame instead of hanging on a bridge that is never coming back.
 *
 * #918 F3: BOTH links are monitored, not just the producer's. Watching
 * `f_xa_prod` alone misses a wedged CONSUMER queue entirely — a submit whose
 * allocator is still busy returns BEFORE leg 1, so `last_submit_seq` stops
 * advancing, `submitted <= prod_done` holds forever, and the panel freezes with
 * a permanently "healthy" watchdog. Hence the third clause: frames are still
 * being ATTEMPTED and the consumer fence is not moving.
 */
static void
xb_watchdog_thread(struct comp_xbridge *xb)
{
	uint64_t last_prod_done = 0;
	uint64_t last_cons_done = 0;
	uint64_t last_attempted = 0;
	uint64_t stall_since_ns = os_monotonic_get_ns();
	int stage = 0;

	while (!xb->quit.load(std::memory_order_relaxed)) {
		os_nanosleep(250 * 1000 * 1000);
		if (xb->quit.load(std::memory_order_relaxed)) {
			break;
		}

		const uint64_t prod_target = xb->prod_submit_seq.load(std::memory_order_acquire);
		const uint64_t attempted = xb->attempted.load(std::memory_order_acquire);
		const uint64_t prod_done = (xb->f_xa_prod != nullptr) ? xb->f_xa_prod->GetCompletedValue() : 0;
		const uint64_t cons_done = (xb->f_out_cons != nullptr) ? xb->f_out_cons->GetCompletedValue() : 0;

		const bool prod_stall = prod_target > prod_done;
		const bool cons_stall = prod_done > cons_done;
		// Nothing new submitted, but the compositor keeps asking — the
		// allocator-busy skip loop, which no fence comparison above can see.
		const bool attempt_stall = (attempted > last_attempted) && (cons_done == last_cons_done);
		const bool advanced = (prod_done != last_prod_done) || (cons_done != last_cons_done);

		last_attempted = attempted;

		if (advanced || !(prod_stall || cons_stall || attempt_stall)) {
			if (stage > 0) {
				U_LOG_W("%s: bridge recovered at prod=%llu cons=%llu (#1017)", XB_TAG(xb),
				        (unsigned long long)prod_done, (unsigned long long)cons_done);
			}
			last_prod_done = prod_done;
			last_cons_done = cons_done;
			stall_since_ns = os_monotonic_get_ns();
			stage = 0;
			continue;
		}

		const int64_t ms = (int64_t)(os_monotonic_get_ns() - stall_since_ns) / 1000000;
		int want = 0;
		if (ms >= 30000) {
			want = 3;
		} else if (ms >= 5000) {
			want = 2;
		} else if (ms >= 2000) {
			want = 1;
		}
		if (want > stage) {
			stage = want;
			const char *link = prod_stall ? "PRODUCER link (app adapter -> cross-adapter ring)"
			                              : (cons_stall ? "CONSUMER link (cross-adapter ring -> egress)"
			                                            : "CONSUMER link (frames attempted, no egress "
			                                              "progress — allocators never free)");
			U_LOG_W(
			    "%s: %s stalled for %lld ms — submitted=%llu prod=%llu cons=%llu "
			    "attempts=%llu (#1017)",
			    XB_TAG(xb), link, (long long)ms, (unsigned long long)prod_target,
			    (unsigned long long)prod_done, (unsigned long long)cons_done,
			    (unsigned long long)attempted);
			if (want >= 2 && !xb->degraded.load(std::memory_order_relaxed)) {
				xb->degraded.store(true, std::memory_order_release);
				U_LOG_W(
				    "%s: DEGRADED — no further submissions; the last good "
				    "egress slot keeps being woven (#1017)",
				    XB_TAG(xb));
			}
		}
	}
}

/*!
 * #918 Phase 2b PR 6 — the watchdog runs inside the ALWAYS-ON service now, not
 * just a session process. An exception escaping a thread entry has no handler
 * anywhere above it, so it is `std::terminate` with no banner and no stack
 * (#930/#950) — and taking the service down takes every client's session with
 * it. `u_crash_guard_run` wraps the body in the same structured-exception filter
 * the render thread already uses: the exception still propagates
 * (`EXCEPTION_CONTINUE_SEARCH`), so nothing about the outcome changes, but a
 * `[TERMINATE] thread 'xbridge-watchdog'` record with a module+offset stack
 * lands in the log first. Diagnosis only — this is not a catch.
 */
static void *
xb_watchdog_entry(void *arg)
{
	xb_watchdog_thread(static_cast<struct comp_xbridge *>(arg));
	return nullptr;
}


/*
 *
 * Stage A.
 *
 */

extern "C" xrt_result_t
comp_xbridge_create(const struct comp_xbridge_info *info, struct comp_xbridge **out_xb, const char **out_reason)
{
	char b[32];
	HRESULT hr;
	*out_reason = "unknown";

	// Value-init zero-fills every POD member before the implicit constructor
	// runs (the std::thread / std::atomic members are what make it non-trivial),
	// so no member below is read uninitialised on a failure path.
	auto *xb = new comp_xbridge();
	xb->weave_slot = -1;
	xb->last_submit_seq.store(0);
	xb->prod_submit_seq.store(0);
	xb->attempted.store(0);
	xb->degraded.store(false);
	xb->quit.store(false);
	xb->max_w = info->max_width;
	xb->max_h = info->max_height;
	// #918 Phase 2a: the planes are panel-sized once. Nothing here allocates —
	// each plane's heap is created lazily on its first bind, so a session that
	// never uses Local2D or zones pays nothing.
	xb->panel_w = info->panel_width;
	xb->panel_h = info->panel_height;
	xb->ingress_mode = XB_INGRESS_DIRECT;
	xb->force_depth = 0;
	{
		const char *e = getenv("DXR_WEAVE_ON_SCANOUT_DEPTH");
		if (e != nullptr && e[0] == '1') {
			xb->force_depth = 1;
		}
	}
	{
		// #1178 — the content probe. Off by default and read once, so the frame
		// path's test is a bool rather than a getenv.
		const char *e = getenv("DXR_SPLIT_CONTENT_PROBE");
		xb->probe_on = (e != nullptr && e[0] == '1');
	}

	/*
	 * #1178 — THE ATLAS FORMAT, carried rather than assumed.
	 *
	 * Every hop of the atlas chain is a copy, and both `CopySubresourceRegion`
	 * and `CopyTextureRegion` require source and destination to share a DXGI
	 * TYPELESS FAMILY. `B8G8R8A8` and `R8G8B8A8` do not share one, and a copy
	 * across families is not an error either API reports — it is dropped, and the
	 * destination keeps whatever it held. The chain therefore has to be built in
	 * the caller's own format; anything else transports a perfectly-accounted
	 * nothing.
	 *
	 * Refusing an unknown format rather than defaulting it is deliberate: the
	 * bandwidth gate, the DIAG line and the cross-adapter heap footprint all
	 * depend on a bytes-per-pixel this unit can state, and a bridge that cannot
	 * state one has no business sizing a heap.
	 */
	xb->fmt = (info->atlas_format != 0) ? (DXGI_FORMAT)info->atlas_format : XB_FORMAT_DEFAULT;
	if (xb->fmt != DXGI_FORMAT_R8G8B8A8_UNORM && xb->fmt != DXGI_FORMAT_B8G8R8A8_UNORM) {
		U_LOG_E(
		    "%s: unsupported atlas format %s (0x%x) — the bridge builds its ingress, cross-adapter and "
		    "egress rings in the CALLER's format and only the 8-bit BGRA/RGBA UNORM pair is supported "
		    "(#1178)",
		    XB_TAG(xb), xb_fmt_name(xb->fmt), (unsigned)xb->fmt);
		*out_reason = "unsupported atlas format";
		goto fail;
	}
	xb->bpp = xb_format_bpp((uint32_t)xb->fmt);

	xb->d3d12_ends = info->d3d12_ends;

	if (xb->d3d12_ends) {
		/*
		 * #918 D12-3a — D3D12 ends. The producer and the consumer are not devices
		 * the bridge creates; they ARE the caller's two devices. That is the whole
		 * saving: every NT share below exists only to reach across an API
		 * boundary, and with D3D12 on both ends there is no boundary left.
		 *
		 * `prod_dev` / `cons_dev` are AddRef'd aliases, so all the middle code —
		 * heaps, placed resources, queues, allocators, lists, the watchdog — is
		 * reached completely unchanged.
		 */
		// Validated BEFORE anything is stored: `fail` releases every member, so
		// a pointer parked in the struct without its AddRef would be released on
		// a reference the bridge never took.
		if (info->app_device == nullptr || info->app_queue == nullptr || info->out_device == nullptr ||
		    info->out_queue == nullptr) {
			U_LOG_W("%s: D3D12 ends require app/out device AND queue", XB_TAG(xb));
			*out_reason = "D3D12 ends incomplete";
			goto fail;
		}
		xb->app_dev12 = static_cast<ID3D12Device *>(info->app_device);
		xb->app_q12 = static_cast<ID3D12CommandQueue *>(info->app_queue);
		xb->out_dev12 = static_cast<ID3D12Device *>(info->out_device);
		xb->out_q12 = static_cast<ID3D12CommandQueue *>(info->out_queue);
		xb->app_dev12->AddRef();
		xb->app_q12->AddRef();
		xb->out_dev12->AddRef();
		xb->out_q12->AddRef();

		xb->prod_dev = xb->app_dev12;
		xb->prod_dev->AddRef();
		xb->cons_dev = xb->out_dev12;
		xb->cons_dev->AddRef();
	} else {
		xb->app_dev = static_cast<ID3D11Device *>(info->app_device);
		xb->app_ctx = static_cast<ID3D11DeviceContext *>(info->app_context);
		xb->out_dev = static_cast<ID3D11Device *>(info->out_device);
		xb->out_ctx = static_cast<ID3D11DeviceContext *>(info->out_context);
		xb->app_dev->AddRef();
		xb->app_ctx->AddRef();
		xb->out_dev->AddRef();
		if (xb->out_ctx != nullptr) {
			xb->out_ctx->AddRef();
			// Cached once: gpu_wait_slot runs on the weave path, which must not QI
			// per frame (#918 F9). A failure here only costs the GPU-side wait.
			if (FAILED(xb->out_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
			                                       reinterpret_cast<void **>(&xb->out_ctx4)))) {
				xb->out_ctx4 = nullptr;
			}
		}

		hr = xb->app_dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&xb->app_dev5));
		if (SUCCEEDED(hr)) {
			hr = xb->app_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
			                                 reinterpret_cast<void **>(&xb->app_ctx4));
		}
		if (FAILED(hr) || xb->app_dev5 == nullptr || xb->app_ctx4 == nullptr) {
			U_LOG_W("%s: app device has no ID3D11Device5/Context4 %s", XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
			*out_reason = "app device lacks D3D11.4 fences";
			goto fail;
		}
		hr = xb->out_dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&xb->out_dev5));
		if (FAILED(hr)) {
			xb->out_dev5 = nullptr; // non-fatal: only costs the GPU-side egress wait
		}

		// --- D3D12 devices + COPY queues ---------------------------------
		hr = D3D12CreateDevice(static_cast<IDXGIAdapter *>(info->app_adapter), D3D_FEATURE_LEVEL_11_0,
		                       __uuidof(ID3D12Device), reinterpret_cast<void **>(&xb->prod_dev));
		if (FAILED(hr)) {
			U_LOG_W("%s: D3D12CreateDevice(producer/app adapter) failed %s", XB_TAG(xb),
			        xb_hr(hr, b, sizeof(b)));
			*out_reason = "D3D12CreateDevice failed";
			goto fail;
		}
		hr = D3D12CreateDevice(static_cast<IDXGIAdapter *>(info->out_adapter), D3D_FEATURE_LEVEL_11_0,
		                       __uuidof(ID3D12Device), reinterpret_cast<void **>(&xb->cons_dev));
		if (FAILED(hr)) {
			U_LOG_W("%s: D3D12CreateDevice(consumer/scanout adapter) failed %s", XB_TAG(xb),
			        xb_hr(hr, b, sizeof(b)));
			*out_reason = "D3D12CreateDevice failed";
			goto fail;
		}
	}

	{
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		hr = xb->prod_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
		                                      reinterpret_cast<void **>(&xb->prod_q));
		if (SUCCEEDED(hr)) {
			hr = xb->cons_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
			                                      reinterpret_cast<void **>(&xb->cons_q));
		}
		for (int i = 0; SUCCEEDED(hr) && i < XB_ALLOC_RING; i++) {
			hr = xb->prod_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
			                                          __uuidof(ID3D12CommandAllocator),
			                                          reinterpret_cast<void **>(&xb->prod_alloc[i]));
			if (SUCCEEDED(hr)) {
				hr = xb->cons_dev->CreateCommandAllocator(
				    D3D12_COMMAND_LIST_TYPE_COPY, __uuidof(ID3D12CommandAllocator),
				    reinterpret_cast<void **>(&xb->cons_alloc[i]));
			}
		}
		if (SUCCEEDED(hr)) {
			hr = xb->prod_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, xb->prod_alloc[0],
			                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
			                                     reinterpret_cast<void **>(&xb->prod_list));
		}
		if (SUCCEEDED(hr)) {
			xb->prod_list->Close();
			hr = xb->cons_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, xb->cons_alloc[0],
			                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
			                                     reinterpret_cast<void **>(&xb->cons_list));
		}
		if (SUCCEEDED(hr)) {
			xb->cons_list->Close();
		}
		if (FAILED(hr)) {
			U_LOG_W("%s: D3D12 copy queue/allocator/list failed %s", XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
			*out_reason = "D3D12CreateDevice failed";
			goto fail;
		}
	}

	// --- cross-adapter heap + placed ring --------------------------------
	{
		D3D12_RESOURCE_DESC td{};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = xb->max_w;
		td.Height = xb->max_h;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.Format = xb->fmt;
		td.SampleDesc.Count = 1;
		td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for cross-adapter
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		xb->prod_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);

		const UINT64 align = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		D3D12_HEAP_DESC hd{};
		hd.SizeInBytes = (total + align - 1) & ~(align - 1);
		hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		hd.Alignment = align;
		hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
		                              D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES);

		// #1178: the FORMAT is on this line because it is now a property of the
		// caller rather than of the unit, and a mis-declared one is invisible in
		// every other diagnostic the transport emits.
		U_LOG_W("%s: cross-adapter ring %ux%u %s rowPitch=%u heap=%llu bytes x%d", XB_TAG(xb), xb->max_w,
		        xb->max_h, xb_fmt_name(xb->fmt), fp.Footprint.RowPitch, (unsigned long long)hd.SizeInBytes,
		        XB_XA_RING);

		for (int i = 0; i < XB_XA_RING; i++) {
			hr = xb->prod_dev->CreateHeap(&hd, __uuidof(ID3D12Heap),
			                              reinterpret_cast<void **>(&xb->xa_heap_prod[i]));
			if (SUCCEEDED(hr)) {
				hr = xb->prod_dev->CreateSharedHandle(xb->xa_heap_prod[i], nullptr, GENERIC_ALL,
				                                      nullptr, &xb->xa_heap_h[i]);
			}
			if (SUCCEEDED(hr)) {
				hr = xb->cons_dev->OpenSharedHandle(xb->xa_heap_h[i], __uuidof(ID3D12Heap),
				                                    reinterpret_cast<void **>(&xb->xa_heap_cons[i]));
			}
			if (SUCCEEDED(hr)) {
				hr = xb->prod_dev->CreatePlacedResource(
				    xb->xa_heap_prod[i], 0, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
				    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&xb->xa_prod[i]));
			}
			if (SUCCEEDED(hr)) {
				hr = xb->cons_dev->CreatePlacedResource(
				    xb->xa_heap_cons[i], 0, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
				    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&xb->xa_cons[i]));
			}
			if (FAILED(hr)) {
				U_LOG_W("%s: cross-adapter heap/placed texture %d failed %s", XB_TAG(xb), i,
				        xb_hr(hr, b, sizeof(b)));
				*out_reason = "cross-adapter heap unsupported";
				goto fail;
			}
		}
	}

	// --- fences -----------------------------------------------------------
	if (xb->d3d12_ends) {
		// The app queue and the producer queue are on the SAME device, so the
		// app->producer ordering fence needs no share at all: one plain
		// ID3D12Fence, signalled from `app_q12` in submit and waited on by
		// `prod_q` there too. Same fence role, same seq, one API call.
		hr = xb->prod_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
		                               reinterpret_cast<void **>(&xb->f_app_prod));
	} else {
		hr = xb->app_dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
		                               reinterpret_cast<void **>(&xb->f_app));
		if (SUCCEEDED(hr)) {
			hr = xb->f_app->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &xb->f_app_h);
		}
		if (SUCCEEDED(hr)) {
			hr = xb->prod_dev->OpenSharedHandle(xb->f_app_h, __uuidof(ID3D12Fence),
			                                    reinterpret_cast<void **>(&xb->f_app_prod));
		}
	}
	if (FAILED(hr)) {
		U_LOG_W("%s: app fence share failed %s", XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		*out_reason = "app fence share failed";
		goto fail;
	}

	hr = xb->prod_dev->CreateFence(
	    0, (D3D12_FENCE_FLAGS)(D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER),
	    __uuidof(ID3D12Fence), reinterpret_cast<void **>(&xb->f_xa_prod));
	if (SUCCEEDED(hr)) {
		hr = xb->prod_dev->CreateSharedHandle(xb->f_xa_prod, nullptr, GENERIC_ALL, nullptr, &xb->f_xa_h);
	}
	if (SUCCEEDED(hr)) {
		hr = xb->cons_dev->OpenSharedHandle(xb->f_xa_h, __uuidof(ID3D12Fence),
		                                    reinterpret_cast<void **>(&xb->f_xa_cons));
	}
	if (FAILED(hr)) {
		U_LOG_W("%s: cross-adapter fence share failed %s", XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		*out_reason = "cross-adapter heap unsupported";
		goto fail;
	}
	/*
	 * #918 F6 — the ingress BACK-fence. In Option I the producer copies straight
	 * out of the renderer's atlas, so without this the app's next frame overwrites
	 * the atlas while that copy is still reading it: torn frames, and the tear
	 * lands in whatever the panel is showing. Re-opening the producer fence on the
	 * APP D3D11 device (same adapter, so a plain shared-fence open) lets the app's
	 * renderer passes take a GPU-QUEUE wait on it — never a CPU one.
	 *
	 * If the open fails, Option I is simply unsafe and is NOT kept: the staged
	 * ingress ring is latched instead, whose per-seq slot gives the copy a full
	 * ring of margin.
	 */
	if (xb->d3d12_ends) {
		/*
		 * #918 D12-3a — with D3D12 ends the back-fence needs no re-open: the app
		 * device IS the producer device, so `f_xa_prod` is already a fence the
		 * app's own queue can wait on. The failure mode this branch exists to
		 * handle therefore cannot occur, which is why Option I is unconditionally
		 * safe here and the staged fallback is unreachable (and refused).
		 */
		xb->app_back_wait_ok = true;
	} else if (SUCCEEDED(hr = xb->app_dev5->OpenSharedFence(xb->f_xa_h, __uuidof(ID3D11Fence),
	                                                        reinterpret_cast<void **>(&xb->f_xa_app))) &&
	           xb->f_xa_app != nullptr) {
		xb->app_back_wait_ok = true;
	} else {
		xb->app_back_wait_ok = false;
		U_LOG_W(
		    "%s: the app device could not open the producer fence %s — ingress Option I "
		    "has no back-pressure and is unsafe; latching the staged ingress ring (#918)",
		    XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		if (!xb_latch_staged_ingress(xb, "ingress Option I has no back-pressure on this device")) {
			*out_reason = "ingress back-fence unavailable";
			goto fail;
		}
	}

	// D3D12 ends: the out queue lives on the consumer device, so the fence needs
	// no share and no second open — hence FLAG_NONE and no CreateSharedHandle.
	hr = xb->cons_dev->CreateFence(0, xb->d3d12_ends ? D3D12_FENCE_FLAG_NONE : D3D12_FENCE_FLAG_SHARED,
	                               __uuidof(ID3D12Fence), reinterpret_cast<void **>(&xb->f_out_cons));
	if (SUCCEEDED(hr) && !xb->d3d12_ends) {
		hr = xb->cons_dev->CreateSharedHandle(xb->f_out_cons, nullptr, GENERIC_ALL, nullptr, &xb->f_out_h);
	}
	if (FAILED(hr)) {
		U_LOG_W("%s: consumer fence share failed %s", XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		*out_reason = "egress share failed";
		goto fail;
	}
	// Same-adapter open on the OUTPUT D3D11 device. Optional: without it the
	// slot pick is still CPU-verified complete before the weave, we simply lose
	// the belt-and-braces GPU ordering (and the forced-depth mode's guarantee).
	//
	// D3D12 ends: unconditionally available. `out_q12` is on the consumer device,
	// so it waits on `f_out_cons` itself — the optionality here was never about
	// the hardware, only about whether the fence could be re-opened across the
	// API boundary, and there is no boundary. That in turn makes
	// comp_xbridge_pick_inflight_slot (the #918 R1 transition pick) always
	// available on this path rather than conditionally.
	xb->out_gpu_wait_ok = xb->d3d12_ends;
	if (!xb->d3d12_ends && xb->out_dev5 != nullptr && xb->out_ctx4 != nullptr) {
		hr = xb->out_dev5->OpenSharedFence(xb->f_out_h, __uuidof(ID3D11Fence),
		                                   reinterpret_cast<void **>(&xb->f_out_out11));
		if (SUCCEEDED(hr) && xb->f_out_out11 != nullptr) {
			xb->out_gpu_wait_ok = true;
		} else {
			U_LOG_W(
			    "%s: output device could not open the consumer fence %s — "
			    "slot picks stay CPU-verified only",
			    XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		}
	}

	// --- probe egress round-trip -----------------------------------------
	// Stage B allocates the real ring; do ONE slot here so an unshareable
	// egress is a create-time fallback rather than a mid-session surprise.
	{
		const char *why = "egress share failed";
		if (!xb_make_egress_slot(xb, 0, 64, 64, &why)) {
			*out_reason = why;
			goto fail;
		}
		safe_release(xb->eg_12[0]);
		safe_close(xb->eg_share[0]);
		safe_release(xb->eg_srv[0]);
		safe_release(xb->eg_tex[0]);
	}

	if (xb->d3d12_ends) {
		U_LOG_W(
		    "%s: D3D12 ends — the producer IS the app device and the consumer IS the output device, "
		    "so the atlas and the egress ring are unshared, ingress is Option I with no open to fail, "
		    "and both GPU-side waits are direct queue waits. Planes are refused on this path "
		    "(#918 D12-3a)",
		    XB_TAG(xb));
	}

	xb->quit.store(false);
	xb->watchdog = std::thread([xb]() { (void)u_crash_guard_run("xbridge-watchdog", xb_watchdog_entry, xb); });

	*out_xb = xb;
	*out_reason = nullptr;
	return XRT_SUCCESS;

fail: {
	struct comp_xbridge *tmp = xb;
	const char *keep = *out_reason;
	comp_xbridge_destroy(&tmp);
	*out_reason = keep;
}
	return XRT_ERROR_D3D;
}


/*
 *
 * Stage B + ingress binding.
 *
 */

//! Rebuild the ring at @p w x @p h. On failure the ring is left released.
static bool
xb_alloc_egress(struct comp_xbridge *xb, uint32_t w, uint32_t h, bool content_sized)
{
	xb_release_egress(xb);

	const char *why = "egress share failed";
	for (int i = 0; i < XB_EGRESS_RING; i++) {
		if (!xb_make_egress_slot(xb, i, w, h, &why)) {
			xb_release_egress(xb);
			return false;
		}
	}
	xb->eg_w = w;
	xb->eg_h = h;
	xb->eg_content_sized = content_sized;
	xb->weave_slot = -1;
	xb->eg_reallocs++;
	U_LOG_W("%s: egress ring x%d at %ux%u on the scanout adapter (%s), rebuild #%llu (#918)", XB_TAG(xb),
	        XB_EGRESS_RING, w, h, content_sized ? "content-sized" : "worst-case",
	        (unsigned long long)xb->eg_reallocs);
	return true;
}

extern "C" bool
comp_xbridge_alloc_worstcase_egress(struct comp_xbridge *xb)
{
	if (xb == nullptr || xb->max_w == 0 || xb->max_h == 0) {
		return false;
	}
	return xb_alloc_egress(xb, xb->max_w, xb->max_h, false);
}

extern "C" bool
comp_xbridge_set_content_size(struct comp_xbridge *xb, uint32_t w, uint32_t h, uint64_t layout_gen)
{
	if (xb == nullptr || w == 0 || h == 0) {
		return false;
	}
	if (w > xb->max_w) {
		w = xb->max_w;
	}
	if (h > xb->max_h) {
		h = xb->max_h;
	}
	/*
	 * #918 R2 — resize hysteresis, BEFORE any of the equality shortcuts below.
	 *
	 * A mode switch changes the content box once; an interactive resize changes
	 * it on every mouse event of the drag. The per-size realloc that is right for
	 * the first is expensive for the second, and the cost is TIME on the frame
	 * path — measured, not theorised: a 1.2 s edge drag rebuilt the ring 12 times
	 * and the split delivered 29 frames where the non-split path delivered 50,
	 * with a 161 ms worst gap against its 77. Each rebuild drains the consumer
	 * fence and then releases and recreates three NT-shared textures, their SRVs
	 * and their D3D12 opens on the scanout adapter.
	 *
	 * So the SECOND same-generation change inside the window switches the ring to
	 * worst-case ONCE and leaves it there: the content still lands (top-left, at
	 * its own stride) and the caller crops it on the output device, so a
	 * correctly-sized weave lands every frame of the drag with no rebuild at all.
	 * One realloc settles back to a content-sized ring when the size holds still.
	 */
	{
		const uint64_t now = os_monotonic_get_ns();
		// A change that arrives WITH a new layout generation is a mode switch:
		// one step, then the size holds still. Only same-generation motion — a
		// resize drag — is churn. Generation 0 means "no frame has been composited
		// yet", i.e. the two setup calls that size the ring before the session
		// runs; those are never churn either. Without both clauses the warmup —
		// which steps through the mode's nominal box and then the first frame's
		// real one inside ~70 ms — would engage churn and commit the worst-case
		// ring during exactly the window Stage A exists to keep cheap.
		const bool gen_changed = (layout_gen == 0 || layout_gen != xb->size_gen);
		xb->size_gen = layout_gen;

		if (w != xb->req_w || h != xb->req_h) {
			const uint64_t prev_change_ns = xb->dims_change_ns;
			xb->req_w = w;
			xb->req_h = h;
			xb->dims_change_ns = now;

			if (xb->churn) {
				return true; // the worst-case ring absorbs it; no realloc
			}
			if (!gen_changed && prev_change_ns != 0 && (now - prev_change_ns) < XB_CHURN_WINDOW_NS) {
				xb->churn = true;
				xb->churn_entries++;
				U_LOG_W(
				    "%s: content box is churning (%ux%u after %llu ms) — holding a "
				    "WORST-CASE egress ring and cropping on the output device so weaves keep "
				    "landing through the resize (#918)",
				    XB_TAG(xb), w, h, (unsigned long long)((now - prev_change_ns) / (1000 * 1000)));
				if (xb->eg_w != xb->max_w || xb->eg_h != xb->max_h) {
					comp_xbridge_alloc_worstcase_egress(xb);
				}
				return true;
			}
			// First change in a while — a mode switch, not a drag. Fall through
			// to the ordinary content-sized realloc.
		} else if (xb->churn) {
			if ((now - xb->dims_change_ns) < XB_SETTLE_NS) {
				return true; // still inside the settle window
			}
			xb->churn = false;
			xb->churn_settles++;
			U_LOG_W(
			    "%s: content box settled at %ux%u — one realloc back to a "
			    "content-sized egress ring (#918)",
			    XB_TAG(xb), w, h);
			// Fall through: eg_* still holds the worst-case ring, so the
			// equality shortcut below cannot swallow this.
		}
	}
	/*
	 * #918 F5 (a): the ring already has EXACTLY these dims. That includes the
	 * case where the content box IS the worst-case atlas — a worst-case ring is
	 * then content-sized by definition, and reallocating it (every frame, since
	 * the flag would never stick) would tear the ring down and back up on the
	 * hot path for nothing.
	 */
	if (xb->eg_w == w && xb->eg_h == h) {
		if (!xb->eg_content_sized) {
			xb->eg_content_sized = true;
			U_LOG_W(
			    "%s: egress ring %ux%u already matches the content box — "
			    "no crop needed (#918)",
			    XB_TAG(xb), w, h);
		}
		return true;
	}
	/*
	 * (b) A size whose allocation already failed is not retried until the
	 * REQUEST changes. Without this a persistently-failing content size (out of
	 * iGPU memory, say) re-attempts the whole ring — and re-logs — every single
	 * frame, while the worst-case ring it falls back to works fine.
	 */
	if (xb->eg_fail_w == w && xb->eg_fail_h == h) {
		return false;
	}
	if (xb_alloc_egress(xb, w, h, true)) {
		xb->eg_fail_w = 0;
		xb->eg_fail_h = 0;
		return true;
	}
	// Never leave the session without an egress ring: restore the worst-case
	// one and let the caller crop on the output device instead. (c) One WARN per
	// transition — never per frame.
	xb->eg_fail_w = w;
	xb->eg_fail_h = h;
	U_LOG_W(
	    "%s: egress ring could not be sized to %ux%u — keeping the worst-case ring and "
	    "cropping on the output device; this size will not be retried (#918)",
	    XB_TAG(xb), w, h);
	comp_xbridge_alloc_worstcase_egress(xb);
	return false;
}

extern "C" bool
comp_xbridge_slot_layout(struct comp_xbridge *xb, int32_t slot, uint64_t *out_gen, uint32_t *out_w, uint32_t *out_h)
{
	if (xb == nullptr || slot < 0 || slot >= XB_EGRESS_RING || xb->eg_seq[slot] == 0) {
		return false;
	}
	*out_gen = xb->eg_gen[slot];
	*out_w = xb->eg_cw[slot];
	*out_h = xb->eg_ch[slot];
	return true;
}

/*!
 * Latch Option II — an app-device NT-shared staging ring the producer CAN open.
 * Costs one extra same-adapter copy per frame; runs only when the renderer's own
 * atlas could not be opened by the producer D3D12 device.
 */
static bool
xb_alloc_ingress_ring(struct comp_xbridge *xb)
{
	char b[32];
	if (xb->in_12[0] != nullptr) {
		return true; // already standing
	}
	for (int i = 0; i < XB_INGRESS_RING; i++) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = xb->max_w;
		td.Height = xb->max_h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = xb->fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = xb->app_dev->CreateTexture2D(&td, nullptr, &xb->in_tex[i]);
		IDXGIResource1 *dr = nullptr;
		if (SUCCEEDED(hr)) {
			hr = xb->in_tex[i]->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dr));
		}
		if (SUCCEEDED(hr)) {
			hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                            nullptr, &xb->in_h[i]);
			dr->Release();
		}
		if (SUCCEEDED(hr)) {
			hr = xb->prod_dev->OpenSharedHandle(xb->in_h[i], __uuidof(ID3D12Resource),
			                                    reinterpret_cast<void **>(&xb->in_12[i]));
		}
		if (FAILED(hr)) {
			U_LOG_W("%s: Option II ingress ring failed %s — bridge inoperative", XB_TAG(xb),
			        xb_hr(hr, b, sizeof(b)));
			return false;
		}
	}
	return true;
}

static bool
xb_latch_staged_ingress(struct comp_xbridge *xb, const char *why)
{
	if (xb->ingress_mode == XB_INGRESS_STAGED) {
		return true;
	}
	/*
	 * #918 D12-3a — there is no Option II with D3D12 ends, and the honest answer
	 * is to say so rather than to build one nothing needs.
	 *
	 * Option II exists for two reasons, and neither survives the move: the source
	 * may be un-openable (there is no open — the producer reads the caller's own
	 * resource), or its identity may change per frame (the fence-deferred retire
	 * in comp_xbridge_bind_atlas_resource already covers that, and covers it
	 * without a per-frame copy). Building it anyway would also need a command
	 * list on the app's DIRECT queue, which the bridge does not own and must not
	 * start owning — recording onto the caller's list is a contract change.
	 *
	 * Reaching here at all would mean a caller asked for staged ingress on this
	 * path; refusing keeps Option I, which is unconditionally safe here (the
	 * back-fence cannot fail — see the create path).
	 */
	if (xb->d3d12_ends) {
		U_LOG_W(
		    "%s: staged ingress is not available with D3D12 ends (%s) — staying on Option I, which "
		    "needs no share and no back-fence open on this path (#918 D12-3a)",
		    XB_TAG(xb), why);
		return false;
	}
	if (!xb_alloc_ingress_ring(xb)) {
		return false;
	}
	/*
	 * #918 PR 6: latching STAGED from ADAPTIVE is a demotion — the bound source
	 * is dropped, and dropping it means releasing an open the producer may still
	 * be reading. Retire it behind the fence exactly as a source change does.
	 */
	if (xb->atlas_12 != nullptr && xb->ingress_mode == XB_INGRESS_ADAPTIVE) {
		xb_retire_source(xb, xb->atlas_12);
		xb->atlas_12 = nullptr;
		xb->src_key = 0;
	}
	xb->src_armed = false;
	xb->ingress_mode = XB_INGRESS_STAGED;
	U_LOG_W(
	    "%s: %s — using the staged ingress ring "
	    "(one extra app-device copy per frame) (#918)",
	    XB_TAG(xb), why);
	return true;
}

/*!
 * #918 Phase 2b — the same latch, chosen deliberately rather than fallen back
 * to. See the header for who wants it and why the timing is load-bearing.
 */
extern "C" bool
comp_xbridge_force_staged_ingress(struct comp_xbridge *xb)
{
	if (xb == nullptr) {
		return false;
	}
	return xb_latch_staged_ingress(xb, "the caller selected staged ingress up front");
}

extern "C" bool
comp_xbridge_enable_adaptive_ingress(struct comp_xbridge *xb)
{
	if (xb == nullptr) {
		return false;
	}
	// #918 D12-3a: adaptive is Option I plus an Option-II fallback, and there is
	// no Option II with D3D12 ends. Refused, and nothing is lost — the thing
	// adaptive exists to make safe (a source whose identity changes) is exactly
	// what comp_xbridge_bind_atlas_resource's fence-deferred retire already does.
	if (xb->d3d12_ends) {
		U_LOG_W(
		    "%s: adaptive ingress is not available with D3D12 ends — Option I already handles a "
		    "changing source through the deferred-retire ring (#918 D12-3a)",
		    XB_TAG(xb));
		return false;
	}
	// The staging ring is the per-frame fallback and is not optional: without it
	// the frame a source change lands on has nothing to copy out of.
	if (!xb_alloc_ingress_ring(xb)) {
		return false;
	}
	if (xb->ingress_mode == XB_INGRESS_ADAPTIVE) {
		return true;
	}
	/*
	 * #918 F6 again: Option I lets the producer read a texture the app device
	 * keeps writing, and the ONLY thing that orders those is the back-fence. No
	 * fence ⟹ no Option I, ever — stay in plain Option II, which gives each copy
	 * a private staging slot and needs no ordering at all. Working bridge, one
	 * WARN, no adaptive.
	 */
	if (!xb->app_back_wait_ok) {
		xb->ingress_mode = XB_INGRESS_STAGED;
		U_LOG_W(
		    "%s: adaptive ingress refused — the app device has no producer back-fence, so "
		    "reading a source in place is unsafe; staying on the staged ingress ring (#918 F6)",
		    XB_TAG(xb));
		return true;
	}
	xb->ingress_mode = XB_INGRESS_ADAPTIVE;
	xb->src_armed = false;
	U_LOG_W(
	    "%s: ADAPTIVE ingress — a stable source is read in place (Option I); the frame a "
	    "source change lands on stages (Option II) and re-binds (#918 Phase 2b)",
	    XB_TAG(xb));
	return true;
}

/*!
 * Release every deferred Option-I open whose producer copy has retired. Called
 * on the caller's thread from `set_source` (and at quiesce); never waits.
 */
static void
xb_sweep_retired_sources(struct comp_xbridge *xb)
{
	if (xb->f_xa_prod == nullptr) {
		return;
	}
	const uint64_t done = xb->f_xa_prod->GetCompletedValue();
	for (int i = 0; i < XB_SRC_RETIRE; i++) {
		if (xb->src_retire[i].res != nullptr && done >= xb->src_retire[i].prod_seq) {
			safe_release(xb->src_retire[i].res);
			xb->src_retire[i].prod_seq = 0;
		}
	}
}

static void
xb_retire_source(struct comp_xbridge *xb, ID3D12Resource *res)
{
	if (res == nullptr) {
		return;
	}
	xb_sweep_retired_sources(xb);
	const uint64_t want = xb->prod_submit_seq.load(std::memory_order_acquire);
	for (int i = 0; i < XB_SRC_RETIRE; i++) {
		if (xb->src_retire[i].res == nullptr) {
			xb->src_retire[i].res = res;
			xb->src_retire[i].prod_seq = want;
			return;
		}
	}
	/*
	 * Every slot busy. Only reachable if the producer link has genuinely stopped
	 * (the watchdog's territory) while the caller keeps changing source — and the
	 * settle hysteresis above means a caller CANNOT keep changing source faster
	 * than the ring drains. So this is a tripwire.
	 *
	 * It LEAKS. The obvious alternative — drain the producer, then release — is a
	 * bounded CPU wait on the caller's thread, and the caller here is the
	 * service's render thread holding `render_mutex`: a 2 s stall there is a
	 * workspace-wide freeze, which is the entire #925 wedge class. "Unreachable"
	 * is not a licence to keep such a wait; every wedge in that epic was on a
	 * path someone believed unreachable. A dropped `ID3D12Resource` open is a
	 * bounded, countable resource loss instead, and `ing_leak` is in the
	 * telemetry precisely so it can be asserted zero.
	 */
	xb->ing_leak++;
	static uint64_t s_last_log_ns = 0;
	const uint64_t now_ns = os_monotonic_get_ns();
	if (now_ns - s_last_log_ns > 5ull * 1000ull * 1000ull * 1000ull) {
		s_last_log_ns = now_ns;
		U_LOG_E(
		    "[XBRIDGE] ingress retire ring full — DROPPING a superseded source open without release "
		    "(leaks=%llu). The producer link has stalled; never wait here, the render thread holds "
		    "render_mutex (#918 PR 6, throttled)",
		    (unsigned long long)xb->ing_leak);
	}
}

extern "C" bool
comp_xbridge_set_source(struct comp_xbridge *xb, void *nt_handle, uint64_t source_key)
{
	char b[32];
	if (xb == nullptr) {
		return false;
	}
	if (xb->ingress_mode != XB_INGRESS_ADAPTIVE) {
		// Option I latched by `bind_atlas` reads in place unconditionally;
		// Option II never does.
		return xb->ingress_mode == XB_INGRESS_DIRECT;
	}
	xb_sweep_retired_sources(xb);

	if (nt_handle == nullptr || source_key == 0) {
		// This frame's source is not shareable (or the caller has none). Stage
		// it, and keep the binding: the very next frame is usually the same
		// stable source again, and dropping the open would cost a re-bind.
		xb->src_armed = false;
		return false;
	}
	if (xb->atlas_12 != nullptr && xb->src_key == source_key) {
		xb->src_armed = true;
		return true;
	}

	/*
	 * The source CHANGED — a focus change, a controller attach or detach, or a
	 * reallocation of the same slot's texture (which a resize drag does on every
	 * mouse event).
	 *
	 * Drop the current binding immediately: from here until a new one settles,
	 * every frame stages, which is exactly what the bridge did before adaptive
	 * ingress and is the correct degrade. Then wait for the caller's key to hold
	 * STILL before paying for a re-open — see XB_SRC_SETTLE_NS for why a drag
	 * must not be able to drive one open per frame.
	 */
	xb->src_armed = false;
	if (xb->atlas_12 != nullptr) {
		xb_retire_source(xb, xb->atlas_12);
		xb->atlas_12 = nullptr;
	}
	xb->src_key = 0;

	const uint64_t now_ns = os_monotonic_get_ns();
	if (source_key != xb->src_pending_key) {
		if (xb->src_pending_key != 0) {
			xb->ing_churn++;
		}
		xb->src_pending_key = source_key;
		xb->src_pending_since_ns = now_ns;
		return false;
	}
	if (now_ns - xb->src_pending_since_ns < XB_SRC_SETTLE_NS) {
		return false;
	}

	ID3D12Resource *opened = nullptr;
	HRESULT hr = xb->prod_dev->OpenSharedHandle(static_cast<HANDLE>(nt_handle), __uuidof(ID3D12Resource),
	                                            reinterpret_cast<void **>(&opened));
	if (FAILED(hr) || opened == nullptr) {
		// Not fatal, and not a demotion: this source stages forever (its key
		// never binds), every other source still gets Option I.
		if (!xb->ing_open_warned) {
			xb->ing_open_warned = true;
			U_LOG_W(
			    "%s: the producer could not open an ingress source %s — that source stages "
			    "(#918 PR 6, once)",
			    XB_TAG(xb), xb_hr(hr, b, sizeof(b)));
		}
		return false;
	}
	xb->atlas_12 = opened;
	xb->src_key = source_key;
	xb->ing_rebind++;
	return false;
}

extern "C" void
comp_xbridge_take_ingress_stats(struct comp_xbridge *xb, struct comp_xbridge_ingress_stats *out)
{
	if (out == nullptr) {
		return;
	}
	if (xb == nullptr) {
		*out = {};
		return;
	}
	out->mode = xb->ingress_mode;
	out->direct = xb->ing_direct;
	out->staged = xb->ing_staged;
	out->rebind = xb->ing_rebind;
	out->churn = xb->ing_churn;
	out->leak = xb->ing_leak;
	xb->ing_direct = 0;
	xb->ing_staged = 0;
}

extern "C" bool
comp_xbridge_bind_atlas(struct comp_xbridge *xb, void *nt_handle, uint64_t generation)
{
	char b[32];
	if (xb == nullptr) {
		return false;
	}
	// #918 D12-3a: an NT handle is the D3D11-ends way in. A D3D12-ends caller
	// binds its atlas by pointer instead — see comp_xbridge_bind_atlas_resource.
	if (xb->d3d12_ends) {
		return false;
	}
	if (xb->ingress_mode == XB_INGRESS_STAGED) {
		return true; // already latched; nothing to re-open
	}
	if (xb->ingress_mode == XB_INGRESS_ADAPTIVE) {
		// An adaptive caller nominates its source per frame through
		// comp_xbridge_set_source; the session-long bind is not its model.
		return true;
	}
	if (nt_handle == nullptr) {
		return xb_latch_staged_ingress(xb, "the renderer atlas is not NT-shareable");
	}
	if (xb->atlas_12 != nullptr && xb->atlas_gen == generation) {
		return true;
	}

	/*
	 * #918 F2: the generation changed (mode switch / resize grew the atlas), so
	 * the old `atlas_12` is about to be released — but the producer's copy of the
	 * last submitted seq reads exactly that resource, and D3D12 will not keep it
	 * alive for us. Drain the producer link first; on timeout, LEAK it rather
	 * than hand the copy queue a freed resource (same rationale as F1).
	 */
	if (xb->atlas_12 != nullptr &&
	    !xb_drain_fence(xb, xb->f_xa_prod, xb->prod_submit_seq.load(std::memory_order_acquire), "atlas re-bind")) {
		xb->atlas_12 = nullptr; // deliberate leak — a UAF is not recoverable
		if (!xb->degraded.load(std::memory_order_relaxed)) {
			xb->degraded.store(true, std::memory_order_release);
			U_LOG_W(
			    "%s: DEGRADED — the producer queue would not drain for an atlas "
			    "re-bind; no further submissions (#918)",
			    XB_TAG(xb));
		}
	}
	safe_release(xb->atlas_12);
	HRESULT hr = xb->prod_dev->OpenSharedHandle(static_cast<HANDLE>(nt_handle), __uuidof(ID3D12Resource),
	                                            reinterpret_cast<void **>(&xb->atlas_12));
	if (FAILED(hr) || xb->atlas_12 == nullptr) {
		U_LOG_W("%s: producer OpenSharedHandle(atlas gen=%llu) failed %s", XB_TAG(xb),
		        (unsigned long long)generation, xb_hr(hr, b, sizeof(b)));
		return xb_latch_staged_ingress(xb,
		                               "the renderer atlas could not be opened by the producer D3D12 device");
	}
	xb->atlas_gen = generation;
	U_LOG_W("%s: producer bound the renderer atlas (generation %llu)", XB_TAG(xb), (unsigned long long)generation);
	return true;
}

extern "C" bool
comp_xbridge_bind_atlas_resource(struct comp_xbridge *xb, void *resource, uint64_t generation)
{
	if (xb == nullptr || !xb->d3d12_ends) {
		return false;
	}
	auto *res = static_cast<ID3D12Resource *>(resource);
	if (res == nullptr) {
		// Unbind. Same deferred retire as a re-bind: the producer's copy of the
		// last submitted seq may still be reading it.
		if (xb->atlas_12 != nullptr) {
			xb_retire_source(xb, xb->atlas_12);
			xb->atlas_12 = nullptr;
			xb->atlas_gen = 0;
		}
		return false;
	}
	if (xb->atlas_12 == res && xb->atlas_gen == generation) {
		return true;
	}

	/*
	 * Same #918 F2 hazard as the NT-handle bind — the producer's copy of the last
	 * submitted seq reads exactly the resource we are dropping — but handled with
	 * the STRONGER of the two tools the bridge already owns, not the weaker one.
	 *
	 * The D3D11 path above takes a bounded CPU DRAIN because it must release the
	 * open synchronously or lose track of it. Here there is nothing to drain FOR:
	 * this is a fence-deferred retire, the exact mechanism #918 PR 6 built for
	 * adaptive ingress precisely because a drain on a caller's frame thread is
	 * the #925 wedge class. So it costs no wait at all, and the ring-exhaustion
	 * tripwire (`ing_leak`, leak-never-wait) carries over unchanged.
	 *
	 * Note also what our reference means here: the app owns the atlas and keeps
	 * its own reference, so ours is not what keeps the resource alive — the
	 * retire is about not RELEASING under a live copy, which is the same
	 * invariant, reached without blocking anybody.
	 */
	if (xb->atlas_12 != nullptr) {
		xb_retire_source(xb, xb->atlas_12);
		xb->atlas_12 = nullptr;
		xb->ing_rebind++;
	}
	res->AddRef();
	xb->atlas_12 = res;
	xb->atlas_gen = generation;
	U_LOG_W(
	    "%s: producer bound the app's atlas resource %p directly (generation %llu) — no share, no "
	    "open, no drain (#918 D12-3a)",
	    XB_TAG(xb), (void *)res, (unsigned long long)generation);
	return true;
}


/*
 *
 * Per-frame submit.
 *
 */

/*!
 * #918 F7 (a): the egress slot this frame's consumer copy writes into.
 *
 * A plain `seq % XB_EGRESS_RING` will, every ring period, land on the very slot
 * the compositor published as `weave_slot` — the one a repaint tick is re-weaving
 * right now, and the one an app frame's DP is sampling. Choose instead the
 * oldest slot that is NOT the published one; with a depth of 3 there is always
 * one free.
 */
static int
xb_egress_write_slot(struct comp_xbridge *xb)
{
	const int32_t avoid = xb->weave_slot;
	int best = -1;
	uint64_t best_seq = UINT64_MAX;
	for (int i = 0; i < XB_EGRESS_RING; i++) {
		if ((int32_t)i == avoid) {
			continue;
		}
		if (xb->eg_seq[i] <= best_seq) {
			best_seq = xb->eg_seq[i];
			best = i;
		}
	}
	return best;
}

/*!
 * The #918 F6 back-fence: queue a GPU-side wait on the app's immediate context
 * for the last seq the producer queue was told to signal, so nothing the app
 * submits AFTER this point can overwrite a resource the producer's copy of an
 * earlier seq is still reading.
 *
 * Wait for the LAST SEQ THE PRODUCER QUEUE WAS ACTUALLY TOLD TO SIGNAL, not for
 * seq-1: a frame whose allocator was busy (or that arrived while the bridge was
 * degraded) never reaches leg 1, so its fence value is never signalled — and a
 * GPU wait on a value nothing will ever signal wedges the app's queue
 * permanently. This value is always backed by work already submitted, so the
 * wait is guaranteed to resolve.
 *
 * The immediate context is one ordered stream, so ONE wait per producer seq
 * covers every app-device write that follows it — hence the `app_waited_seq`
 * guard, which makes repeat calls within a frame free.
 */
static void
xb_app_back_wait(struct comp_xbridge *xb, uint64_t want)
{
	if (!xb->app_back_wait_ok) {
		return;
	}
	// Monotonic: the immediate context is one ordered stream, so a wait already
	// issued for a LATER seq subsumes this one.
	if (want == 0 || want <= xb->app_waited_seq) {
		return;
	}
	if (xb->d3d12_ends) {
		/*
		 * #918 D12-3a. Same wait, one indirection fewer: `f_xa_prod` lives on the
		 * producer device, which IS the app device, so the app's own queue waits
		 * on it directly instead of on a re-opened D3D11 mirror.
		 *
		 * This is a GPU-side wait ON THE APP QUEUE, and it must not be mistaken
		 * for the thing D12-2 forbids. What it waits for is the PRODUCER copy
		 * queue, on the app's own adapter; the producer in turn waits only on
		 * `f_app_prod`, which this same app queue signals. The output queue
		 * appears nowhere in that chain, so no app-side wait can ever be made to
		 * cover out-queue work — the data flows app -> out and the only
		 * cross-adapter ordering is `cons_q->Wait(f_xa_cons)`, never the inverse.
		 * And it is a queue wait, never a CPU one, so this thread does not block.
		 */
		xb->app_q12->Wait(xb->f_xa_prod, want);
	} else {
		xb->app_ctx4->Wait(xb->f_xa_app, want);
	}
	xb->app_waited_seq = want;
}

extern "C" void
comp_xbridge_pre_render(struct comp_xbridge *xb)
{
	if (xb == nullptr) {
		return;
	}
	if (xb->ingress_mode == XB_INGRESS_DIRECT) {
		xb_app_back_wait(xb, xb->prod_submit_seq.load(std::memory_order_acquire));
		return;
	}
	/*
	 * #918 PR 6 — adaptive. Only a submit that read its source IN PLACE has a
	 * producer copy reading app-owned pixels, so only those need to be fenced
	 * against this frame's writes. Targeting `prod_direct_seq` rather than the
	 * last submitted seq is what keeps a run of staged frames free of any
	 * ordering the staging ring already provides privately.
	 */
	if (xb->ingress_mode == XB_INGRESS_ADAPTIVE) {
		xb_app_back_wait(xb, xb->prod_direct_seq);
	}
}

extern "C" void
comp_xbridge_pre_plane_write(struct comp_xbridge *xb, uint32_t plane)
{
	if (xb == nullptr || plane >= COMP_XBRIDGE_PLANE_COUNT) {
		return;
	}
	/*
	 * #918 review, R1-adjacent. A plane source is opened by the producer
	 * directly (the Option-I shape), so the producer's copy of seq N-1 reads the
	 * app's texture — and the authored mask's staging copy runs on an OpenXR call
	 * of the app's, BEFORE layer_commit and therefore before the frame's
	 * pre_render wait. Nothing ordered that write against the in-flight read.
	 * Same mechanism, same fence, applied at the write instead.
	 *
	 * Unconditional on ingress mode, unlike pre_render: Option II gives the ATLAS
	 * a private staging slot, but the planes are direct either way.
	 */
	auto &pl = xb->plane[plane];
	if (!pl.live || !pl.src_bound) {
		return;
	}
	xb_app_back_wait(xb, xb->prod_submit_seq.load(std::memory_order_acquire));
}

extern "C" void
comp_xbridge_submit(struct comp_xbridge *xb,
                    uint64_t seq,
                    uint64_t layout_gen,
                    void *atlas_texture,
                    uint32_t content_w,
                    uint32_t content_h)
{
	if (xb == nullptr || seq == 0 || xb->degraded.load(std::memory_order_relaxed)) {
		return;
	}
	if (content_w == 0 || content_h == 0 || xb->eg_w == 0) {
		return;
	}
	// Counted BEFORE the allocator-busy early-outs below: the watchdog's whole
	// point is that a skipped frame is still an attempted frame (#918 F3).
	xb->attempted.fetch_add(1, std::memory_order_release);
	if (content_w > xb->eg_w) {
		content_w = xb->eg_w;
	}
	if (content_h > xb->eg_h) {
		content_h = xb->eg_h;
	}

	ID3D12Resource *src12 = xb->atlas_12;

	/*
	 * #918 PR 6 — which ingress this ONE frame uses. Latched Option I always
	 * reads in place; adaptive does so while the caller's nominated source
	 * matched the bound one (`set_source` armed it); everything else stages.
	 */
	const bool use_direct = (xb->ingress_mode == XB_INGRESS_DIRECT) ||
	                        (xb->ingress_mode == XB_INGRESS_ADAPTIVE && xb->src_armed && xb->atlas_12 != nullptr);

	// Option II: stage the content box into an app-device NT-shared texture the
	// producer can open. One extra same-adapter copy, still no CPU wait.
	// Unreachable with D3D12 ends — `use_direct` is always true there, because
	// the only mode is DIRECT and every latch to STAGED is refused at create.
	if (!use_direct) {
		const int in = (int)(seq % XB_INGRESS_RING);
		auto *atlas = static_cast<ID3D11Texture2D *>(atlas_texture);
		if (atlas == nullptr || xb->in_tex[in] == nullptr) {
			return;
		}
		/*
		 * #1178 — GUARD THE COPY. `xb->in_tex[in]` is allocated in `xb->fmt`; if
		 * the caller's atlas is a different typeless family this
		 * CopySubresourceRegion does nothing at all and says nothing at all.
		 * Refuse the frame instead.
		 */
		D3D11_TEXTURE2D_DESC ad = {};
		atlas->GetDesc(&ad);
		if (!xb_check_source_format(xb, xb->fmt, ad.Format, "ingress stage (Option II)")) {
			return;
		}
		D3D11_BOX box = {0, 0, 0, content_w, content_h, 1};
		xb->app_ctx->CopySubresourceRegion(xb->in_tex[in], 0, 0, 0, 0, atlas, 0, &box);
		src12 = xb->in_12[in];
	}
	if (src12 == nullptr) {
		return;
	}
	/*
	 * #1178 — and guard the D3D12 leg too. Option I hands the producer the
	 * caller's OWN resource, whose format nothing has ever checked against the
	 * cross-adapter ring's; Option II arrives here already checked, and re-testing
	 * its (bridge-owned) staging texture is free.
	 */
	{
		const D3D12_RESOURCE_DESC sd = src12->GetDesc();
		if (!xb_check_source_format(xb, xb->fmt, sd.Format,
		                            use_direct ? "atlas source (Option I)" : "ingress ring")) {
			return;
		}
	}

	// The app's writes to the atlas complete before the producer reads it. This
	// is a fence SIGNAL on the immediate context — no CPU wait. The Flush is
	// what makes the signal reach the GPU now rather than at the next present.
	//
	// D3D12 ends: the same signal on the app's own queue. There is no Flush
	// because there is nothing deferred — a queue Signal is already submitted
	// work — and no share, because the fence lives on the app device. The caller
	// owes us only that it executed the frame's atlas work on this queue first,
	// which is the header's stated contract.
	if (xb->d3d12_ends) {
		xb->app_q12->Signal(xb->f_app_prod, seq);
	} else {
		xb->app_ctx4->Signal(xb->f_app, seq);
		xb->app_ctx->Flush();
	}

	const int pa = (int)(seq % XB_ALLOC_RING);
	const int ca = pa;
	const int xa = (int)(seq % XB_XA_RING);
	const int eg = xb_egress_write_slot(xb);
	if (eg < 0) {
		return;
	}

	// Never CPU-wait for an allocator: if the seq that last used it is still in
	// flight the frame is simply not bridged, and the weave picks an older slot.
	if (xb->prod_alloc_seq[pa] != 0 && xb->f_xa_prod->GetCompletedValue() < xb->prod_alloc_seq[pa]) {
		xb->skip_alloc_busy++;
		return;
	}
	if (xb->cons_alloc_seq[ca] != 0 && xb->f_out_cons->GetCompletedValue() < xb->cons_alloc_seq[ca]) {
		xb->skip_alloc_busy++;
		return;
	}

	D3D12_BOX box{};
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = content_w;
	box.bottom = content_h;
	box.back = 1;

	/*
	 * #918 Phase 2a: BOTH command lists are opened first and closed last, with
	 * the atlas copy and every plane copy recorded between. That is the whole
	 * atomicity argument — one seq, one fence pair, so the DP can never sample an
	 * atlas from frame N beside a Local2D plane from frame N-1, and the layout
	 * refusal / slot-readiness / repaint republish that already guard the atlas
	 * guard the planes for free.
	 */
	if (FAILED(xb->prod_alloc[pa]->Reset()) || FAILED(xb->prod_list->Reset(xb->prod_alloc[pa], nullptr))) {
		return;
	}
	if (FAILED(xb->cons_alloc[ca]->Reset()) || FAILED(xb->cons_list->Reset(xb->cons_alloc[ca], nullptr))) {
		xb->prod_list->Close();
		return;
	}

	// Every resource here is COMMON and implicitly promotes to
	// COPY_SOURCE/COPY_DEST on a copy queue — no barriers, and D3D11-shared
	// resources must stay in COMMON anyway.
	{
		D3D12_TEXTURE_COPY_LOCATION dst = sub_loc(xb->xa_prod[xa]);
		D3D12_TEXTURE_COPY_LOCATION src = sub_loc(src12);
		xb->prod_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	}
	{
		D3D12_TEXTURE_COPY_LOCATION dst = sub_loc(xb->eg_12[eg]);
		D3D12_TEXTURE_COPY_LOCATION src = sub_loc(xb->xa_cons[xa]);
		xb->cons_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	}
	// #918 review F7: atlas and plane bytes stay separate all the way into the
	// gate — the gate can only throttle planes, so it must know which is which.
	// #1178: the multiplier is the CHAIN's bytes-per-pixel, not a hardcoded 4 —
	// the chain is built in the caller's format now, so the gate must be too.
	const uint64_t atlas_frame_bytes = (uint64_t)content_w * content_h * (uint64_t)xb->bpp;
	xb->atlas_bytes += atlas_frame_bytes;

	uint64_t plane_frame_bytes = 0;
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		plane_frame_bytes += xb_record_plane(xb, p, seq, xa, eg);
	}

	xb->prod_list->Close();
	xb->cons_list->Close();

	// --- leg 1: app adapter -> cross-adapter ring -------------------------
	xb->prod_q->Wait(xb->f_app_prod, seq);
	{
		ID3D12CommandList *l[] = {xb->prod_list};
		xb->prod_q->ExecuteCommandLists(1, l);
	}
	xb->prod_q->Signal(xb->f_xa_prod, seq);
	xb->prod_alloc_seq[pa] = seq;
	// Published for the watchdog's producer link, the F1/F2 drains and the
	// F6 back-fence — all of which need the last seq the producer queue was
	// told to signal, never a seq that was skipped.
	xb->prod_submit_seq.store(seq, std::memory_order_release);
	// #918 PR 6: `prod_direct_seq` is the back-fence target under adaptive
	// ingress and must only ever name a seq whose producer copy reads APP-OWNED
	// pixels. Recorded here, past every early-out, so a frame that never reached
	// leg 1 cannot arm a wait for pixels nothing is reading.
	if (use_direct) {
		xb->prod_direct_seq = seq;
		xb->ing_direct++;
	} else {
		xb->ing_staged++;
	}

	// --- leg 2: cross-adapter ring -> egress (scanout adapter) ------------
	// The ONE cross-adapter wait in the whole design, and it belongs to the
	// consumer's own copy queue.
	xb->cons_q->Wait(xb->f_xa_cons, seq);
	{
		ID3D12CommandList *l[] = {xb->cons_list};
		xb->cons_q->ExecuteCommandLists(1, l);
	}
	xb->cons_q->Signal(xb->f_out_cons, seq);
	xb->cons_alloc_seq[ca] = seq;
	xb->eg_seq[eg] = seq;
	// #918 R1/R2: stamp the slot with the layout that produced its pixels.
	// The weave refuses a superseded generation outright, and weaves a
	// surviving one with ITS OWN content box rather than the frame's.
	xb->eg_gen[eg] = layout_gen;
	xb->eg_cw[eg] = content_w;
	xb->eg_ch[eg] = content_h;

	/*
	 * #918 Phase 2a — stamp the COMPOSITE RECIPE. plane_valid/plane_seq are
	 * resolved from what the slot ACTUALLY holds after the recording above, not
	 * from what the frame wished for: a change-skipped plane keeps the seq it
	 * already carried, so the consume half can tell "same pixels, provably" from
	 * "older pixels" without a second channel.
	 */
	xb->eg_recipe[eg] = xb->staged_recipe;
	xb->eg_recipe[eg].plane_valid = 0;
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		auto &pl = xb->plane[p];
		xb->eg_recipe[eg].plane_seq[p] = pl.slot_seq[eg];
		/*
		 * #918 review F3 — `plane_valid` means "this slot's pixels ARE the
		 * generation the frame staged", not merely "this slot has some pixels".
		 * A change-skip satisfies it by construction, which is the whole point
		 * of the change-skip: the slot already holds exactly this generation.
		 * A HALF-RATE skip or an untransportable (empty-box) skip does not —
		 * those leave the slot on an older generation, and stamping them valid
		 * is what let a stale 2D band composite under a current frame with
		 * nothing anywhere able to tell.
		 */
		if (xb_plane_slot_is_valid(pl.live, pl.staged, pl.slot_seq[eg], pl.stage_seq)) {
			xb->eg_recipe[eg].plane_valid |= (1u << p);
		}
	}

	xb_bandwidth_gate(xb, atlas_frame_bytes, plane_frame_bytes, seq);

	xb->last_submit_seq.store(seq, std::memory_order_release);
	xb->frames++;
}


/*
 *
 * Slot selection + weave-side accessors.
 *
 */

extern "C" int32_t
comp_xbridge_pick_slot(struct comp_xbridge *xb, uint64_t want_gen)
{
	if (xb == nullptr || xb->eg_w == 0) {
		return -1;
	}
	const uint64_t submitted = xb->last_submit_seq.load(std::memory_order_acquire);
	const uint64_t done = xb->f_out_cons->GetCompletedValue();

	int32_t pick = -1;
	uint64_t best = 0;
	//! #918 R1: content HAS landed, but every landed slot predates the current
	//! layout generation. Weaving one is the offset-cube frame; refusing it and
	//! holding the last presented frame is the whole point of the stamp.
	bool saw_stale = false;

	if (xb->force_depth == 1) {
		// Deterministic one-frame pipeline: always the previous frame's slot,
		// GPU-waited rather than CPU-verified. Removes the pick jitter when the
		// question under measurement is latency, not throughput.
		if (submitted >= 2) {
			// Searched, not computed: the write slot is chosen to dodge the
			// published weave slot (#918 F7), so seq no longer maps to a slot
			// by modulo.
			const uint64_t want = submitted - 1;
			for (int32_t i = 0; i < XB_EGRESS_RING; i++) {
				if (xb->eg_seq[i] == want) {
					if (xb->eg_gen[i] != want_gen) {
						saw_stale = true;
						break;
					}
					pick = i;
					best = want;
					break;
				}
			}
		}
	}
	if (pick < 0) {
		for (int32_t i = 0; i < XB_EGRESS_RING; i++) {
			if (xb->eg_seq[i] != 0 && xb->eg_seq[i] <= done && xb->eg_seq[i] > best) {
				// #918 R1: never hand back a slot whose pixels were composited
				// under a layout the DP has since switched away from.
				if (xb->eg_gen[i] != want_gen) {
					saw_stale = true;
					continue;
				}
				best = xb->eg_seq[i];
				pick = i;
			}
		}
	}

	if (pick < 0 && saw_stale) {
		xb->pick_stale_gen++;
		xb->pick_none++;
	} else if (pick < 0) {
		xb->pick_none++;
	} else if (best == submitted) {
		xb->pick_now++;
	} else if (best + 1 == submitted) {
		xb->pick_prev++;
	} else {
		xb->pick_older++;
	}

	const uint64_t total = xb->pick_now + xb->pick_prev + xb->pick_older + xb->pick_none;
	if (total > 0 && (total % 1000) == 0) {
		U_LOG_I(
		    "%s: pick distribution over %llu weaves — seq=%llu seq-1=%llu older=%llu "
		    "none=%llu (of which stale-layout refusals %llu; alloc-busy skips %llu)",
		    XB_TAG(xb), (unsigned long long)total, (unsigned long long)xb->pick_now,
		    (unsigned long long)xb->pick_prev, (unsigned long long)xb->pick_older,
		    (unsigned long long)xb->pick_none, (unsigned long long)xb->pick_stale_gen,
		    (unsigned long long)xb->skip_alloc_busy);
	}
	return pick;
}

extern "C" int32_t
comp_xbridge_pick_inflight_slot(struct comp_xbridge *xb, uint64_t want_gen)
{
	/*
	 * #918 R1. Requires the GPU-side ordering: without the consumer fence open
	 * on the output device there is nothing to sequence the DP's sampling behind
	 * the copy that is still filling this slot, and the only honest answer is to
	 * present nothing.
	 */
	if (xb == nullptr || !xb->out_gpu_wait_ok || xb->eg_w == 0) {
		return -1;
	}
	int32_t pick = -1;
	uint64_t best = 0;
	for (int32_t i = 0; i < XB_EGRESS_RING; i++) {
		if (xb->eg_seq[i] != 0 && xb->eg_gen[i] == want_gen && xb->eg_seq[i] > best) {
			best = xb->eg_seq[i];
			pick = i;
		}
	}
	if (pick >= 0) {
		xb->pick_inflight++;
	}
	return pick;
}

extern "C" void
comp_xbridge_gpu_wait_slot(struct comp_xbridge *xb, int32_t slot)
{
	if (xb == nullptr || !xb->out_gpu_wait_ok || slot < 0 || slot >= XB_EGRESS_RING) {
		return;
	}
	const uint64_t want = xb->eg_seq[slot];
	if (want == 0) {
		return;
	}
	if (xb->d3d12_ends) {
		// The out queue is on the consumer device, so it waits on the consumer's
		// own fence — no mirror, no open, and (unlike the D3D11 path) never
		// unavailable. Still a GPU-side wait: the weave thread does not block.
		xb->out_q12->Wait(xb->f_out_cons, want);
		return;
	}
	// out_ctx4 is cached at create — out_gpu_wait_ok implies it is non-NULL.
	xb->out_ctx4->Wait(xb->f_out_out11, want);
}

extern "C" bool
comp_xbridge_slot_ready(struct comp_xbridge *xb, int32_t slot)
{
	if (xb == nullptr || slot < 0 || slot >= XB_EGRESS_RING) {
		return false;
	}
	if (xb->out_gpu_wait_ok) {
		return true; // the queue-side wait orders the weave behind the copy
	}
	const uint64_t want = xb->eg_seq[slot];
	if (want == 0 || xb->f_out_cons == nullptr) {
		return false;
	}
	return xb->f_out_cons->GetCompletedValue() >= want;
}

extern "C" void *
comp_xbridge_get_srv(struct comp_xbridge *xb, int32_t slot)
{
	if (xb == nullptr || slot < 0 || slot >= XB_EGRESS_RING) {
		return nullptr;
	}
	// D3D12 ends have no D3D11 view; `eg_srv` is NULL there anyway, but say so
	// rather than rely on that — see comp_xbridge_get_egress_resource.
	return xb->d3d12_ends ? nullptr : xb->eg_srv[slot];
}

extern "C" void *
comp_xbridge_get_egress_resource(struct comp_xbridge *xb, int32_t slot)
{
	if (xb == nullptr || !xb->d3d12_ends || slot < 0 || slot >= XB_EGRESS_RING) {
		return nullptr;
	}
	/*
	 * Guarded on `d3d12_ends` deliberately, not merely because the other path
	 * would return something odd: on a D3D11-ends bridge `eg_12[slot]` is the
	 * CONSUMER's open of an output-device D3D11 texture, and the consumer is a
	 * device the bridge created — not the caller's output device. Handing it back
	 * would give a caller a resource belonging to a device it has never seen,
	 * which is the exact class of mistake the #918 Phase 2 plane transports exist
	 * to prevent.
	 */
	return xb->eg_12[slot];
}

extern "C" void
comp_xbridge_get_egress_dims(struct comp_xbridge *xb, uint32_t *out_w, uint32_t *out_h)
{
	*out_w = (xb != nullptr) ? xb->eg_w : 0;
	*out_h = (xb != nullptr) ? xb->eg_h : 0;
}

/*!
 * #1178 — read the destination back and say what is actually in it.
 *
 * Two-phase and non-blocking. Phase one records a small `CopySubresourceRegion`
 * from the egress slot into a CPU-readable staging square and flushes; phase two,
 * on a LATER call, maps it with `DO_NOT_WAIT` and reports. A copy that has not
 * landed is simply retried next call — nothing here ever waits, which is the
 * bridge's standing invariant and not negotiable even for a diagnostic.
 *
 * What it reports is deliberately not a pixel dump: `nonzero` (how many of the
 * sampled pixels are not fully black) and a cheap additive checksum. Those two
 * numbers are all it takes to separate the three states the byte counters cannot
 * — transported content (nonzero high, sum moving frame to frame), transported a
 * static image (nonzero high, sum steady) and transported NOTHING (nonzero 0,
 * sum 0), which is what #1178 looked like from the inside.
 */
static void
xb_content_probe(struct comp_xbridge *xb, int32_t slot)
{
	if (xb->d3d12_ends) {
		if (!xb->probe_unavailable_warned) {
			xb->probe_unavailable_warned = true;
			U_LOG_W(
			    "%s: DXR_SPLIT_CONTENT_PROBE is set but this bridge has D3D12 ends, whose egress "
			    "readback needs a readback heap and a copy-queue round trip — probe not run (#1178)",
			    XB_TAG(xb));
		}
		return;
	}
	if (xb->out_ctx == nullptr) {
		return;
	}

	// --- phase two: the map, if a copy is already sitting in the staging square.
	if (xb->probe_pending) {
		D3D11_MAPPED_SUBRESOURCE m = {};
		HRESULT hr = xb->out_ctx->Map(xb->probe_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			return; // not landed yet — try again next call, never wait
		}
		if (FAILED(hr) || m.pData == nullptr) {
			xb->probe_pending = false;
			return;
		}
		uint64_t nonzero = 0, sum = 0, total = 0;
		const auto *base = static_cast<const uint8_t *>(m.pData);
		for (uint32_t y = 0; y < xb->probe_h; y++) {
			const uint8_t *row = base + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < xb->probe_w; x++) {
				const uint8_t *p = row + (size_t)x * xb->bpp;
				// Colour channels only — an opaque-black pixel has alpha 255 and
				// would otherwise count as content on every one of the three legs.
				const uint32_t lum = (uint32_t)p[0] + p[1] + p[2];
				total++;
				sum += lum;
				if (lum != 0) {
					nonzero++;
				}
			}
		}
		xb->out_ctx->Unmap(xb->probe_stage, 0);
		xb->probe_pending = false;
		xb->probe_last_ns = os_monotonic_get_ns();
		U_LOG_W(
		    "%s: CONTENT PROBE slot=%d %s %ux%u — nonzero=%llu/%llu sum=%llu (#1178). nonzero=0 means "
		    "the destination received NOTHING, however healthy the byte counters look.",
		    XB_TAG(xb), xb->probe_slot, xb_fmt_name(xb->fmt), xb->probe_w, xb->probe_h,
		    (unsigned long long)nonzero, (unsigned long long)total, (unsigned long long)sum);
		return;
	}

	// --- phase one: record the copy, at most once a period, on a landed slot.
	const uint64_t now = os_monotonic_get_ns();
	if (xb->probe_last_ns != 0 && now - xb->probe_last_ns < XB_PROBE_PERIOD_NS) {
		return;
	}
	if (slot < 0 || slot >= XB_EGRESS_RING || xb->eg_tex[slot] == nullptr || xb->eg_seq[slot] == 0) {
		return;
	}
	// Only a slot whose consumer copy is CPU-verified complete: reading one the
	// consumer is still writing would report a tear as a defect.
	if (xb->f_out_cons == nullptr || xb->f_out_cons->GetCompletedValue() < xb->eg_seq[slot]) {
		return;
	}

	const uint32_t pw = (xb->eg_w < XB_PROBE_EXTENT) ? xb->eg_w : XB_PROBE_EXTENT;
	const uint32_t ph = (xb->eg_h < XB_PROBE_EXTENT) ? xb->eg_h : XB_PROBE_EXTENT;
	if (pw == 0 || ph == 0) {
		return;
	}
	if (xb->probe_stage == nullptr || xb->probe_w != pw || xb->probe_h != ph) {
		safe_release(xb->probe_stage);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = pw;
		td.Height = ph;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = xb->fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(xb->out_dev->CreateTexture2D(&td, nullptr, &xb->probe_stage))) {
			xb->probe_stage = nullptr;
			// Latch off — one failed allocation must not be retried per frame.
			xb->probe_on = false;
			U_LOG_W("%s: content-probe staging texture could not be created — probe off (#1178)",
			        XB_TAG(xb));
			return;
		}
		xb->probe_w = pw;
		xb->probe_h = ph;
	}

	// The CENTRE of the atlas, not its corner: a tiled atlas's top-left is a view's
	// own edge and is legitimately black in plenty of scenes.
	const uint32_t x0 = (xb->eg_w - pw) / 2u;
	const uint32_t y0 = (xb->eg_h - ph) / 2u;
	D3D11_BOX box = {x0, y0, 0, x0 + pw, y0 + ph, 1};
	xb->out_ctx->CopySubresourceRegion(xb->probe_stage, 0, 0, 0, 0, xb->eg_tex[slot], 0, &box);
	xb->out_ctx->Flush();
	xb->probe_slot = slot;
	xb->probe_pending = true;
}

extern "C" void
comp_xbridge_content_probe(struct comp_xbridge *xb, int32_t slot)
{
	if (xb == nullptr || !xb->probe_on) {
		return;
	}
	xb_content_probe(xb, slot);
}

extern "C" void
comp_xbridge_set_weave_slot(struct comp_xbridge *xb, int32_t slot)
{
	if (xb != nullptr) {
		xb->weave_slot = slot;
	}
}

extern "C" int32_t
comp_xbridge_get_weave_slot(struct comp_xbridge *xb)
{
	return (xb != nullptr) ? xb->weave_slot : -1;
}

extern "C" bool
comp_xbridge_is_degraded(struct comp_xbridge *xb)
{
	return xb != nullptr && xb->degraded.load(std::memory_order_relaxed);
}


/*
 *
 * Teardown.
 *
 */

extern "C" void
comp_xbridge_quiesce(struct comp_xbridge *xb)
{
	// Idempotent: the compositor quiesces explicitly (before tearing the DP and
	// the target down) and destroy calls it again.
	if (xb == nullptr || xb->quit.load(std::memory_order_acquire)) {
		return;
	}
	// Stop submitting first, so the drain below has a fixed target.
	xb->degraded.store(true, std::memory_order_release);
	xb->quit.store(true, std::memory_order_release);
	if (xb->watchdog.joinable()) {
		xb->watchdog.join();
	}

	// Both links, bounded, producer first (it is the upstream one). #1017: a
	// hybrid stack that has wedged must not turn a session teardown into a hang —
	// but a timeout here means the releases below are unsafe, so it also latches
	// `drain_failed` and the destroy leaks instead of freeing (#918 F1/F2).
	if (!xb_drain_fence(xb, xb->f_xa_prod, xb->prod_submit_seq.load(std::memory_order_acquire),
	                    "teardown producer") ||
	    !xb_drain_fence(xb, xb->f_out_cons, xb->last_submit_seq.load(std::memory_order_acquire),
	                    "teardown consumer")) {
		xb->drain_failed = true;
	}

	const uint64_t total = xb->pick_now + xb->pick_prev + xb->pick_older + xb->pick_none;
	if (total > 0) {
		U_LOG_W(
		    "%s: %llu frames bridged; picks seq=%llu seq-1=%llu older=%llu none=%llu "
		    "in-flight=%llu (stale-layout refusals %llu; alloc-busy skips %llu); egress rebuilds %llu "
		    "(resize-churn entries %llu, settles %llu)",
		    XB_TAG(xb), (unsigned long long)xb->frames, (unsigned long long)xb->pick_now,
		    (unsigned long long)xb->pick_prev, (unsigned long long)xb->pick_older,
		    (unsigned long long)xb->pick_none, (unsigned long long)xb->pick_inflight,
		    (unsigned long long)xb->pick_stale_gen, (unsigned long long)xb->skip_alloc_busy,
		    (unsigned long long)xb->eg_reallocs, (unsigned long long)xb->churn_entries,
		    (unsigned long long)xb->churn_settles);
	}
	// #1178 — a session that refused anything ends by saying so. A refusal means
	// frames were NOT transported, and that must not be recoverable only from a
	// log line that scrolled past five seconds into the session.
	if (xb->fmt_refused > 0) {
		U_LOG_E("%s: %llu submits/binds REFUSED on a DXGI format mismatch — the chain is %s (0x%x) (#1178)",
		        XB_TAG(xb), (unsigned long long)xb->fmt_refused, xb_fmt_name(xb->fmt), (unsigned)xb->fmt);
	}
	if (xb->ingress_mode == XB_INGRESS_ADAPTIVE) {
		U_LOG_W(
		    "%s: adaptive ingress — %llu settled source re-binds, %llu churn changes that never "
		    "re-opened, %llu leaked opens (must be 0) (#918 PR 6)",
		    XB_TAG(xb), (unsigned long long)xb->ing_rebind, (unsigned long long)xb->ing_churn,
		    (unsigned long long)xb->ing_leak);
	}
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		const auto &pl = xb->plane[p];
		if (!pl.live && pl.copies == 0) {
			continue;
		}
		U_LOG_W("%s: %s plane — %llu copies, %llu change-skips%s (#918 Phase 2a)", XB_TAG(xb), xb_plane_name(p),
		        (unsigned long long)pl.copies, (unsigned long long)pl.skips,
		        pl.half_rate ? ", LATCHED to half rate by the bandwidth gate" : "");
	}
}

extern "C" void
comp_xbridge_destroy(struct comp_xbridge **xb_ptr)
{
	struct comp_xbridge *xb = *xb_ptr;
	if (xb == nullptr) {
		return;
	}
	*xb_ptr = nullptr;

	comp_xbridge_quiesce(xb);

	/*
	 * #1178 — the content probe's staging square. Output-device, CPU-readable and
	 * written only by an out-context copy this thread already issued, so it is
	 * outside the D3D12 drain-or-leak question below entirely.
	 */
	safe_release(xb->probe_stage);

	/*
	 * #918 F1/F2: if the bounded drain in quiesce timed out, copies may STILL be
	 * running against the D3D12 resources below. D3D12 has no deferred
	 * destruction, so releasing them here is a use-after-free on the copy queues
	 * — the thing that shows up as DEVICE_REMOVED. Leak the whole D3D12 side
	 * instead: holding the devices alive keeps every child alive with them, the
	 * memory is reclaimed when the process exits, and a leak is recoverable in a
	 * way a use-after-free is not.
	 */
	if (xb->drain_failed) {
		U_LOG_W(
		    "%s: teardown drain never completed — LEAKING the D3D12 side "
		    "(both devices, queues, the cross-adapter ring and the egress opens) rather than "
		    "freeing resources under in-flight copies (#918)",
		    XB_TAG(xb));
		safe_release(xb->f_out_out11);
		safe_release(xb->f_xa_app);
		safe_release(xb->out_ctx4);
		safe_release(xb->out_ctx);
		safe_release(xb->out_dev5);
		safe_release(xb->out_dev);
		safe_release(xb->app_ctx4);
		safe_release(xb->app_ctx);
		safe_release(xb->app_dev5);
		safe_release(xb->app_dev);
		/*
		 * #918 D12-3a — the ENDS are the caller's objects and our references on
		 * them are safe to drop (the caller holds its own), exactly as the D3D11
		 * ends above are. What is leaked is the D3D12 SIDE, and on this path
		 * `prod_dev` / `cons_dev` are aliases of these two devices — so leaking
		 * the D3D12 side leaks ONE reference on each of the app's and the
		 * runtime's device. That is the intended trade and it is not new: every
		 * queue, allocator and heap being leaked already holds a device reference
		 * internally, so the devices were going to outlive this teardown either
		 * way. A recoverable leak beats a use-after-free on a live copy queue.
		 */
		safe_release(xb->out_q12);
		safe_release(xb->out_dev12);
		safe_release(xb->app_q12);
		safe_release(xb->app_dev12);
		delete xb;
		return;
	}

	// Reverse of create: egress -> consumer -> cross-adapter heap -> producer
	// -> fences -> the D3D11 ends.
	xb_release_egress(xb);

	/*
	 * #918 Phase 2a: the planes go through the SAME drain-or-leak gate as the
	 * egress ring, and in the same drained window. Their egress textures are
	 * written by `cons_list` copies exactly like the atlas slots, so freeing one
	 * under an in-flight copy is the same use-after-free (#918 F1). xb_drain_fence
	 * returns immediately when the fence has already reached the target, so this
	 * costs nothing on the normal path.
	 */
	{
		const bool pl_drained = xb_drain_fence(
		    xb, xb->f_out_cons, xb->last_submit_seq.load(std::memory_order_acquire), "plane release");
		for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
			xb_release_plane(xb, p, pl_drained);
		}
	}

	for (int i = 0; i < XB_INGRESS_RING; i++) {
		safe_release(xb->in_12[i]);
		safe_close(xb->in_h[i]);
		safe_release(xb->in_tex[i]);
	}
	safe_release(xb->atlas_12);
	// #918 PR 6: the deferred-retire list. The producer drained above, so every
	// entry is provably past its copy; the leak branch above deliberately leaves
	// them alone with the rest of the D3D12 side.
	for (int i = 0; i < XB_SRC_RETIRE; i++) {
		safe_release(xb->src_retire[i].res);
	}

	for (int i = 0; i < XB_XA_RING; i++) {
		safe_release(xb->xa_cons[i]);
		safe_release(xb->xa_prod[i]);
		safe_release(xb->xa_heap_cons[i]);
		safe_release(xb->xa_heap_prod[i]);
		safe_close(xb->xa_heap_h[i]);
	}

	safe_release(xb->f_out_out11);
	safe_release(xb->f_out_cons);
	safe_close(xb->f_out_h);
	safe_release(xb->f_xa_app);
	safe_release(xb->f_xa_cons);
	safe_release(xb->f_xa_prod);
	safe_close(xb->f_xa_h);
	safe_release(xb->f_app_prod);
	safe_release(xb->f_app);
	safe_close(xb->f_app_h);

	safe_release(xb->cons_list);
	safe_release(xb->prod_list);
	for (int i = 0; i < XB_ALLOC_RING; i++) {
		safe_release(xb->cons_alloc[i]);
		safe_release(xb->prod_alloc[i]);
	}
	safe_release(xb->cons_q);
	safe_release(xb->prod_q);
	safe_release(xb->cons_dev);
	safe_release(xb->prod_dev);

	safe_release(xb->out_ctx4);
	safe_release(xb->out_ctx);
	safe_release(xb->out_dev5);
	safe_release(xb->out_dev);
	safe_release(xb->app_ctx4);
	safe_release(xb->app_ctx);
	safe_release(xb->app_dev5);
	safe_release(xb->app_dev);

	// #918 D12-3a — the D3D12 ends. Last, for the same reason the D3D11 ends are
	// last: `prod_dev` / `cons_dev` above are aliases of these, so the second
	// reference must not go until every child object has.
	safe_release(xb->out_q12);
	safe_release(xb->out_dev12);
	safe_release(xb->app_q12);
	safe_release(xb->app_dev12);

	delete xb;
}
