// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion registry: the provider table, the capability table built
 *         from describe_cap(), descriptor validation, Supersedes, and the
 *         persisted describe cache (ADR-053 decisions 1, 2, 6).
 *
 * Pure C11: no threads, no registry / file I/O, no logging. The loader hands
 * one conv_provider_desc per discovered provider; the registry calls only the
 * [C] slots get_provider_status / describe_cap / get_generation (answer from
 * memory, no model load). The describe cache is a byte buffer; the 20-line
 * file wrapper belongs to the caller.
 *
 * Spec: docs/specs/runtime/conversion-provider-discovery.md §3.1 (Supersedes,
 * Hosting), §4.1 (loading policy, persisted describe cache), §4.5 (Supersedes
 * of the legacy adapter), §7.2 (descriptor validation rules); roadmap
 * docs/roadmap/conversion-providers.md §2.3.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include "conv_host_iface.h"
#include "conv_readiness.h"
#include "conv_select.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONV_REGISTRY_MAX_PROVIDERS 16u
#define CONV_REGISTRY_MAX_ROWS 128u
#define CONV_ID_MAX 64u
#define CONV_BINARY_PATH_MAX 512u
#define CONV_PROBE_ORDER_DEFAULT 100u //!< the loader applies it when ProbeOrder is missing

/*! @name Hosting (Hosting registry value) @{ */
#define CONV_HOSTING_AUTO 0u
#define CONV_HOSTING_IN_SERVICE 1u
#define CONV_HOSTING_SATELLITE 2u
/*! @} */

/*! What the loader knows about one provider. Strings are copied. */
struct conv_provider_desc
{
	const char *id;         //!< registry / manifest <id>; required, < CONV_ID_MAX
	uint32_t probe_order;   //!< ProbeOrder (lower wins)
	uint32_t hosting;       //!< CONV_HOSTING_*
	const char *supersedes; //!< Supersedes; NULL / "" = none
	bool enabled;           //!< Enabled != 0
	uint32_t flags;         //!< CONV_PROV_* (FALLBACK is also read from get_provider_status)

	/* Describe-cache key (spec §4.1: binary path, size, mtime). */
	const char *binary_path; //!< may be NULL (built-in rows)
	uint64_t binary_size;
	uint64_t binary_mtime;

	/* Live provider, when loaded in this process. NULL = not loaded (yet): the
	 * rows can still come from the describe cache. */
	const struct xrt_conversion_plugin_iface *iface;
	struct xrt_conversion_plugin_instance *inst;

	/* A pre-built host (the satellite proxy, conv_host_proxy.c) in place of
	 * iface / inst: the provider is NOT loaded in this process; describe goes
	 * through the host's [C] calls. conv_core_add_provider takes ownership. */
	struct conv_host *host;
};

/*! @name Descriptor validation (selftest rules, spec §7.2) @{ */
enum conv_cap_error
{
	CONV_CAP_OK = 0,
	CONV_CAP_ERR_CAP_ID_ZERO,      //!< cap_id must be non-zero
	CONV_CAP_ERR_TASK,             //!< unknown task (NONE, or ≥ XRT_CONV_TASK_RESERVED_FIRST)
	CONV_CAP_ERR_LANES,            //!< lanes == 0, or an unknown lane bit
	CONV_CAP_ERR_VIEWS,            //!< not min_views ≤ max_views ≤ XRT_CONV_MAX_VIEWS
	CONV_CAP_ERR_VIEWPOINTS,       //!< VIEWS_* without EXPLICIT_VIEWPOINTS
	CONV_CAP_ERR_LICENCE,          //!< unknown licence class
	CONV_CAP_ERR_DUPLICATE_CAP_ID, //!< cap_id already used by this provider
	CONV_CAP_ERR_STATE,            //!< get_state answered an out-of-range state
	CONV_CAP_ERR_COUNT,            //!< more than XRT_CONV_MAX_CAPABILITIES
	CONV_CAP_ERR_DESCRIBE_FAILED,  //!< describe_cap failed for an index < count
};
/*! @} */

struct conv_provider
{
	char id[CONV_ID_MAX];
	uint32_t probe_order;
	uint32_t hosting;
	char supersedes[CONV_ID_MAX];
	bool enabled;
	uint32_t flags; //!< CONV_PROV_*

	char binary_path[CONV_BINARY_PATH_MAX];
	uint64_t binary_size;
	uint64_t binary_mtime;

	const struct xrt_conversion_plugin_iface *iface;
	struct xrt_conversion_plugin_instance *inst;
	struct conv_host *host; //!< describe source when iface is NULL (satellite); not owned here

	//! Negotiated at ABI 1, probed OK, platform not ABSENT / INCOMPATIBLE.
	//! Default true; the loader clears it (conv_registry_set_usable) on a
	//! reject, the registry on a PLATFORM_ABSENT / INCOMPATIBLE status.
	bool usable;
	uint32_t platform_state; //!< xrt_conv_provider_status::state
	//! #2279: the satellite host failed terminally (CONV_HOST_EV_FAILED /
	//! CRASH_LOOP): its XRT_CONV_FAIL_* reason, 0 = not failed. Set and cleared
	//! by the core (a configuration change or the host's return clears it).
	//! A failed provider counts as settled for discovery, described or not.
	uint32_t host_failed_reason;

