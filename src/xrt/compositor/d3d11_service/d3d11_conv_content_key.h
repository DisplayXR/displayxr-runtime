// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The cheap GPU content check behind still-image dedupe (#2030): a
 *         key for the picture in a stream's input slot, so an unchanged PHOTO
 *         input is not converted again (conv_frame_submit::content_key).
 *
 * Per sample, on a PRIVATE device and context of its own (never the
 * service's shared immediate context): the slot image (a shared NT-handle
 * keyed-mutex texture, opened once) is copied into a mip chain, box-filtered
 * down with GenerateMips to its level of at most 16 x 16 texels, and that
 * level is copied to a staging texture. Readback is asynchronous: a sample
 * returns the key of the PREVIOUS sample of the same stream when its staging
 * copy has completed (Map with DO_NOT_WAIT), else 0 ("unknown": the core
 * converts). So a changed picture converts on the submit after the change;
 * the frame of the change itself may republish the previous result once. A
 * stream's first sample waits for its own readback (once), so the second
 * identical submit already dedupes. Nothing full-size is ever read back.
 *
 * The block is an average: a change too small to move any 16 x 16 block
 * average by one 8-bit step is not seen. Lift spec 4 (ADR-053 P3) adds a
 * client-supplied frame identity on the wire, which supersedes this check
 * where the client sends one; there is no wire field for it today.
 *
 * Thread-safe (one internal mutex; called from the producer threads).
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <stdint.h>

struct d3d11_conv_content_check;

/*!
 * Create the check on its own device on @p adapter (the adapter of the
 * device that owns the slot images; NULL = default; a WARP adapter works).
 * @return nullptr on failure (logged once).
 */
struct d3d11_conv_content_check *
d3d11_conv_content_check_create(IDXGIAdapter *adapter);

void
d3d11_conv_content_check_destroy(struct d3d11_conv_content_check **chk_ptr);

/*!
 * Sample the picture in a slot image of stream @p stream: @p share is the
 * image's shared NT handle (borrowed; opened once per @p import_key), its
 * keyed mutex is taken with key 0 for at most 4 ms.
 *
 * @return the content key of this stream's previous sample (its first
 *         sample: of this one), 0 when unknown (readback not ready, keyed
 *         mutex busy, unsupported format). Never 0 otherwise.
 */
uint64_t
d3d11_conv_content_check_sample(struct d3d11_conv_content_check *chk,
                                uint64_t stream,
                                HANDLE share,
                                uint64_t import_key);

//! The slot image went away (its key may be reused for another allocation).
void
d3d11_conv_content_check_forget_image(struct d3d11_conv_content_check *chk, uint64_t import_key);

//! The stream went away.
void
d3d11_conv_content_check_forget_stream(struct d3d11_conv_content_check *chk, uint64_t stream);
