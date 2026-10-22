// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion pipeline composer (ADR-053 decision 3; roadmap
 *         conversion-providers.md §2.6): the runtime-composed stages S0–S8
 *         that replace the hard-wired paths of d3d11_lift's lift_convert_one.
 *
 * | Stage | What                                        | Present when                           |
 * |-------|---------------------------------------------|----------------------------------------|
 * | S0    | snapshot / import (producer thread)          | always                                  |
 * | S1    | letterbox crop + input cap                   | weave-rect sources                      |
 * | S2    | colour / format normalise                    | provider lacks the source format        |
 * | S3    | viewpoints + convergence + budget (policy)   | view-producing tasks                    |
 * | S4    | the provider                                 | always                                  |
 * | S5    | reference view synthesis (injected)          | composed pipelines                      |
 * | S6    | depth decode + per-frame range               | depth produced (or composed)            |
 * | S7    | observers (auto-convergence, cursor relief)  | a consumer enabled them                 |
 * | S8    | publish to the ring                          | always                                  |
 *
 * Execution order inside one conversion is S3, S4, S6, S5, S7, S8: the
 * reference view synthesis reads S6's range of the SAME inference.
 *
 * Graphics-API work is injected: the view synthesis (S5) through
 * conv_viewsyn_ops — the service hands in the D3D11 stage
 * (d3d11_conv_viewsyn), tests a CPU fake — and GPU depth readback for S6/S7
 * through conv_depth_readback_ops. S0–S2 are the GPU glue's (producer side).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_host_iface.h"
#include "conv_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Stages (conv_pipeline::stages) @{ */
#define CONV_STAGE_S0_SNAPSHOT (1u << 0)
#define CONV_STAGE_S1_LETTERBOX (1u << 1)
#define CONV_STAGE_S2_NORMALISE (1u << 2)
#define CONV_STAGE_S3_POLICY (1u << 3)
#define CONV_STAGE_S4_PROVIDER (1u << 4)
#define CONV_STAGE_S5_VIEWSYN (1u << 5)
#define CONV_STAGE_S6_DEPTH (1u << 6)
#define CONV_STAGE_S7_OBSERVERS (1u << 7)
#define CONV_STAGE_S8_PUBLISH (1u << 8)
/*! @} */

struct conv_pipeline_request
{
	uint32_t task;             //!< XRT_CONV_TASK_* the app asked for
	uint32_t input_kind;       //!< one XRT_CONV_INPUT_* bit; 0 = MONO
	uint32_t required_outputs; //!< XRT_CONV_OUT_* the app REQUIRES (refuse rather than degrade)
	uint32_t view_count;       //!< VIEWS_*: 0 = 2
	uint32_t views_layout;     //!< XRT_CONV_VIEWS_*
	uint32_t source_format;    //!< XRT_CONV_FMT_* of the snapshot; 0 = unknown
	bool weave_rect_source;    //!< a lift-flagged weave rect (S1 on)
	bool observers;            //!< a consumer enabled S7 observers
};

struct conv_pipeline
{
	uint32_t task;             //!< app-facing task
	uint32_t provider_task;    //!< the bound row's task (DEPTH_* when composed)
	bool composed;             //!< provider depth + the reference view synthesis
	uint32_t stages;           //!< CONV_STAGE_*
	uint32_t provider_outputs; //!< XRT_CONV_OUT_* asked of the provider
	uint32_t outputs;          //!< XRT_CONV_OUT_* the stream produces
	uint32_t view_count;       //!< 0 for non-view tasks
	uint32_t views_layout;
	uint32_t depth_kind; //!< XRT_CONV_DEPTH_* the stream's depth will carry
	//! Accepted against describe-time outputs while the capability is not
	//! READY; refined (conv_pipeline_refine) once it is.
	bool provisional;
	//! S7 observers asked for (conv_pipeline_request::observers): the
	//! provider's same-inference depth is requested for them even when the
	//! app did not ask for it (rt#1978's runtime auto-convergence, #2003) --
	//! it feeds S6 / S7 but is never published.
	bool observer_depth;
};

/*!
 * Compose the pipeline for one stream on capability @p cap (the selected row;
 * @p composed when selection picked a depth row for VIEWS_MONO).
 *
 * @return XRT_SUCCESS; XRT_ERROR_FEATURE_NOT_SUPPORTED when view_count is out
 *         of range, or when the capability is READY and lacks a required
 *         output (a non-READY capability is accepted provisionally: never a
 *         re-selection, discovery spec §3.4 rule 6).
 */
xrt_result_t
conv_pipeline_compose(const struct conv_pipeline_request *req,
                      const struct xrt_conv_capability *cap,
                      bool composed,
                      bool cap_ready,
                      struct conv_pipeline *out);

/*!
 * The bound capability became READY (refined describe in @p cap): clear
 * provisional, recompute outputs. Returns false when a REQUIRED output is
 * still missing: the stream cannot be served by this capability.
 */
bool
conv_pipeline_refine(struct conv_pipeline *p, uint32_t required_outputs, const struct xrt_conv_capability *cap);

