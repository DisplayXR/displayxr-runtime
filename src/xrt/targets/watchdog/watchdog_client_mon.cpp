// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - Client monitor implementation
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#include "watchdog_client_mon.h"

ClientMonitor::ClientMonitor(const std::wstring &directory, ClientCountCallback callback)
    : m_directory(directory), m_callback(callback)
{}

ClientMonitor::~ClientMonitor()
{
	stop();
}

bool
ClientMonitor::start()
{
	if (m_running.load()) {
		return true; // Already running
	}

	// Create stop event
	m_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (m_stop_event == NULL) {
		return false;
	}

	// Get initial count
	m_client_count = count_signal_files();

	// Start monitor thread
	m_running = true;
	m_thread = std::thread(&ClientMonitor::monitor_thread, this);

	return true;
}

void
ClientMonitor::stop()
{
	if (!m_running.load()) {
		return;
	}

	m_running = false;

	// Signal the thread to stop
	if (m_stop_event != NULL) {
		SetEvent(m_stop_event);
	}

	// Wait for thread to finish
	if (m_thread.joinable()) {
		m_thread.join();
	}

	// Cleanup
	if (m_stop_event != NULL) {
		CloseHandle(m_stop_event);
		m_stop_event = NULL;
	}
}

int
ClientMonitor::get_client_count() const
{
	return m_client_count.load();
}

void
ClientMonitor::monitor_thread()
{
	// Set up directory change notification
	HANDLE dir_handle =
	    FindFirstChangeNotificationW(m_directory.c_str(),
	                                 FALSE, // Don't watch subtree
	                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);

	if (dir_handle == INVALID_HANDLE_VALUE) {
		return;
	}

	HANDLE wait_handles[2] = {dir_handle, m_stop_event};

	while (m_running.load()) {
		// Wait for directory change or stop signal
		DWORD result = WaitForMultipleObjects(2, wait_handles, FALSE, 5000);

		if (result == WAIT_OBJECT_0) {
			// Directory changed - recount files
			int new_count = count_signal_files();
			int old_count = m_client_count.exchange(new_count);

			if (new_count != old_count && m_callback) {
				m_callback(new_count);
			}

			// Reset the notification
			FindNextChangeNotification(dir_handle);
		} else if (result == WAIT_OBJECT_0 + 1) {
			// Stop event signaled
			break;
		} else if (result == WAIT_TIMEOUT) {
			// Periodic check even without notification
			int new_count = count_signal_files();
			int old_count = m_client_count.exchange(new_count);

			if (new_count != old_count && m_callback) {
				m_callback(new_count);
			}
		}
		// WAIT_FAILED: continue loop and try again
	}

	FindCloseChangeNotification(dir_handle);
}

int
ClientMonitor::count_signal_files() const
{
	int count = 0;
	WIN32_FIND_DATAW find_data;

	std::wstring search_pattern = m_directory + L"\\*.txt";
	HANDLE find_handle = FindFirstFileW(search_pattern.c_str(), &find_data);

	if (find_handle != INVALID_HANDLE_VALUE) {
		do {
			// Skip directories
			if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				count++;
			}
		} while (FindNextFileW(find_handle, &find_data));

		FindClose(find_handle);
	}

	return count;
}
