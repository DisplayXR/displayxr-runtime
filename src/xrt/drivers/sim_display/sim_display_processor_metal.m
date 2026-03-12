// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation Metal display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements SBS, anaglyph, and alpha-blend atlas output modes using MSL
 * shaders compiled at runtime. All 3 pipelines are pre-compiled at init
 * for instant runtime switching via 1/2/3 keys.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#import <Metal/Metal.h>

#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m_metal, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)


/*
 *
 * Embedded MSL shader source.
 *
 */

static NSString *const shader_source = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct VertexOut {\n"
    "    float4 position [[position]];\n"
    "    float2 texCoord;\n"
    "};\n"
    "\n"
    "struct TileParams {\n"
    "    float tile_cols_inv;\n"
    "    float tile_rows_inv;\n"
    "    float tile_cols;\n"
    "    float tile_rows;\n"
    "};\n"
    "\n"
    "vertex VertexOut fullscreen_vertex(uint vid [[vertex_id]]) {\n"
    "    VertexOut out;\n"
    "    out.texCoord = float2((vid << 1) & 2, vid & 2);\n"
    "    out.position = float4(out.texCoord * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "// SBS with center crop: each eye rendered at full-display FOV,\n"
    "// crop center 50% horizontally to match half-display SBS layout.\n"
    "// Tile-aware: uses tile_cols/tile_rows to locate each view in the atlas.\n"
    "fragment float4 sbs_fragment(VertexOut in [[stage_in]],\n"
    "                             texture2d<float> tex [[texture(0)]],\n"
    "                             sampler smp [[sampler(0)]],\n"
    "                             constant TileParams &tp [[buffer(0)]]) {\n"
    "    float x = in.texCoord.x;\n"
    "    // View 0 (left): col=0, row=0\n"
    "    float2 left_uv = float2(in.texCoord.x * tp.tile_cols_inv,\n"
    "                            in.texCoord.y * tp.tile_rows_inv);\n"
    "    // View 1 (right): col=1%tile_cols, row=1/tile_cols\n"
    "    float col1 = float(1 % uint(tp.tile_cols));\n"
    "    float row1 = float(1 / uint(tp.tile_cols));\n"
    "    float2 right_uv = float2((in.texCoord.x + col1) * tp.tile_cols_inv,\n"
    "                             (in.texCoord.y + row1) * tp.tile_rows_inv);\n"
    "    float src_u;\n"
    "    float src_v;\n"
    "    if (x < 0.5) {\n"
    "        float eye_u = x / 0.5;\n"
    "        // center crop: take middle 50% of the left view\n"
    "        src_u = (0.25 + eye_u * 0.5) * tp.tile_cols_inv;\n"
    "        src_v = in.texCoord.y * tp.tile_rows_inv;\n"
    "    } else {\n"
    "        float eye_u = (x - 0.5) / 0.5;\n"
    "        src_u = (col1 + 0.25 + eye_u * 0.5) * tp.tile_cols_inv;\n"
    "        src_v = (in.texCoord.y + row1) * tp.tile_rows_inv;\n"
    "    }\n"
    "    return tex.sample(smp, float2(src_u, src_v));\n"
    "}\n"
    "\n"
    "// Anaglyph: red from left eye, cyan from right eye\n"
    "fragment float4 anaglyph_fragment(VertexOut in [[stage_in]],\n"
    "                                  texture2d<float> tex [[texture(0)]],\n"
    "                                  sampler smp [[sampler(0)]],\n"
    "                                  constant TileParams &tp [[buffer(0)]]) {\n"
    "    float2 left_uv  = float2(in.texCoord.x * tp.tile_cols_inv,\n"
    "                             in.texCoord.y * tp.tile_rows_inv);\n"
    "    float col1 = float(1 % uint(tp.tile_cols));\n"
    "    float row1 = float(1 / uint(tp.tile_cols));\n"
    "    float2 right_uv = float2((in.texCoord.x + col1) * tp.tile_cols_inv,\n"
    "                             (in.texCoord.y + row1) * tp.tile_rows_inv);\n"
    "    float4 left  = tex.sample(smp, left_uv);\n"
    "    float4 right = tex.sample(smp, right_uv);\n"
    "    return float4(left.r, right.g, right.b, 1.0);\n"
    "}\n"
    "\n"
    "// Blend: 50/50 mix\n"
    "fragment float4 blend_fragment(VertexOut in [[stage_in]],\n"
    "                               texture2d<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]],\n"
    "                               constant TileParams &tp [[buffer(0)]]) {\n"
    "    float2 left_uv  = float2(in.texCoord.x * tp.tile_cols_inv,\n"
    "                             in.texCoord.y * tp.tile_rows_inv);\n"
    "    float col1 = float(1 % uint(tp.tile_cols));\n"
    "    float row1 = float(1 / uint(tp.tile_cols));\n"
    "    float2 right_uv = float2((in.texCoord.x + col1) * tp.tile_cols_inv,\n"
    "                             (in.texCoord.y + row1) * tp.tile_rows_inv);\n"
    "    return mix(tex.sample(smp, left_uv), tex.sample(smp, right_uv), 0.5);\n"
    "}\n";


/*!
 * Implementation struct for the Metal simulation display processor.
 */
struct sim_display_processor_metal
{
	struct xrt_display_processor_metal base;
	id<MTLDevice> device;
	id<MTLRenderPipelineState> pipelines[3]; //!< One per output mode (SBS, anaglyph, blend)
	id<MTLSamplerState> sampler;

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
};

static inline struct sim_display_processor_metal *
sim_dp_metal(struct xrt_display_processor_metal *xdp)
{
	return (struct sim_display_processor_metal *)xdp;
}


/*
 *
 * process_atlas: fullscreen quad with runtime-switchable fragment shader.
 *
 */

/*!
 * Tile parameter constants passed to Metal fragment shaders.
 * Must match the TileParams struct in the MSL shader source.
 */
struct tile_params
{
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

static void
sim_dp_metal_process_atlas(struct xrt_display_processor_metal *xdp,
                            void *command_buffer,
                            void *atlas_texture,
                            uint32_t view_width,
                            uint32_t view_height,
                            uint32_t tile_columns,
                            uint32_t tile_rows,
                            uint32_t format,
                            void *target_texture,
                            uint32_t target_width,
                            uint32_t target_height)
{
	struct sim_display_processor_metal *sdp = sim_dp_metal(xdp);
	id<MTLCommandBuffer> cmd_buf = (__bridge id<MTLCommandBuffer>)command_buffer;
	id<MTLTexture> atlas_tex = (__bridge id<MTLTexture>)atlas_texture;
	id<MTLTexture> target_tex = (__bridge id<MTLTexture>)target_texture;

	if (cmd_buf == nil || atlas_tex == nil || target_tex == nil) {
		return;
	}

	// Read the current mode (may change at runtime via 1/2/3 keys)
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	id<MTLRenderPipelineState> active_pipeline = sdp->pipelines[mode];
	if (active_pipeline == nil) {
		return;
	}

	// Prepare tile layout constants for the fragment shader.
	// Use view_width/atlas_width (not 1/tile_columns) so UV mapping is
	// correct when the atlas texture is larger than the tiled region.
	uint32_t atlas_w = (uint32_t)atlas_tex.width;
	uint32_t atlas_h = (uint32_t)atlas_tex.height;
	struct tile_params tp = {
	    .tile_cols_inv = (atlas_w > 0) ? ((float)view_width / (float)atlas_w) : 0.5f,
	    .tile_rows_inv = (atlas_h > 0) ? ((float)view_height / (float)atlas_h) : 1.0f,
	    .tile_cols = (float)tile_columns,
	    .tile_rows = (float)tile_rows,
	};

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target_tex;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

	id<MTLRenderCommandEncoder> encoder = [cmd_buf renderCommandEncoderWithDescriptor:pass];

	[encoder setRenderPipelineState:active_pipeline];
	[encoder setFragmentTexture:atlas_tex atIndex:0];
	[encoder setFragmentSamplerState:sdp->sampler atIndex:0];
	[encoder setFragmentBytes:&tp length:sizeof(tp) atIndex:0];

	MTLViewport vp = {
	    .originX = 0,
	    .originY = 0,
	    .width = (double)target_width,
	    .height = (double)target_height,
	    .znear = 0.0,
	    .zfar = 1.0,
	};
	[encoder setViewport:vp];

	// Draw fullscreen triangle (3 vertices, no VBO)
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
}


static bool
sim_dp_metal_get_predicted_eye_positions(struct xrt_display_processor_metal *xdp,
                                         struct xrt_eye_pair *out_eye_pair)
{
	struct sim_display_processor_metal *sdp = sim_dp_metal(xdp);
	float half_ipd = sdp->ipd_m / 2.0f;

	out_eye_pair->left = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
	out_eye_pair->right = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
	out_eye_pair->timestamp_ns = os_monotonic_get_ns();
	out_eye_pair->valid = true;
	out_eye_pair->is_tracking = false; // Nominal, not real tracking
	return true;
}

static void
sim_dp_metal_destroy(struct xrt_display_processor_metal *xdp)
{
	struct sim_display_processor_metal *sdp = sim_dp_metal(xdp);

	for (int i = 0; i < 3; i++) {
		sdp->pipelines[i] = nil;
	}
	sdp->sampler = nil;
	sdp->device = nil;

	free(sdp);
}


/*
 *
 * Pipeline creation.
 *
 */

static bool
create_pipelines(struct sim_display_processor_metal *sdp)
{
	NSError *error = nil;

	id<MTLLibrary> library = [sdp->device newLibraryWithSource:shader_source
	                                                   options:nil
	                                                     error:&error];
	if (library == nil) {
		U_LOG_E("sim_display Metal: failed to compile shaders: %s",
		        error.localizedDescription.UTF8String);
		return false;
	}

	id<MTLFunction> vertex_fn = [library newFunctionWithName:@"fullscreen_vertex"];
	if (vertex_fn == nil) {
		U_LOG_E("sim_display Metal: vertex function not found");
		return false;
	}

	NSString *frag_names[3] = {@"sbs_fragment", @"anaglyph_fragment", @"blend_fragment"};
	const char *mode_names[3] = {"SBS", "Anaglyph", "Blend"};

	for (int i = 0; i < 3; i++) {
		id<MTLFunction> frag_fn = [library newFunctionWithName:frag_names[i]];
		if (frag_fn == nil) {
			U_LOG_E("sim_display Metal: %s fragment function not found", mode_names[i]);
			return false;
		}

		MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction = vertex_fn;
		desc.fragmentFunction = frag_fn;
		desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		sdp->pipelines[i] = [sdp->device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (sdp->pipelines[i] == nil) {
			U_LOG_E("sim_display Metal: failed to create %s pipeline: %s",
			        mode_names[i], error.localizedDescription.UTF8String);
			return false;
		}
	}

	// Create sampler (linear, clamp to edge)
	MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
	sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
	sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;

	sdp->sampler = [sdp->device newSamplerStateWithDescriptor:sampler_desc];
	if (sdp->sampler == nil) {
		U_LOG_E("sim_display Metal: failed to create sampler");
		return false;
	}

	return true;
}


/*
 *
 * Exported creation function.
 *
 */

xrt_result_t
sim_display_processor_metal_create(enum sim_display_output_mode mode,
                                   void *metal_device,
                                   struct xrt_display_processor_metal **out_xdp)
{
	if (out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (metal_device == NULL) {
		U_LOG_E("sim_display Metal: device required for display processor");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor_metal *sdp = calloc(1, sizeof(*sdp));
	if (sdp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	sdp->base.destroy = sim_dp_metal_destroy;
	sdp->base.process_atlas = sim_dp_metal_process_atlas;
	sdp->base.get_predicted_eye_positions = sim_dp_metal_get_predicted_eye_positions;

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m_metal();

	sdp->device = (__bridge id<MTLDevice>)metal_device;

	if (!create_pipelines(sdp)) {
		U_LOG_E("sim_display Metal: failed to create pipeline resources");
		sim_dp_metal_destroy(&sdp->base);
		return XRT_ERROR_VULKAN;
	}

	// Set the initial output mode (atomic global read by process_atlas each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display Metal processor (all 3 pipelines), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS       ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH   ? "Anaglyph" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}


/*
 *
 * Factory function — matches xrt_dp_factory_metal_fn_t signature.
 *
 */

xrt_result_t
sim_display_dp_factory_metal(void *metal_device,
                             void *command_queue,
                             void *window_handle,
                             struct xrt_display_processor_metal **out_xdp)
{
	(void)command_queue;
	(void)window_handle;

	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_metal_create(mode, metal_device, out_xdp);
}
