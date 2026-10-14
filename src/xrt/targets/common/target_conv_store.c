// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P4 (#2108): point a host process's model store at the
 *         registered providers' packs. Its own translation unit so that only
 *         the hosts (service, satellite) pull the store into their link.
 * @ingroup targets_common
 */

#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"

#include "conversion/conv_model_store.h"
#include "xrt/xrt_display_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
target_conv_store_configure(const struct xrt_conversion_plugin_entry *entries,
                            int count,
                            const struct target_conv_policy *policy)
{
	static char paths[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][XRT_CONV_LOADER_PATH_MAX];
	const char *ptrs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS];
	uint32_t n = 0;
	for (int i = 0; entries != NULL && i < count; i++) {
		for (uint32_t k = 0; k < entries[i].pack_count && k < XRT_CONV_LOADER_MAX_PACKS; k++) {
			if (n >= XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS) {
				break;
			}
			conv_store_pack_path(entries[i].packs[k], entries[i].binary_path, paths[n], sizeof(paths[n]));
			ptrs[n] = paths[n];
			n++;
		}
	}
	conv_store_host_configure(ptrs, n, policy == NULL || policy->allow_downloads,
	                          policy != NULL && policy->allow_research_only);
}

uint32_t
target_conv_store_registered_packs(char (*out)[512], uint32_t max)
{
	struct xrt_conversion_plugin_entry *rows = calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(*rows));
	if (rows == NULL) {
		return 0;
	}
	const int n = target_conversion_plugin_enumerate(rows, XRT_CONV_LOADER_MAX_ENTRIES);
	uint32_t count = 0;
	for (int i = 0; i < n; i++) {
		if (target_conv_is_policy_subkey(rows[i].id)) {
			continue;
		}
		for (uint32_t k = 0; k < rows[i].pack_count && k < XRT_CONV_LOADER_MAX_PACKS && count < max; k++) {
			conv_store_pack_path(rows[i].packs[k], rows[i].binary_path, out[count], 512);
			bool dup = false;
			for (uint32_t j = 0; j < count && !dup; j++) {
				dup = strcmp(out[j], out[count]) == 0;
			}
			count += dup ? 0u : 1u;
		}
	}
	free(rows);
	return count;
}

void
target_conv_store_fill_snapshot_with(struct xrt_status_snapshot *snap,
                                     const char *root,
                                     const char *source,
                                     const char *const *pack_paths,
                                     uint32_t pack_count)
{
	if (snap == NULL) {
		return;
	}
	snap->conv_pack_count = 0;
	(void)snprintf(snap->conv_model_store_root, sizeof(snap->conv_model_store_root), "%s", root != NULL ? root : "");
	(void)snprintf(snap->conv_model_store_source, sizeof(snap->conv_model_store_source), "%s",
	               source != NULL ? source : "");
	if (root == NULL || root[0] == '\0') {
		return;
	}
	struct conv_model_store_config cfg;
	conv_model_store_config_defaults(&cfg);
	(void)snprintf(cfg.root, sizeof(cfg.root), "%s", root);
	struct conv_model_store *s = NULL;
	if (!conv_model_store_create(&cfg, &s)) {
		return;
	}
	for (uint32_t i = 0; i < pack_count; i++) {
		(void)conv_model_store_add_pack_file(s, pack_paths[i]);
	}
	const uint32_t n = conv_model_store_pack_count(s);
	for (uint32_t i = 0; i < n && snap->conv_pack_count < XRT_STATUS_MAX_CONV_PACKS; i++) {
		struct conv_store_pack_summary sum;
		if (!conv_model_store_pack_summary(s, i, &sum)) {
			continue;
		}
		struct xrt_status_conv_pack *row = &snap->conv_packs[snap->conv_pack_count++];
		memset(row, 0, sizeof(*row));
		(void)snprintf(row->pack, sizeof(row->pack), "%s", sum.pack);
		(void)snprintf(row->provider, sizeof(row->provider), "%s", sum.provider);
		(void)snprintf(row->state, sizeof(row->state), "%s", sum.state);
		row->models = sum.models;
		row->models_installed = sum.models_installed;
		row->research_models = sum.research_models;
		row->bytes_total = sum.bytes_total;
		row->bytes_installed = sum.bytes_installed;
		row->download_done = sum.download_done;
		row->download_total = sum.download_total;
	}
	conv_model_store_destroy(&s);
}

void
target_conv_store_fill_snapshot(struct xrt_status_snapshot *snap)
{
	char root[512], source[64];
	if (!conv_store_root_resolve(root, sizeof(root), source, sizeof(source))) {
		root[0] = '\0';
		source[0] = '\0';
	}
	static char packs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS][512];
	const char *ptrs[XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS];
	const uint32_t n = target_conv_store_registered_packs(packs, XRT_CONV_LOADER_MAX_ENTRIES * XRT_CONV_LOADER_MAX_PACKS);
	for (uint32_t i = 0; i < n; i++) {
		ptrs[i] = packs[i];
	}
	target_conv_store_fill_snapshot_with(snap, root, source, ptrs, n);
}
