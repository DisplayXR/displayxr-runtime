// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - System tray icon header
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#pragma once

#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

/*!
 * Tray menu actions.
 */
enum class TrayAction
{
	ShowStatus,
	RestartService,
	StopService,
	Exit
};

/*!
 * Callback type for tray actions.
 */
using TrayActionCallback = std::function<void(TrayAction)>;

/*!
 * Manages the system tray icon and context menu.
 */
class TrayIcon
{
public:
	/*!
	 * Constructor.
	 * @param hInstance Application instance handle
	 * @param callback Function to call on menu actions
	 */
	TrayIcon(HINSTANCE hInstance, TrayActionCallback callback);

	/*!
	 * Destructor.
	 */
	~TrayIcon();

	/*!
	 * Create and show the tray icon.
	 * @return true if successful
	 */
	bool
	create();

	/*!
	 * Remove the tray icon.
	 */
	void
	destroy();

	/*!
	 * Update the service running state.
	 * @param running true if service is running
	 */
	void
	set_service_running(bool running);

	/*!
	 * Update the connected client count.
	 * @param count Number of connected clients
	 */
	void
	set_client_count(int count);

private:
	/*!
	 * Window procedure for the hidden tray window.
	 */
	static LRESULT CALLBACK
	window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	/*!
	 * Show the context menu.
	 */
	void
	show_context_menu();

	/*!
	 * Update the tooltip text.
	 */
	void
	update_tooltip();

	HINSTANCE m_hInstance = NULL;
	HWND m_hwnd = NULL;
	NOTIFYICONDATAW m_nid = {};
	TrayActionCallback m_callback;

	bool m_service_running = false;
	int m_client_count = 0;

	static const UINT WM_TRAYICON = WM_USER + 1;
	static const UINT IDM_STATUS = 1001;
	static const UINT IDM_RESTART = 1002;
	static const UINT IDM_STOP = 1003;
	static const UINT IDM_EXIT = 1004;
};
