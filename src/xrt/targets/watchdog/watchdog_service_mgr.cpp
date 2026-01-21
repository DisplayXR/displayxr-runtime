// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - Service manager implementation
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#include "watchdog_service_mgr.h"

#include <shlwapi.h>

ServiceManager::ServiceManager(const std::wstring &service_path) : m_service_path(service_path) {}

ServiceManager::~ServiceManager()
{
	stop_service();
}

bool
ServiceManager::start_service()
{
	// Don't start if already running
	if (is_running()) {
		return true;
	}

	// Check if service executable exists
	if (!PathFileExistsW(m_service_path.c_str())) {
		return false;
	}

	// Get the working directory (same as service executable)
	std::wstring working_dir = m_service_path;
	PathRemoveFileSpecW(&working_dir[0]);

	// Prepare process creation
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE; // Run hidden

	PROCESS_INFORMATION pi = {};

	// Create a mutable copy of the path for CreateProcessW
	std::wstring cmd_line = L"\"" + m_service_path + L"\"";

	// Create the process
	BOOL success = CreateProcessW(NULL,                          // Application name (use command line)
	                              &cmd_line[0],                  // Command line
	                              NULL,                          // Process security attributes
	                              NULL,                          // Thread security attributes
	                              FALSE,                         // Inherit handles
	                              CREATE_NEW_PROCESS_GROUP,      // Creation flags
	                              NULL,                          // Environment
	                              working_dir.empty() ? NULL : working_dir.c_str(), // Working directory
	                              &si,                                               // Startup info
	                              &pi                                                // Process info
	);

	if (!success) {
		return false;
	}

	// Store process information
	m_process_handle = pi.hProcess;
	m_process_id = pi.dwProcessId;

	// Close the thread handle (not needed)
	CloseHandle(pi.hThread);

	return true;
}

void
ServiceManager::stop_service()
{
	if (m_process_handle != NULL) {
		// Try graceful termination first by sending Ctrl+Break
		// to the process group
		GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_process_id);

		// Wait briefly for graceful shutdown
		if (WaitForSingleObject(m_process_handle, 1000) == WAIT_TIMEOUT) {
			// Force terminate if still running
			TerminateProcess(m_process_handle, 0);
			WaitForSingleObject(m_process_handle, 1000);
		}

		CloseHandle(m_process_handle);
		m_process_handle = NULL;
		m_process_id = 0;
	}
}

bool
ServiceManager::is_running() const
{
	if (m_process_handle == NULL) {
		return false;
	}

	// Check if process is still running
	DWORD exit_code;
	if (GetExitCodeProcess(m_process_handle, &exit_code)) {
		return exit_code == STILL_ACTIVE;
	}

	return false;
}

DWORD
ServiceManager::get_pid() const
{
	return is_running() ? m_process_id : 0;
}
