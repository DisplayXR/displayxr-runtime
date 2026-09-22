// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1663 — which source format the bridge may ingest, as a pure decision.
 * @ingroup comp_xbridge
 *
 * The submit guard's LOGGING (throttle, counter, the line that names both
 * formats) belongs to the bridge and stays there. The DECISION does not: it is a
 * three-way property of two DXGI enums, it is the whole of #1178 and #1663, and
 * the one case that is subtle — copy-legal but colour-wrong — is invisible in a
 * hardware run and looks like an over-strict guard to the next reader. So it
 * lives here, named, and is covered by `tests/tests_comp_xbridge_plane_policy.cpp`,
 * the same split `comp_xbridge_plane_policy.h` was extracted for.
 *
 * Only `dxgiformat.h` is needed — the enum, no device, no library.
 */

#pragma once

#include <dxgiformat.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Why a source format was accepted or refused. @see xb_source_format_compatible
enum xb_source_format_verdict
{
	//! Accepted: the source IS the chain's format.
	XB_SRC_FMT_EXACT,
	/*!
	 * Accepted: the source is the TYPELESS member of the chain's own family.
	 * A plain byte move, and a typeless resource carries no colour
	 * interpretation of its own — the chain's format supplies it, which is the
	 * whole reason a resource is allocated typeless. This is the per-client
	 * atlas (allocated TYPELESS so UNORM and UNORM_SRGB views can share it)
	 * reaching the bridge unwrapped on a display-filling submission (#1663).
	 */
	XB_SRC_FMT_TYPELESS_TO_CONCRETE,
	/*!
	 * Refused: two CONCRETE formats in one typeless family — `UNORM` against
	 * `UNORM_SRGB`. The copy is perfectly legal and moves the right bits; what
	 * changes is what those bits MEAN to the next sampler, which is the
	 * #1589 / #1610 colour trap. Accepting the family wholesale would trade a
	 * loud refusal for a silent colour error, so this stays refused.
	 *
	 * Also the verdict for the mirror case — a concrete source into a TYPELESS
	 * chain — which defers the encoding question to whatever later views that
	 * chain rather than answering it. The bridge's chains are concrete in
	 * practice (see `XB_FORMAT_DEFAULT`), so this is the conservative posture
	 * on a case that does not arise, not a considered allowance.
	 */
	XB_SRC_FMT_REFUSED_SRGB_SIBLING,
	/*!
	 * Refused: different typeless families — `B8G8R8A8_*` against
	 * `R8G8B8A8_*`. `CopySubresourceRegion` / `CopyTextureRegion` do not fail
	 * this, do not warn and set no HRESULT: Windows DROPS the copy and the
	 * destination keeps what it held, which is how #1178 reached a maintainer's
	 * eyeball with every counter reading healthy.
	 */
	XB_SRC_FMT_REFUSED_CROSS_FAMILY,
	/*!
	 * Refused: at least one side is a format this unit cannot place in a family,
	 * so the rule cannot be stated for it. An unstatable rule is not a licence —
	 * same posture as the chain refusing to size a heap for a format whose
	 * bytes-per-pixel it cannot state.
	 */
	XB_SRC_FMT_REFUSED_UNKNOWN,
};

/*!
 * The TYPELESS member of @p f's family, or `DXGI_FORMAT_UNKNOWN` for a format
 * this unit does not know.
 *
 * The family is the unit a D3D copy is legal across, but NOT the unit the guard
 * accepts — see @ref xb_source_format_compatible. The mapping is CLOSED: every
 * format named here maps into the set of family representatives, and each
 * representative maps to itself, so `family(family(f)) == family(f)`.
 *
 * The inputs are the formats the bridge's own `xb_fmt_name` can print, so the
 * guard can never reason about something its log cannot name.
 */
static inline DXGI_FORMAT
xb_typeless_family(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

/*!
 * May a source in @p src_fmt be ingested into a bridge chain built in
 * @p chain_fmt, and on what grounds?
 *
 * TWO formats are accepted and no more: the chain's own, and the TYPELESS member
 * of the chain's family. Everything else is refused, INCLUDING the rest of that
 * family — see @ref XB_SRC_FMT_REFUSED_SRGB_SIBLING, the case that is copy-legal
 * and colour-wrong and is therefore the reason this predicate is a tested unit
 * rather than a comment.
 *
 * @return the verdict; @ref xb_source_format_accepted turns it into a bool.
 */
static inline enum xb_source_format_verdict
xb_source_format_compatible(DXGI_FORMAT chain_fmt, DXGI_FORMAT src_fmt)
{
	const DXGI_FORMAT chain_family = xb_typeless_family(chain_fmt);
	const DXGI_FORMAT src_family = xb_typeless_family(src_fmt);

	// Unknown first: an exact match on two formats this unit cannot place is
	// still a rule it cannot state, and a chain it cannot size a heap for.
	if (chain_family == DXGI_FORMAT_UNKNOWN || src_family == DXGI_FORMAT_UNKNOWN) {
		return XB_SRC_FMT_REFUSED_UNKNOWN;
	}
	if (src_fmt == chain_fmt) {
		return XB_SRC_FMT_EXACT;
	}
	if (src_family != chain_family) {
		return XB_SRC_FMT_REFUSED_CROSS_FAMILY;
	}
	// Same family, different format. Exactly one direction is safe.
	if (src_fmt == chain_family) {
		return XB_SRC_FMT_TYPELESS_TO_CONCRETE;
	}
	return XB_SRC_FMT_REFUSED_SRGB_SIBLING;
}

//! True for the two accepted verdicts. @see xb_source_format_compatible
static inline bool
xb_source_format_accepted(enum xb_source_format_verdict v)
{
	return v == XB_SRC_FMT_EXACT || v == XB_SRC_FMT_TYPELESS_TO_CONCRETE;
}

#ifdef __cplusplus
}
#endif
