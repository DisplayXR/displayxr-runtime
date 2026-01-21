// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SRMonado Watchdog - Main entry point
 *
 * The watchdog monitors for OpenXR client applications and launches
 * the monado-service when needed. It provides a system tray icon for
 * user interaction and status display.
 *
 * @author Leia Inc.
 * @ingroup targets_watchdog
 */

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <memory>

#include "watchdog_service_mgr.h"
#include "watchdog_client_mon.h"
#include "watchdog_tray.h"

// Unique mutex name for single-instance enforcement
static const wchar_t *WATCHDOG_MUTEX_NAME = L"Global\\SRMonadoWatchdogMutex";

// Global instances
static std::unique_ptr<ServiceManager> g_service_mgr;
static std::unique_ptr<ClientMonitor> g_client_mon;
static std::unique_ptr<TrayIcon> g_tray;

// Application state
static bool g_running = true;

/*!
 * Get the directory where this executable is located.
 */
static std::wstring
get_executable_directory()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(NULL, path, MAX_PATH);
	PathRemoveFileSpecW(path);
	return std::wstring(path);
}

/*!
 * Get the path to the monado-service executable.
 */
static std::wstring
get_service_path()
{
	std::wstring dir = get_executable_directory();
	return dir + L"\\monado-service.exe";
}

/*!
 * Get the client signal directory path.
 * Creates the directory if it doesn't exist.
 */
static std::wstring
get_client_signal_directory()
{
	wchar_t appdata[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
		std::wstring path = std::wstring(appdata) + L"\\LeiaSR\\SRMonado\\Clients";

		// Create directory structure if it doesn't exist
		std::wstring parent1 = std::wstring(appdata) + L"\\LeiaSR";
		std::wstring parent2 = parent1 + L"\\SRMonado";

		CreateDirectoryW(parent1.c_str(), NULL);
		CreateDirectoryW(parent2.c_str(), NULL);
		CreateDirectoryW(path.c_str(), NULL);

		return path;
	}
	return L"";
}

/*!
 * Callback when client count changes.
 */
static void
on_client_count_changed(int count)
{
	if (g_tray) {
		g_tray->set_client_count(count);
	}

	if (count > 0 && g_service_mgr && !g_service_mgr->is_running()) {
		// Client appeared, launch service
		g_service_mgr->start_service();
	}
}

/*!
 * Callback for tray menu actions.
 */
static void
on_tray_action(TrayAction action)
{
	switch (action) {
	case TrayAction::ShowStatus:
		// TODO: Show status window
		break;

	case TrayAction::RestartService:
		if (g_service_mgr) {
			g_service_mgr->stop_service();
			g_service_mgr->start_service();
		}
		break;

	case TrayAction::StopService:
		if (g_service_mgr) {
			g_service_mgr->stop_service();
		}
		break;

	case TrayAction::Exit: g_running = false; break;
	}
}

/*!
 * Windows message loop.
 */
static int
run_message_loop()
{
	MSG msg;
	while (g_running && GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return (int)msg.wParam;
}

/*!
 * Application entry point.
 */
int WINAPI
wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	// Single instance enforcement
	HANDLE mutex = CreateMutexW(NULL, TRUE, WATCHDOG_MUTEX_NAME);
	if (mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
		if (mutex) {
			CloseHandle(mutex);
		}
		// Another instance is already running
		return 0;
	}

	// Initialize COM for shell functions
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	// Get paths
	std::wstring service_path = get_service_path();
	std::wstring client_dir = get_client_signal_directory();

	if (client_dir.empty()) {
		MessageBoxW(NULL, L"Failed to get client signal directory", L"SRMonado Watchdog", MB_ICONERROR);
		CoUninitialize();
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return 1;
	}

	// Create components
	g_service_mgr = std::make_unique<ServiceManager>(service_path);
	g_client_mon = std::make_unique<ClientMonitor>(client_dir, on_client_count_changed);
	g_tray = std::make_unique<TrayIcon>(hInstance, on_tray_action);

	// Start monitoring
	if (!g_client_mon->start()) {
		MessageBoxW(NULL, L"Failed to start client monitor", L"SRMonado Watchdog", MB_ICONERROR);
		CoUninitialize();
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return 1;
	}

	// Show tray icon
	if (!g_tray->create()) {
		MessageBoxW(NULL, L"Failed to create tray icon", L"SRMonado Watchdog", MB_ICONERROR);
		g_client_mon->stop();
		CoUninitialize();
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return 1;
	}

	// Update initial state
	g_tray->set_service_running(g_service_mgr->is_running());
	g_tray->set_client_count(g_client_mon->get_client_count());

	// Periodically check service status
	SetTimer(NULL, 0, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
		if (g_service_mgr && g_tray) {
			g_tray->set_service_running(g_service_mgr->is_running());
		}
	});

	// Run message loop
	int result = run_message_loop();

	// Cleanup
	g_tray->destroy();
	g_client_mon->stop();
	g_service_mgr->stop_service();

	g_tray.reset();
	g_client_mon.reset();
	g_service_mgr.reset();

	CoUninitialize();
	ReleaseMutex(mutex);
	CloseHandle(mutex);

	return result;
}
