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
!ifndef BUILD_NUM
	!define BUILD_NUM "0"
!endif

;--------------------------------
; General Attributes

Name "SRMonado ${VERSION}"
OutFile "${OUTPUT_DIR}\SRMonadoSetup-${VERSION}.${BUILD_NUM}.exe"
InstallDir "$PROGRAMFILES64\LeiaSR\SRMonado"
InstallDirRegKey HKLM "Software\LeiaSR\SRMonado" "InstallPath"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "TextFunc.nsh"

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
; DumpLog Function - saves installer log to file
; Based on NSIS Wiki example

!define LVM_GETITEMCOUNT 0x1004
!define LVM_GETITEMTEXTA 0x102D
!define LVM_GETITEMTEXTW 0x1073
!if "${NSIS_CHAR_SIZE}" > 1
	!define LVM_GETITEMTEXT ${LVM_GETITEMTEXTW}
!else
	!define LVM_GETITEMTEXT ${LVM_GETITEMTEXTA}
!endif

Function DumpLog
	Exch $5
	Push $0
	Push $1
	Push $2
	Push $3
	Push $4
	Push $6
	FindWindow $0 "#32770" "" $HWNDPARENT
	GetDlgItem $0 $0 1016
	StrCmp $0 0 exit
	FileOpen $5 $5 "w"
	StrCmp $5 "" exit
		SendMessage $0 ${LVM_GETITEMCOUNT} 0 0 $6
		System::Call "*(&t${NSIS_MAX_STRLEN})p.r3"
		StrCpy $2 0
		System::Call "*(i, i, i, i, i, p, i, i, i) p  (0, 0, 0, 0, 0, r3, ${NSIS_MAX_STRLEN}) .r1"
		loop: StrCmp $2 $6 done
			System::Call "User32::SendMessage(p, i, p, p) p ($0, ${LVM_GETITEMTEXT}, $2, r1)"
			System::Call "*$3(&t${NSIS_MAX_STRLEN} .r4)"
			FileWrite $5 "$4$\r$\n"
			IntOp $2 $2 + 1
			Goto loop
		done:
			FileClose $5
			System::Free $1
			System::Free $3
	exit:
		Pop $6
		Pop $4
		Pop $3
		Pop $2
		Pop $1
		Pop $0
		Pop $5
FunctionEnd

; Uninstaller version of DumpLog
Function un.DumpLog
	Exch $5
	Push $0
	Push $1
	Push $2
	Push $3
	Push $4
	Push $6
	FindWindow $0 "#32770" "" $HWNDPARENT
	GetDlgItem $0 $0 1016
	StrCmp $0 0 exit
	FileOpen $5 $5 "w"
	StrCmp $5 "" exit
		SendMessage $0 ${LVM_GETITEMCOUNT} 0 0 $6
		System::Call "*(&t${NSIS_MAX_STRLEN})p.r3"
		StrCpy $2 0
		System::Call "*(i, i, i, i, i, p, i, i, i) p  (0, 0, 0, 0, 0, r3, ${NSIS_MAX_STRLEN}) .r1"
		loop: StrCmp $2 $6 done
			System::Call "User32::SendMessage(p, i, p, p) p ($0, ${LVM_GETITEMTEXT}, $2, r1)"
			System::Call "*$3(&t${NSIS_MAX_STRLEN} .r4)"
			FileWrite $5 "$4$\r$\n"
			IntOp $2 $2 + 1
			Goto loop
		done:
			FileClose $5
			System::Free $1
			System::Free $3
	exit:
		Pop $6
		Pop $4
		Pop $3
		Pop $2
		Pop $1
		Pop $0
		Pop $5
FunctionEnd

;--------------------------------
; Installer Sections

Section "SRMonado Runtime" SecRuntime
	SectionIn RO  ; Required section

	SetOutPath "$INSTDIR"

	; Stop any running service before installing
	nsExec::ExecToLog 'taskkill /F /IM monado-service.exe'
	Sleep 500

	; Install runtime files
	File "${BIN_DIR}\SRMonadoClient.dll"
	File "${BIN_DIR}\monado-service.exe"

	; Install manifest
	File "${OUTPUT_DIR}\SRMonado_win64.json"

	; Install switcher if available
	File /nonfatal "${BIN_DIR}\LeiaXRSwitcher.exe"

	; Install runtime DLL dependencies
	File /nonfatal "${BIN_DIR}\*.dll"

	; Create AppData directories
	CreateDirectory "$APPDATA\LeiaSR"
	CreateDirectory "$APPDATA\LeiaSR\SRMonado"

	; Write registry keys
	WriteRegStr HKLM "Software\LeiaSR\SRMonado" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\LeiaSR\SRMonado" "Version" "${VERSION}"

	; Set as active OpenXR runtime
	WriteRegStr HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime" "$INSTDIR\SRMonado_win64.json"

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
		"DisplayIcon" "$INSTDIR\monado-service.exe"
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

	; Save installation log to install directory
	StrCpy $0 "$INSTDIR\install.log"
	Push $0
	Call DumpLog

SectionEnd

Section "Start Menu Shortcuts" SecShortcuts
	CreateDirectory "$SMPROGRAMS\LeiaSR"

	; Add switcher shortcut if installed
	IfFileExists "$INSTDIR\LeiaXRSwitcher.exe" 0 +2
		CreateShortCut "$SMPROGRAMS\LeiaSR\LeiaXR Runtime Switcher.lnk" "$INSTDIR\LeiaXRSwitcher.exe"

	CreateShortCut "$SMPROGRAMS\LeiaSR\Uninstall SRMonado.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Stop running processes
	nsExec::ExecToLog 'taskkill /F /IM monado-service.exe'
	Sleep 500

	; Remove files
	Delete "$INSTDIR\SRMonadoClient.dll"
	Delete "$INSTDIR\monado-service.exe"
	Delete "$INSTDIR\LeiaXRSwitcher.exe"
	Delete "$INSTDIR\SRMonado_win64.json"
	Delete "$INSTDIR\install.log"

	; Remove any remaining DLLs
	Delete "$INSTDIR\*.dll"

	; Save uninstall log to temp before removing directory
	StrCpy $0 "$TEMP\SRMonado_uninstall.log"
	Push $0
	Call un.DumpLog

	; Remove uninstaller
	Delete "$INSTDIR\Uninstall.exe"

	; Remove install directory (if empty)
	RMDir "$INSTDIR"
	RMDir "$PROGRAMFILES64\LeiaSR"

	; Remove AppData directory
	RMDir "$APPDATA\LeiaSR\SRMonado"
	; Don't remove LeiaSR folder as it may be shared with SRHydra

	; Remove Start Menu shortcuts
	Delete "$SMPROGRAMS\LeiaSR\LeiaXR Runtime Switcher.lnk"
	Delete "$SMPROGRAMS\LeiaSR\Uninstall SRMonado.lnk"
	RMDir "$SMPROGRAMS\LeiaSR"

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
