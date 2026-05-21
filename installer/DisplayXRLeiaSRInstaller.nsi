; DisplayXR Leia SR Plug-in Installer Script
; Copyright 2026, DisplayXR
; SPDX-License-Identifier: BSL-1.0
;
; Ships the Leia SR display-processor plug-in DLL (issue #256 /
; ADR-019 / plan §4.6). Registers it at
; HKLM\Software\DisplayXR\DisplayProcessors\leia-sr so the runtime's
; registry-driven discovery (target_plugin_loader.c) picks it up at
; xrCreateInstance time.
;
; Hard prereq: DisplayXR runtime, located via
; HKLM\Software\DisplayXR\Runtime\InstallPath. Without the runtime,
; the plug-in's DisplayXRClient.lib import can't resolve and
; LoadLibraryEx fails — we fail fast at install time with a clear
; error pointing at the runtime download.

;--------------------------------
; Build-time definitions (passed from CMake):
;   VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, BUILD_NUM
;   BIN_DIR        — _package/bin/ (DisplayXR-LeiaSR.dll lives in BIN_DIR/plugins/)
;   SR_VK_BETA_DLL — abs path to bundled SimulatedRealityVulkanBeta.dll
;   SOURCE_DIR     — repo root (for icon)
;   OUTPUT_DIR     — _package/

!ifndef VERSION
	!define VERSION "1.3.4"
!endif
!ifndef VERSION_MAJOR
	!define VERSION_MAJOR "1"
!endif
!ifndef VERSION_MINOR
	!define VERSION_MINOR "3"
!endif
!ifndef VERSION_PATCH
	!define VERSION_PATCH "4"
!endif
!ifndef BUILD_NUM
	!define BUILD_NUM "0"
!endif

;--------------------------------
; General Attributes

Name "DisplayXR Leia SR Plug-in ${VERSION}"
OutFile "${OUTPUT_DIR}\DisplayXRLeiaSRSetup-${VERSION}.${BUILD_NUM}.exe"
InstallDir "$PROGRAMFILES64\DisplayXR\Plugins\LeiaSR"
InstallDirRegKey HKLM "Software\DisplayXR\Plugins\LeiaSR" "InstallPath"
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
!define MUI_ICON "${SOURCE_DIR}\assets\displayxr_white.ico"
!define MUI_UNICON "${SOURCE_DIR}\assets\displayxr_white.ico"

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

