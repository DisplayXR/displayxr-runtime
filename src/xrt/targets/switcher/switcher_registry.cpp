// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  LeiaXR Runtime Switcher - Registry operations implementation
 * @author Leia Inc.
 * @ingroup targets_switcher
 */

#include "switcher_registry.h"

#include <shlwapi.h>

// Known runtime locations
static const wchar_t *SRHYDRA_MANIFEST = L"C:\\Program Files\\LeiaSR\\OpenXR\\SRHydra_win64.json";
static const wchar_t *SRHYDRA_INSTALL = L"C:\\Program Files\\LeiaSR\\OpenXR";
static const wchar_t *SRMONADO_MANIFEST = L"C:\\Program Files\\LeiaSR\\SRMonado\\SRMonado_win64.json";
static const wchar_t *SRMONADO_INSTALL = L"C:\\Program Files\\LeiaSR\\SRMonado";

// OpenXR registry key
static const wchar_t *OPENXR_KEY = L"Software\\Khronos\\OpenXR\\1";
static const wchar_t *ACTIVE_RUNTIME_VALUE = L"ActiveRuntime";

RuntimeRegistry::RuntimeRegistry()
{
	refresh();
}

void
RuntimeRegistry::refresh()
{
	m_runtimes.clear();

	// Check for known runtimes
	add_runtime_if_exists(L"SRHydra", SRHYDRA_MANIFEST, SRHYDRA_INSTALL);
	add_runtime_if_exists(L"SRMonado", SRMONADO_MANIFEST, SRMONADO_INSTALL);

	// Also check for SRMonado in the registry (in case installed elsewhere)
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LeiaSR\\SRMonado", 0, KEY_READ, &key) == ERROR_SUCCESS) {
		wchar_t install_path[MAX_PATH];
		DWORD size = sizeof(install_path);
		if (RegQueryValueExW(key, L"InstallPath", NULL, NULL, (LPBYTE)install_path, &size) == ERROR_SUCCESS) {
			std::wstring manifest = std::wstring(install_path) + L"\\SRMonado_win64.json";
			// Check if not already added
			bool found = false;
			for (const auto &rt : m_runtimes) {
				if (_wcsicmp(rt.manifest_path.c_str(), manifest.c_str()) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				add_runtime_if_exists(L"SRMonado", manifest, install_path);
			}
		}
		RegCloseKey(key);
	}

	// Update active status
	std::wstring active = get_active_runtime_path();
	for (auto &rt : m_runtimes) {
		rt.is_active = (_wcsicmp(rt.manifest_path.c_str(), active.c_str()) == 0);
	}
}

const std::vector<RuntimeInfo> &
RuntimeRegistry::get_runtimes() const
{
	return m_runtimes;
}

int
RuntimeRegistry::get_active_index() const
{
	for (size_t i = 0; i < m_runtimes.size(); ++i) {
		if (m_runtimes[i].is_active) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool
RuntimeRegistry::set_active_runtime(size_t index)
{
	if (index >= m_runtimes.size()) {
		return false;
	}

	HKEY key;
	LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, OPENXR_KEY, 0, KEY_SET_VALUE, &key);
	if (result != ERROR_SUCCESS) {
		// Try to create the key
		result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, OPENXR_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
		if (result != ERROR_SUCCESS) {
			return false;
		}
	}

	const std::wstring &manifest = m_runtimes[index].manifest_path;
	result = RegSetValueExW(key, ACTIVE_RUNTIME_VALUE, 0, REG_SZ, (const BYTE *)manifest.c_str(),
	                        (DWORD)((manifest.length() + 1) * sizeof(wchar_t)));

	RegCloseKey(key);

	if (result == ERROR_SUCCESS) {
		// Update active status
		for (auto &rt : m_runtimes) {
			rt.is_active = (&rt == &m_runtimes[index]);
		}
		return true;
	}

	return false;
}

std::wstring
RuntimeRegistry::get_active_runtime_path() const
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, OPENXR_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return L"";
	}

	wchar_t value[MAX_PATH];
	DWORD size = sizeof(value);
	DWORD type;

	LONG result = RegQueryValueExW(key, ACTIVE_RUNTIME_VALUE, NULL, &type, (LPBYTE)value, &size);
	RegCloseKey(key);

	if (result == ERROR_SUCCESS && type == REG_SZ) {
		return std::wstring(value);
	}

	return L"";
}

bool
RuntimeRegistry::check_manifest_exists(const std::wstring &path) const
{
	return PathFileExistsW(path.c_str()) != FALSE;
}

void
RuntimeRegistry::add_runtime_if_exists(const std::wstring &name,
                                       const std::wstring &manifest_path,
                                       const std::wstring &install_path)
{
	if (check_manifest_exists(manifest_path)) {
		RuntimeInfo info;
		info.name = name;
		info.manifest_path = manifest_path;
		info.install_path = install_path;
		info.is_active = false;
		m_runtimes.push_back(info);
	}
}
