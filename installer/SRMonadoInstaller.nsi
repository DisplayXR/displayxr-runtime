; SRMonado Windows Installer Script
; Copyright 2024, Leia Inc.
; SPDX-License-Identifier: BSL-1.0

;--------------------------------
; Build-time definitions (passed from CMake)
; VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH
; BIN_DIR, SOURCE_DIR, OUTPUT_DIR

!ifndef VERSION
	!define VERSION "25.0.0"
!endif
!ifndef VERSION_MAJOR
	!define VERSION_MAJOR "25"
!endif
!ifndef VERSION_MINOR
	!define VERSION_MINOR "0"
!endif
!ifndef VERSION_PATCH
	!define VERSION_PATCH "0"
!endif

;--------------------------------
; General Attributes

Name "SRMonado ${VERSION}"
OutFile "${OUTPUT_DIR}\SRMonadoSetup-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\LeiaSR\SRMonado"
InstallDirRegKey HKLM "Software\LeiaSR\SRMonado" "InstallPath"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

;--------------------------------
; Interface Settings

!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_DIR}\doc\monado.ico"
!define MUI_UNICON "${SOURCE_DIR}\doc\monado.ico"

;--------------------------------
; Pages

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

;--------------------------------
; Languages

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer Sections

Section "SRMonado Runtime" SecRuntime
	SectionIn RO  ; Required section

	SetOutPath "$INSTDIR"

	; Stop any running watchdog/service before installing
	nsExec::ExecToLog 'taskkill /F /IM SRMonadoWatchdog.exe'
	nsExec::ExecToLog 'taskkill /F /IM monado-service.exe'
	Sleep 500

	; Install runtime files
	File "${BIN_DIR}\SRMonadoClient.dll"
	File "${BIN_DIR}\monado-service.exe"
	File "${BIN_DIR}\SRMonadoWatchdog.exe"

	; Install manifest
	File "${OUTPUT_DIR}\SRMonado_win64.json"

	; Install switcher if available
	File /nonfatal "${BIN_DIR}\LeiaXRSwitcher.exe"

	; Install runtime DLL dependencies
	File /nonfatal "${BIN_DIR}\*.dll"

	; Create AppData directories
	CreateDirectory "$APPDATA\LeiaSR"
	CreateDirectory "$APPDATA\LeiaSR\SRMonado"
	CreateDirectory "$APPDATA\LeiaSR\SRMonado\Clients"

	; Write registry keys
	WriteRegStr HKLM "Software\LeiaSR\SRMonado" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\LeiaSR\SRMonado" "Version" "${VERSION}"

	; Set as active OpenXR runtime
	WriteRegStr HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime" "$INSTDIR\SRMonado_win64.json"

	; Register watchdog for auto-start
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "SRMonadoWatchdog" '"$INSTDIR\SRMonadoWatchdog.exe"'

	; Write uninstaller
	WriteUninstaller "$INSTDIR\Uninstall.exe"

	; Add to Add/Remove Programs
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"DisplayName" "SRMonado OpenXR Runtime"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"DisplayIcon" "$INSTDIR\SRMonadoWatchdog.exe"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"Publisher" "Leia Inc."
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"NoRepair" 1

	; Calculate installed size
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado" \
		"EstimatedSize" "$0"

SectionEnd

Section "Start Menu Shortcuts" SecShortcuts
	CreateDirectory "$SMPROGRAMS\LeiaSR"
	CreateShortCut "$SMPROGRAMS\LeiaSR\SRMonado Watchdog.lnk" "$INSTDIR\SRMonadoWatchdog.exe"

	; Add switcher shortcut if installed
	IfFileExists "$INSTDIR\LeiaXRSwitcher.exe" 0 +2
		CreateShortCut "$SMPROGRAMS\LeiaSR\LeiaXR Runtime Switcher.lnk" "$INSTDIR\LeiaXRSwitcher.exe"

	CreateShortCut "$SMPROGRAMS\LeiaSR\Uninstall SRMonado.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Launch Watchdog on Install" SecLaunch
	; Start the watchdog
	Exec '"$INSTDIR\SRMonadoWatchdog.exe"'
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Stop running processes
	nsExec::ExecToLog 'taskkill /F /IM SRMonadoWatchdog.exe'
	nsExec::ExecToLog 'taskkill /F /IM monado-service.exe'
	Sleep 500

	; Remove files
	Delete "$INSTDIR\SRMonadoClient.dll"
	Delete "$INSTDIR\monado-service.exe"
	Delete "$INSTDIR\SRMonadoWatchdog.exe"
	Delete "$INSTDIR\LeiaXRSwitcher.exe"
	Delete "$INSTDIR\SRMonado_win64.json"
	Delete "$INSTDIR\Uninstall.exe"

	; Remove any remaining DLLs
	Delete "$INSTDIR\*.dll"

	; Remove install directory (if empty)
	RMDir "$INSTDIR"
	RMDir "$PROGRAMFILES64\LeiaSR"

	; Remove client signal files
	RMDir /r "$APPDATA\LeiaSR\SRMonado\Clients"
	RMDir "$APPDATA\LeiaSR\SRMonado"
	; Don't remove LeiaSR folder as it may be shared with SRHydra

	; Remove Start Menu shortcuts
	Delete "$SMPROGRAMS\LeiaSR\SRMonado Watchdog.lnk"
	Delete "$SMPROGRAMS\LeiaSR\LeiaXR Runtime Switcher.lnk"
	Delete "$SMPROGRAMS\LeiaSR\Uninstall SRMonado.lnk"
	RMDir "$SMPROGRAMS\LeiaSR"

	; Remove auto-start registry entry
	DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "SRMonadoWatchdog"

	; Remove SRMonado registry keys
	DeleteRegKey HKLM "Software\LeiaSR\SRMonado"
	DeleteRegKey /ifempty HKLM "Software\LeiaSR"

	; Remove from Add/Remove Programs
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SRMonado"

	; Only remove ActiveRuntime if it points to our manifest
	ReadRegStr $0 HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"
	StrCmp $0 "$INSTDIR\SRMonado_win64.json" 0 +2
		DeleteRegValue HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"

SectionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecRuntime} "SRMonado OpenXR runtime files (required)"
	!insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Create Start Menu shortcuts"
	!insertmacro MUI_DESCRIPTION_TEXT ${SecLaunch} "Start the watchdog service after installation"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
	; Check for 64-bit Windows
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "SRMonado requires 64-bit Windows."
		Abort
	${EndIf}
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "SRMonado"
VIAddVersionKey "CompanyName" "Leia Inc."
VIAddVersionKey "LegalCopyright" "Copyright (c) 2024 Leia Inc."
VIAddVersionKey "FileDescription" "SRMonado OpenXR Runtime Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