Section "Leia SR Plug-in" SecPlugin
	SectionIn RO

	; Force 64-bit registry view — NSIS is 32-bit and would otherwise
	; land at HKLM\Software\WOW6432Node\DisplayXR\* (mismatch with the
	; runtime's 64-bit-view contract).
	SetRegView 64

	; -----------------------------------------------------------------
	; Hard prereq: the DisplayXR runtime must be installed first.
	; Without it, DisplayXR-LeiaSR.dll's DisplayXRClient.dll import
	; can't resolve and LoadLibraryEx fails. Mirrors the workspace-
	; controller installer's prereq check pattern.
	; -----------------------------------------------------------------
	ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $0 == ""
		MessageBox MB_OK|MB_ICONSTOP \
			"DisplayXR Runtime is required and was not found.$\r$\n$\r$\nInstall it first from https://github.com/DisplayXR/displayxr-runtime/releases then retry this plug-in installer."
		Abort
	${EndIf}
	${IfNot} ${FileExists} "$0\DisplayXRClient.dll"
		MessageBox MB_OK|MB_ICONSTOP \
			"DisplayXR Runtime install path registered ($0) but DisplayXRClient.dll is missing. Reinstall the runtime then retry this plug-in installer."
		Abort
	${EndIf}
	DetailPrint "Verified DisplayXR Runtime at $0"

	SetOutPath "$INSTDIR"

	; Kill any in-flight cube / app process that might have the plug-in
	; loaded (mirrors the runtime installer's process-kill pattern).
	; Once the registry is rewritten, the next xrCreateInstance picks
	; the new DLL up.

	; Install the plug-in DLL.
	File "${BIN_DIR}\plugins\DisplayXR-LeiaSR.dll"

	; Bundle the SR SDK's Vulkan Beta DLL alongside the plug-in DLL
	; so the plug-in's /DELAYLOAD:SimulatedRealityVulkanBeta.dll
	; resolves via LOAD_WITH_ALTERED_SEARCH_PATH (the runtime's loader
	; uses this flag, putting the plug-in DLL's own directory first in
	; the search). Apps on boxes that have only the D3D11 weaver never
	; trigger the delay-load — the bundle just covers the VK path.
	;
	; SR Platform's own installer may also provide
	; SimulatedRealityVulkanBeta.dll on machine PATH; either resolution
	; works.
	File "${SR_VK_BETA_DLL}"

	; -----------------------------------------------------------------
	; Register at HKLM\Software\DisplayXR\DisplayProcessors\leia-sr per
	; the contract in docs/specs/runtime/plugin-discovery.md.
	;
	; ProbeOrder=50 — lower than sim-display's 200, so on systems with
	; Leia hardware this plug-in's probe runs first and wins. On
	; systems without Leia hardware, probe declines via
	; XRT_ERROR_PROBER_NOT_SUPPORTED and sim-display takes over.
	;
	; UninstallString set so the runtime's cascade-uninstall cleans us
	; up if the user uninstalls the runtime before this plug-in.
	; -----------------------------------------------------------------
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Binary"          "$INSTDIR\DisplayXR-LeiaSR.dll"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"DisplayName"     "DisplayXR Leia SR"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Vendor"          "Leia Inc."
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Version"         "${VERSION}"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegDWORD HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"ProbeOrder"      50

	; Track our own install location for the uninstaller.
	WriteRegStr HKLM "Software\DisplayXR\Plugins\LeiaSR" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\DisplayXR\Plugins\LeiaSR" "Version"     "${VERSION}"

	; Write uninstaller.
	WriteUninstaller "$INSTDIR\Uninstall.exe"

	; Add to Add/Remove Programs.
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayName" "DisplayXR Leia SR Plug-in"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayIcon" "$INSTDIR\DisplayXR-LeiaSR.dll"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"Publisher" "Leia Inc."
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"NoRepair" 1

	; Calculate installed size.
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"EstimatedSize" "$0"

SectionEnd

;--------------------------------
; Uninstaller

Section "Uninstall"
	SetRegView 64

	; Remove the registry subkey first so the runtime's discovery
	; doesn't pick us up after the DLL is gone (race window is very
	; narrow but cheap to close).
	DeleteRegKey HKLM "Software\DisplayXR\DisplayProcessors\leia-sr"

	; Remove our files.
	Delete "$INSTDIR\DisplayXR-LeiaSR.dll"
	Delete "$INSTDIR\SimulatedRealityVulkanBeta.dll"
	Delete "$INSTDIR\Uninstall.exe"

	; Remove install dir.
	RMDir "$INSTDIR"
	RMDir "$PROGRAMFILES64\DisplayXR\Plugins"
	; Don't RMDir $PROGRAMFILES64\DisplayXR — the runtime's uninstaller
	; owns that directory.

	; Remove tracking + Add/Remove entry.
	DeleteRegKey HKLM "Software\DisplayXR\Plugins\LeiaSR"
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR"

	; Drop the now-empty Plugins parent if no other plug-ins remain.
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\Plugins"

SectionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecPlugin} "DisplayXR Leia SR display-processor plug-in (required)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "DisplayXR Leia SR Plug-in requires 64-bit Windows."
		Abort
	${EndIf}
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR Leia SR Plug-in"
VIAddVersionKey "CompanyName" "Leia Inc."
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Leia Inc."
VIAddVersionKey "FileDescription" "DisplayXR Leia SR Display Processor Plug-in Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
