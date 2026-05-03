; DisplayXR Shell Installer Script
; Copyright 2024-2026, DisplayXR
; SPDX-License-Identifier: BSL-1.0
;
; Standalone installer for the DisplayXR Shell — a workspace controller
; for the DisplayXR runtime. Reads HKLM\Software\DisplayXR\Runtime\InstallPath
; to locate the runtime, installs into that directory alongside the runtime
; binaries, and registers itself at
; HKLM\Software\DisplayXR\WorkspaceControllers\shell so the runtime's
; service orchestrator can discover and spawn it. See
; docs/specs/workspace-controller-registration.md for the registration
; contract.
;
; The DisplayXR runtime is a hard prerequisite — this installer aborts on
; .onInit if the runtime's registry key is missing. When the runtime is
; uninstalled, its uninstaller invokes this installer's UninstallString
; with /S, so user-initiated runtime removal cleans the shell up too.

;--------------------------------
; Build-time definitions (passed from CMake)
; VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH
; BIN_DIR, SOURCE_DIR

!ifndef VERSION
	!define VERSION "0.0.0"
!endif
!ifndef VERSION_MAJOR
	!define VERSION_MAJOR "0"
!endif
!ifndef VERSION_MINOR
	!define VERSION_MINOR "0"
!endif
!ifndef VERSION_PATCH
	!define VERSION_PATCH "0"
!endif
!ifndef BUILD_NUM
	!define BUILD_NUM "0"
!endif

;--------------------------------
; General Attributes

Name "DisplayXR Shell ${VERSION}"
OutFile "${OUTPUT_DIR}\DisplayXRShellSetup-${VERSION}.${BUILD_NUM}.exe"
; .onInit overrides this with the runtime install path read from registry.
InstallDir "$PROGRAMFILES64\DisplayXR\Runtime"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

;--------------------------------
; Interface Settings

!define MUI_ABORTWARNING
; White icon so the installer .exe is visible in dark mode Explorer/taskbar
!define MUI_ICON "${SOURCE_DIR}\doc\displayxr_white.ico"
!define MUI_UNICON "${SOURCE_DIR}\doc\displayxr_white.ico"

;--------------------------------
; Pages

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
; No directory page — we install into the runtime's directory.
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

;--------------------------------
; Languages

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer Section

Section "DisplayXR Shell" SecShell
	SectionIn RO  ; Required section

	; Force 64-bit registry view so HKLM\Software\DisplayXR\* and the
	; Add/Remove entry land in the non-redirected view that the 64-bit
	; service reads via KEY_WOW64_64KEY. Matches the runtime installer.
	SetRegView 64

	SetOutPath "$INSTDIR"

	; Stop running shell to release the exe / mutex.
	DetailPrint "Stopping any running DisplayXR Shell..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-shell.exe'
	Pop $0
	Sleep 1500

	; Install the shell binary into the runtime tree.
	File "${BIN_DIR}\displayxr-shell.exe"

	; Self-uninstaller co-located with the shell exe. Named distinctly so it
	; doesn't collide with the runtime's Uninstall.exe.
	WriteUninstaller "$INSTDIR\Uninstall-Shell.exe"

	; Register with the service orchestrator.
	; HKLM\Software\DisplayXR\WorkspaceControllers\shell
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell" \
		"Binary" "$INSTDIR\displayxr-shell.exe"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell" \
		"DisplayName" "DisplayXR Shell"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell" \
		"Vendor" "DisplayXR"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell" \
		"Version" "${VERSION}"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell" \
		"UninstallString" "$\"$INSTDIR\Uninstall-Shell.exe$\""

	; Publish tray-menu actions for the service orchestrator. Service
	; reads HKLM\Software\DisplayXR\WorkspaceControllers\shell\Actions\*
	; (alphabetical by subkey name) and renders the workspace submenu
	; from this list. See docs/specs/workspace-controller-registration.md.
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\01-enable" \
		"Label" "Enable"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\01-enable" \
		"Type" "lifecycle:enable"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\02-auto" \
		"Label" "Auto"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\02-auto" \
		"Type" "lifecycle:auto"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\03-disable" \
		"Label" "Disable"
	WriteRegStr HKLM "Software\DisplayXR\WorkspaceControllers\shell\Actions\03-disable" \
		"Type" "lifecycle:disable"

	; Add/Remove Programs entry so the user can uninstall the shell on its own.
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"DisplayName" "DisplayXR Shell"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"UninstallString" "$\"$INSTDIR\Uninstall-Shell.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall-Shell.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"DisplayIcon" "$INSTDIR\displayxr-shell.exe"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"Publisher" "DisplayXR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"NoRepair" 1

	; Calculate installed size (for Add/Remove Programs).
	${GetSize} "$INSTDIR" "/M=displayxr-shell.exe /S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell" \
		"EstimatedSize" "$0"
SectionEnd

Section "Start Menu Shortcut" SecShortcut
	CreateDirectory "$SMPROGRAMS\DisplayXR"
	CreateShortCut "$SMPROGRAMS\DisplayXR\DisplayXR Shell.lnk" \
		"$INSTDIR\displayxr-shell.exe"
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Same 64-bit view as the install section.
	SetRegView 64

	; Stop running shell.
	DetailPrint "Stopping any running DisplayXR Shell..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-shell.exe'
	Sleep 1000

	; Remove only files we installed. Do NOT touch runtime files.
	Delete "$INSTDIR\displayxr-shell.exe"
	Delete "$INSTDIR\Uninstall-Shell.exe"

	; Start Menu shortcut.
	Delete "$SMPROGRAMS\DisplayXR\DisplayXR Shell.lnk"
	; The runtime owns $SMPROGRAMS\DisplayXR; don't RMDir it here.

	; Registry — registration + Add/Remove entry.
	DeleteRegKey HKLM "Software\DisplayXR\WorkspaceControllers\shell"
	; Don't try DeleteRegKey /ifempty on the parent; runtime's uninstaller
	; (or another workspace app's installer) may still own siblings.

	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRShell"

	; Do NOT RMDir $INSTDIR — it's the runtime's directory.
SectionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecShell} "DisplayXR Shell — workspace controller for the DisplayXR runtime (required)"
	!insertmacro MUI_DESCRIPTION_TEXT ${SecShortcut} "Create a Start Menu shortcut"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
	; 64-bit Windows check.
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "DisplayXR Shell requires 64-bit Windows."
		Abort
	${EndIf}

	; Force 64-bit registry view — see install section comment for rationale.
	SetRegView 64

	; Hard prereq: DisplayXR Runtime must be installed.
	ReadRegStr $R0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $R0 == ""
		MessageBox MB_OK|MB_ICONSTOP \
			"DisplayXR Runtime is not installed.$\r$\n$\r$\n\
			The DisplayXR Shell requires the runtime to be installed first.$\r$\n$\r$\n\
			Install the DisplayXR Runtime, then run this installer again."
		Abort
	${EndIf}

	; Install into the runtime's directory so the shell sits next to
	; openxr_loader.dll and other runtime DLLs the shell binds against.
	StrCpy $INSTDIR "$R0"
FunctionEnd

Function un.onInit
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "Uninstall requires 64-bit Windows."
		Abort
	${EndIf}
	SetRegView 64
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR Shell"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2024-2026 DisplayXR"
VIAddVersionKey "FileDescription" "DisplayXR Shell Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