//! VIEWS_MONO / VIEWS_FROM_DEPTH.
bool
conv_task_produces_views(uint32_t task);


/*
 *
 * Injected stages.
 *
 */

/*! One reference view synthesis (S5), in provider-neutral terms; the
 *  service's adapter maps it onto u_conv_viewsyn_params. */
struct conv_viewsyn_request
{
	const struct xrt_conv_image *color;
	const struct xrt_conv_image *depth;
	const struct xrt_conv_depth_info *depth_info;
	uint32_t view_count;
	float viewpoints_m[XRT_CONV_MAX_VIEWS][3]; //!< rect-relative metres
	float rect_width_m, rect_height_m;
	float range_near, range_far; //!< decoded values at n = 1 / n = 0
	float convergence;           //!< decoded value on the glass
	float budget;                //!< total near-far disparity of the nominal pair, fraction of width
	float rear_cap_m;            //!< ADR-040; < 0 unlimited
	const struct xrt_conv_image *target;
	uint32_t layout; //!< XRT_CONV_VIEWS_*
	bool new_content;
};

struct conv_viewsyn_ops
{
	bool (*render)(void *ud, const struct conv_viewsyn_request *req);
	void *ud;
};

/*!
 * ADR-058 late view synthesis: the S5 inputs of one composed result, kept in
 * its ring slot instead of the views (the images live beside it). A backend
 * re-runs S5 from it at the eyes of each display frame
 * (conv_core_stream_late_prepare).
 */
struct conv_late_source
{
	struct xrt_conv_depth_info depth_info;
	uint32_t view_count;
	uint32_t layout;                           //!< XRT_CONV_VIEWS_*
	float viewpoints_m[XRT_CONV_MAX_VIEWS][3]; //!< the RECORDED (conversion) viewpoints, rect-relative
	float rect_width_m, rect_height_m;
	float range_near, range_far;
	float convergence;
	float budget;
	float rear_cap_m;
};

struct u_conv_viewsyn_params;

/*!
 * The one mapping of an S5 request onto the reference view synthesis'
 * parameters (the GPU glues' and the late stage's): geometry, depth units
 * and the policy's numbers; everything else at the shipping defaults.
 */
void
conv_viewsyn_request_to_params(const struct conv_viewsyn_request *req, struct u_conv_viewsyn_params *out);

/*! GPU depth → CPU samples for S6 / S7 (the CPU memory kind needs none). */
struct conv_depth_readback_ops
{
	//! *out_samples stays valid until release(); false = nothing this frame.
	bool (*read)(void *ud,
	             const struct xrt_conv_image *depth,
	             const float **out_samples,
	             uint32_t *out_w,
	             uint32_t *out_h,
	             uint32_t *out_stride_floats);
	void (*release)(void *ud, const float *samples);
	void *ud;
};


/*
 *
 * One conversion.
 *
 */

struct conv_pipeline_frame
{
	/* In. */
	struct conv_host *host;
	uint32_t cap_id;
	uint64_t provider_stream;
	struct xrt_conv_frame_in in;
	struct xrt_conv_params params;          //!< S3 already applied
	struct xrt_conv_frame_out provider_out; //!< the provider's targets (struct_size set)
	struct xrt_conv_image views_target;     //!< composed: the final views target
	float budget;                           //!< S3 decision (S5 reads it)
	float rear_cap_m;
	bool new_content;
	//! ADR-058: build S5's request but do not render it (late view
	//! synthesis): frame->late is filled instead and VIEWS is not written.
	bool defer_s5;

	/* Out. */
	xrt_result_t provider_result;
	uint32_t blocked;                  //!< CONV_CMD_BLOCKED_* of the provider call
	struct xrt_conv_frame_out result;  //!< what S8 publishes
	struct conv_depth_samples samples; //!< S6's CPU view (for S7), until conv_pipeline_frame_release
	bool samples_from_readback;
	//! defer_s5: the S5 inputs, ready for a late synthesis.
	struct conv_late_source late;
	bool late_ready;
};

/*!
 * Run S4, S6, S5 for one frame (S3 is the caller's, S7 / S8 the stream's).
 * Blocks on the provider through @p frame->host (never call with a
 * conversion lock held). @p readback may be NULL.
 *
 * @return XRT_SUCCESS when frame->result holds a publishable result.
 */
xrt_result_t
conv_pipeline_execute(const struct conv_pipeline *p,
                      const struct conv_viewsyn_ops *viewsyn,
                      const struct conv_depth_readback_ops *readback,
                      struct conv_pipeline_frame *frame);

/*!
 * A deferred frame (late_ready) that must be woven now after all (its depth
 * is not usable later): run the S5 it skipped, into frame->views_target.
 * Clears late_ready on success.
 */
bool
conv_pipeline_render_deferred(const struct conv_pipeline *p,
                              const struct conv_viewsyn_ops *viewsyn,
                              struct conv_pipeline_frame *frame);

//! Release S6's readback samples.
void
conv_pipeline_frame_release(const struct conv_depth_readback_ops *readback, struct conv_pipeline_frame *frame);

#ifdef __cplusplus
}
#endif