	bool described;      //!< rows exist (live or from the cache)
	bool from_cache;     //!< rows came from the describe cache, not a live describe
	uint64_t generation; //!< get_generation() at the last live describe

	uint32_t bad_descriptors; //!< rows rejected by the last describe
	uint32_t first_error;     //!< conv_cap_error of the first rejection
};

/*! One capability row: provider × capability, with its readiness machine. */
struct conv_cap_row
{
	uint32_t provider; //!< index into conv_registry::providers
	struct xrt_conv_capability cap;
	struct conv_readiness rd;
};

struct conv_registry
{
	struct conv_provider providers[CONV_REGISTRY_MAX_PROVIDERS];
	uint32_t provider_count;
	struct conv_cap_row rows[CONV_REGISTRY_MAX_ROWS];
	uint32_t row_count;
};

void
conv_registry_init(struct conv_registry *reg);

/*! Add (or, same id, replace the description of) a provider. Returns its index, or -1. */
int32_t
conv_registry_add(struct conv_registry *reg, const struct conv_provider_desc *desc);

int32_t
conv_registry_find_provider(const struct conv_registry *reg, const char *id);

void
conv_registry_set_usable(struct conv_registry *reg, uint32_t provider, bool usable);

/*!
 * Describe one live provider: provider status, then walk describe_cap() once
 * per index between two get_generation() reads (re-walk, at most 3 times, if
 * the generation moved). Every row is validated; a malformed row is dropped
 * and counted (bad_descriptors / first_error). A cap_id that survives a
 * re-describe keeps its readiness; a new one starts INSTALLED; a vanished one
 * is removed.
 *
 * @return number of rows accepted, or -1 when the provider has no live iface
 *         or describe_cap could not even report its count.
 */
int32_t
conv_registry_describe(struct conv_registry *reg, uint32_t provider, uint64_t now_ns);

/*! Validate one descriptor against the selftest rules (all but uniqueness). */
uint32_t
conv_cap_validate(const struct xrt_conv_capability *cap);

/*! Validate a get_state answer (in-range state). */
uint32_t
conv_cap_validate_state(const struct xrt_conv_state *st);

/*! Is @p provider hidden by another registered provider's Supersedes? */
bool
conv_registry_is_hidden(const struct conv_registry *reg, uint32_t provider);

/*! Every registered provider is described (live or cached) or terminal (disabled / unusable / host failed). */
bool
conv_registry_discovery_settled(const struct conv_registry *reg);

/*! Row index of (provider, cap_id), or -1. */
int32_t
conv_registry_find_row(const struct conv_registry *reg, uint32_t provider, uint32_t cap_id);

/*!
 * Build the selection candidates for @p task. Pointers in the output refer to
 * registry storage (valid until the registry changes).
 *
 * @param include_composed  also synthesise VIEWS_MONO rows from every
 *                          stream-lane DEPTH_MONO / DEPTH_VIDEO row (depth +
 *                          the reference view-synthesis stage: 2..8 views,
 *                          explicit viewpoints honoured by the stage)
 * @param out_cap_row       optional, @p max entries: registry row behind each candidate
 * @return candidates written
 */
uint32_t
conv_registry_build_candidates(const struct conv_registry *reg,
                               uint32_t task,
                               bool include_composed,
                               struct conv_select_row *out,
                               uint32_t *out_cap_row,
                               uint32_t max);

/*
 *
 * Persisted describe cache.
 *
 */

#define CONV_CACHE_VERSION 1u

/*! @name conv_registry_cache_load() errors (negative) @{ */
#define CONV_CACHE_ERR_SHORT (-1)    //!< truncated
#define CONV_CACHE_ERR_MAGIC (-2)    //!< not a describe cache
#define CONV_CACHE_ERR_VERSION (-3)  //!< other cache version
#define CONV_CACHE_ERR_ENDIAN (-4)   //!< written on another byte order
#define CONV_CACHE_ERR_CHECKSUM (-5) //!< corrupted
#define CONV_CACHE_ERR_FORMAT (-6)   //!< inconsistent counts / sizes
/*! @} */

/*!
 * Serialise every described provider's rows. Call with buf == NULL to get the
 * size. Returns bytes needed; when @p buf_size is smaller, nothing is written.
 */
size_t
conv_registry_cache_save(const struct conv_registry *reg, uint8_t *buf, size_t buf_size);

/*!
 * Restore rows for every registered, not-yet-described provider whose id AND
 * (binary path, size, mtime) match a cache record. Restored rows are
 * re-validated and start INSTALLED (from_cache = true), so a cold boot can
 * answer and select before any provider is loaded. A live describe later
 * replaces them.
 *
 * @return providers restored (≥ 0), or a CONV_CACHE_ERR_* value.
 */
int32_t
conv_registry_cache_load(struct conv_registry *reg, const uint8_t *buf, size_t len, uint64_t now_ns);

#ifdef __cplusplus
}
#endif
