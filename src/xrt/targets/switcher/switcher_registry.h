// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  LeiaXR Runtime Switcher - Registry operations header
 * @author Leia Inc.
 * @ingroup targets_switcher
 */

#pragma once

#include <windows.h>
#include <string>
#include <vector>

/*!
 * Information about an installed OpenXR runtime.
 */
struct RuntimeInfo
{
	std::wstring name;          //!< Display name (e.g., "SRMonado", "SRHydra")
	std::wstring manifest_path; //!< Full path to the manifest JSON
	std::wstring install_path;  //!< Installation directory
	bool is_active;             //!< Currently set as active runtime
};

/*!
 * Handles registry operations for runtime detection and switching.
 */
class RuntimeRegistry
{
public:
	/*!
	 * Constructor. Scans for installed runtimes.
	 */
	RuntimeRegistry();

	/*!
	 * Rescan for installed runtimes.
	 */
	void
	refresh();

	/*!
	 * Get the list of detected runtimes.
	 * @return Vector of RuntimeInfo structures
	 */
	const std::vector<RuntimeInfo> &
	get_runtimes() const;

	/*!
	 * Get the index of the currently active runtime.
	 * @return Index into get_runtimes(), or -1 if none active
	 */
	int
	get_active_index() const;

	/*!
	 * Set a runtime as the active OpenXR runtime.
	 * @param index Index into get_runtimes()
	 * @return true if successful
	 */
	bool
	set_active_runtime(size_t index);

	/*!
	 * Get the current active runtime path from registry.
	 * @return Path to the active runtime manifest, or empty if none set
	 */
	std::wstring
	get_active_runtime_path() const;

private:
	/*!
	 * Check if a manifest file exists at the given path.
	 */
	bool
	check_manifest_exists(const std::wstring &path) const;

	/*!
	 * Add a runtime if its manifest exists.
	 */
	void
	add_runtime_if_exists(const std::wstring &name,
	                      const std::wstring &manifest_path,
	                      const std::wstring &install_path);

	std::vector<RuntimeInfo> m_runtimes;
};
