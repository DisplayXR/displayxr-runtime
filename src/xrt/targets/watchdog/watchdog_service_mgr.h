// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - Service manager header
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#pragma once

#include <windows.h>
#include <string>

/*!
 * Manages the lifecycle of monado-service.exe.
 *
 * Handles launching, monitoring, and stopping the service process.
 */
class ServiceManager
{
public:
	/*!
	 * Constructor.
	 * @param service_path Full path to monado-service.exe
	 */
	explicit ServiceManager(const std::wstring &service_path);

	/*!
	 * Destructor. Stops the service if running.
	 */
	~ServiceManager();

	/*!
	 * Start the service process.
	 * @return true if service started successfully
	 */
	bool
	start_service();

	/*!
	 * Stop the service process.
	 */
	void
	stop_service();

	/*!
	 * Check if the service process is currently running.
	 * @return true if service is running
	 */
	bool
	is_running() const;

	/*!
	 * Get the process ID of the running service.
	 * @return Process ID, or 0 if not running
	 */
	DWORD
	get_pid() const;

private:
	std::wstring m_service_path;
	HANDLE m_process_handle = NULL;
	DWORD m_process_id = 0;
};
