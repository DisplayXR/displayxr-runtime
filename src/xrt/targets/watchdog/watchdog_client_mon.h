// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - Client monitor header
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include <atomic>
#include <thread>

/*!
 * Callback type for client count changes.
 */
using ClientCountCallback = std::function<void(int)>;

/*!
 * Monitors the client signal directory for connected OpenXR applications.
 *
 * Client applications create signal files (PID.txt) when connecting to
 * the runtime. This class monitors that directory and triggers callbacks
 * when the client count changes.
 */
class ClientMonitor
{
public:
	/*!
	 * Constructor.
	 * @param directory Path to the client signal directory
	 * @param callback Function to call when client count changes
	 */
	ClientMonitor(const std::wstring &directory, ClientCountCallback callback);

	/*!
	 * Destructor. Stops monitoring.
	 */
	~ClientMonitor();

	/*!
	 * Start monitoring the directory.
	 * @return true if monitoring started successfully
	 */
	bool
	start();

	/*!
	 * Stop monitoring the directory.
	 */
	void
	stop();

	/*!
	 * Get the current number of connected clients.
	 * @return Number of signal files in the directory
	 */
	int
	get_client_count() const;

private:
	/*!
	 * Worker thread function.
	 */
	void
	monitor_thread();

	/*!
	 * Count signal files in the directory.
	 * @return Number of .txt files
	 */
	int
	count_signal_files() const;

	std::wstring m_directory;
	ClientCountCallback m_callback;

	std::atomic<bool> m_running{false};
	std::thread m_thread;
	HANDLE m_stop_event = NULL;

	std::atomic<int> m_client_count{0};
};
