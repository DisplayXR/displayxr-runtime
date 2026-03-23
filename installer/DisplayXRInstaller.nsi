; DisplayXR Windows Installer Script
; Copyright 2024-2026, DisplayXR
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

Name "DisplayXR ${VERSION}"
OutFile "${OUTPUT_DIR}\DisplayXRSetup-${VERSION}.${BUILD_NUM}.exe"
InstallDir "$PROGRAMFILES64\DisplayXR\Runtime"
InstallDirRegKey HKLM "Software\DisplayXR\Runtime" "InstallPath"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "TextFunc.nsh"
!include "WinMessages.nsh"

; Windows constants for PATH modification
!ifndef HWND_BROADCAST
	!define HWND_BROADCAST 0xFFFF
!endif
!ifndef WM_SETTINGCHANGE
	!define WM_SETTINGCHANGE 0x001A
!endif

;--------------------------------
; Interface Settings

!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_DIR}\doc\displayxr.ico"
!define MUI_UNICON "${SOURCE_DIR}\doc\displayxr.ico"

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

!ifndef LVM_GETITEMCOUNT
	!define LVM_GETITEMCOUNT 0x1004
!endif
!ifndef LVM_GETITEMTEXTA
	!define LVM_GETITEMTEXTA 0x102D
!endif
!ifndef LVM_GETITEMTEXTW
	!define LVM_GETITEMTEXTW 0x1073
!endif
!ifndef LVM_GETITEMTEXT
	!if "${NSIS_CHAR_SIZE}" > 1
		!define LVM_GETITEMTEXT ${LVM_GETITEMTEXTW}
	!else
		!define LVM_GETITEMTEXT ${LVM_GETITEMTEXTA}
	!endif
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

; Pure NSIS Lowercase function (Installer)
Function StrLower
  Exch $0 ; Input string
  Push $1 ; Index
  Push $2 ; Current Char
  Push $3 ; Result Buffer
  
  StrCpy $3 "" 
  StrCpy $1 0  

loop:
  StrCpy $2 $0 1 $1 
  StrCmp $2 "" done 
  
  ; FIX: Added 'W' to CharLower and used 'w' for the type
  System::Call "user32::CharLowerW(w r2)w.r2"
  
  StrCpy $3 "$3$2" 
  IntOp $1 $1 + 1  
  Goto loop

done:
  StrCpy $0 $3     
  Pop $3
  Pop $2
  Pop $1
  Exch $0          
FunctionEnd

; Pure NSIS Lowercase function (Uninstaller)
Function un.StrLower
  Exch $0 ; Input string
  Push $1 ; Index
  Push $2 ; Current Char
  Push $3 ; Result Buffer
  
  StrCpy $3 "" 
  StrCpy $1 0  

loop:
  StrCpy $2 $0 1 $1 
  StrCmp $2 "" done 
  
  ; FIX: Added 'W' to CharLower and used 'w' for the type
  System::Call "user32::CharLowerW(w r2)w.r2"
  
  StrCpy $3 "$3$2" 
  IntOp $1 $1 + 1  
  Goto loop

done:
  StrCpy $0 $3     
  Pop $3
  Pop $2
  Pop $1
  Exch $0          
FunctionEnd

;--------------------------------
; PATH manipulation functions
; Based on NSIS Wiki and SR Platform installer

; AddToPath - Adds a directory to the system PATH
; Uses System::Call to read REG_EXPAND_SZ properly (ReadRegStr can fail on
; REG_EXPAND_SZ values, returning empty and causing PATH to be overwritten).
; Usage: Push "C:\path\to\add"
;        Call AddToPath
Function AddToPath
	Exch $0  ; Path to add
	Push $1  ; Current PATH
	Push $2  ; Temp for search result
	Push $3  ; Registry handle
	Push $4  ; Data type / buffer pointer
	Push $5  ; Data length

	; Open the Environment registry key
	; 0x80000002 = HKEY_LOCAL_MACHINE, 0x20019 = KEY_READ
	System::Call 'Advapi32::RegOpenKeyExW(i 0x80000002, w "SYSTEM\CurrentControlSet\Control\Session Manager\Environment", i 0, i 0x20019, *i .r3) i .r2'
	StrCmp $2 0 0 reg_failed

	; Query the size of the Path value first (pass null buffer to get size)
	System::Call 'Advapi32::RegQueryValueExW(i r3, w "Path", i 0, i 0, i 0, *i .r5) i .r2'
	StrCmp $2 0 0 reg_close_failed

	; Allocate buffer and read the value
	System::Alloc $5
	Pop $4  ; $4 = buffer pointer
	System::Call 'Advapi32::RegQueryValueExW(i r3, w "Path", i 0, i 0, i r4, *i r5) i .r2'
	StrCmp $2 0 0 reg_free_failed

	; Copy the buffer contents to $1 as a string
	System::Call '*$4(&w${NSIS_MAX_STRLEN} .r1)'

	; Free buffer and close key
	System::Free $4
	System::Call 'Advapi32::RegCloseKey(i r3)'

	Goto got_path

reg_free_failed:
	System::Free $4
reg_close_failed:
	System::Call 'Advapi32::RegCloseKey(i r3)'
reg_failed:
	; Fall back to ReadRegStr if the System::Call approach fails
	ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"

got_path:
	; Check if PATH is empty - if so, this is suspicious on a normal Windows system.
	; Read from the expanded environment as a safety fallback to avoid wiping PATH.
	StrCmp $1 "" try_expanded

	Goto check_exists

try_expanded:
	; Last resort: read the current PATH from the process environment
	; This won't have unexpanded %vars% but is better than losing PATH entirely
	ReadEnvStr $1 PATH
	StrCmp $1 "" empty_path

check_exists:
	; Check if path already exists in PATH (avoid duplicates)
	; Extra Push $0: StrStr returns via Exch $0 which clobbers $0 and
	; consumes one stack item below the args. This extra push provides
	; the item for Exch $0 to restore $0 to path_to_add.
	Push $0
	Push $1
	Push $0
	Call StrStr
	Pop $2
	StrCmp $2 "" 0 already_exists

	; Append to existing PATH
	StrCpy $0 "$1;$0"
	Goto write_path

empty_path:
	; PATH is truly empty (fresh system?), just use our path (already in $0)

write_path:
	; Write new PATH
	WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$0"
	DetailPrint "Added to system PATH"
	Goto done

already_exists:
	DetailPrint "Path already exists in system PATH, skipping"

done:
	Pop $5
	Pop $4
	Pop $3
	Pop $2
	Pop $1
	Pop $0
FunctionEnd

; RemoveFromPath - Removes a directory from the system PATH
; Uses System::Call to read REG_EXPAND_SZ properly (ReadRegStr can fail on
; REG_EXPAND_SZ values, returning empty and causing PATH to be overwritten).
; Handles multiple occurrences, trailing backslashes, and case variations
; Usage: Push "C:\path\to\remove" 
;        Call un.RemoveFromPath
; RemoveFromPath - Removes a directory from the system PATH
; Handles 64-bit registry, Unicode characters, and case-insensitive matching.
; RemoveFromPath - Removes a directory from the system PATH
; Handles 64-bit registry, Unicode characters, and case-insensitive matching.
Function un.RemoveFromPath
  Exch $0 ; Target path
  Push $1 ; Full PATH 
  Push $2 ; Rebuilt PATH 
  Push $3 ; Current Segment 
  Push $4 ; Normalized Target 
  Push $5 ; Normalized Segment 
  Push $6 ; Loop Index 
  Push $7 ; Temp Char 
  Push $8 ; Log Handle (Unused but kept for stack balance)

  SetRegView 64
  
  ; Logging disabled for production
  ; StrCpy $8 "$TEMP\RemoveFromPath.log"
  ; FileOpen $8 $8 "w"
  ; FileWrite $8 "=== RemoveFromPath started ===$\r$\n"
  ; FileWrite $8 "Target to remove: '$0'$\r$\n"

  ; 1. Read Registry
  ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  ; FileWrite $8 "FULL REGISTRY PATH: '$1'$\r$\n"

  ; 2. Normalize Target
  StrCpy $4 $0
  StrLen $7 $4
  IntOp $7 $7 - 1
  StrCpy $6 $4 1 $7
  StrCmp $6 "\" 0 +2
    StrCpy $4 $4 $7 
  
  Push $4
  Call un.StrLower
  Pop $4 
  ; FileWrite $8 "Normalized Target: '$4'$\r$\n"

  ; SAFETY GUARD: Do not proceed if target is empty!
  StrCmp $4 "" 0 +2
    ; FileWrite $8 "ERROR: Normalized target is empty. Aborting to save PATH.$\r$\n"
    Goto done_cleanup

  StrCpy $2 "" 

loop_segments:
  StrCmp $1 "" done_loop
  StrCpy $6 0 
  StrCpy $3 "" 

find_semi:
  StrCpy $7 $1 1 $6 
  StrCmp $7 "" segment_found 
  StrCmp $7 ";" segment_found 
  IntOp $6 $6 + 1
  Goto find_semi

segment_found:
  StrCpy $3 $1 $6 
  IntOp $6 $6 + 1
  StrCpy $1 $1 "" $6 

  StrCmp $3 "" loop_segments ; Skip empty segments

  ; Normalize Segment
  StrCpy $5 $3
  StrLen $7 $5
  IntOp $7 $7 - 1
  StrCpy $6 $5 1 $7
  StrCmp $6 "\" 0 +2
    StrCpy $5 $5 $7
  
  Push $5
  Call un.StrLower
  Pop $5
  
  ; 3. Compare
  StrCmp $5 $4 is_match
    ; Result: NO MATCH - KEEPING
    StrCmp $2 "" 0 +3
      StrCpy $2 "$3" 
      Goto loop_segments
    StrCpy $2 "$2;$3" 
    Goto loop_segments

is_match:
  ; FileWrite $8 "MATCHED AND REMOVED: '$3'$\r$\n"
  Goto loop_segments ; Go back to loop!

done_loop:
  ; FileWrite $8 "FINAL REBUILT PATH: '$2'$\r$\n"

  ; 4. Final Safety Check & Write
  ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  
  ; Final sanity check: don't write an empty path if the original wasn't empty
  StrCmp $2 "" done_write 
  
  StrCmp $2 $1 done_write
    WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$2"
    ; FileWrite $8 "Registry successfully updated.$\r$\n"
  
done_write:
  ; FileWrite $8 "=== RemoveFromPath completed ===$\r$\n"

done_cleanup:
  ; FileClose $8
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  SetRegView 32

  Pop $8
  Pop $7
  Pop $6
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

; ---------------------------------------------------------
; Uninstaller version of TrimCRLF
; ---------------------------------------------------------
Function un.TrimCRLF
  Exch $R0 ; Get string from stack
  Push $R1 ; Save $R1
loop:
  StrCpy $R1 $R0 1 -1 ; Get last character
  StrCmp $R1 "$\r" trim
  StrCmp $R1 "$\n" trim
  StrCmp $R1 " "   trim ; Optional: trim trailing spaces too
  Goto done
trim:
  StrCpy $R0 $R0 -1    ; Remove last character
  Goto loop
done:
  Pop $R1  ; Restore $R1
  Exch $R0 ; Put trimmed string back on stack
FunctionEnd

; StrStr - Find substring in string
; Usage: Push "haystack"
;        Push "needle"
;        Call StrStr
;        Pop $0  ; Returns position or "" if not found
Function StrStr
	Exch $1  ; needle
	Exch
	Exch $2  ; haystack
	Push $3
	Push $4
	Push $5

	StrLen $3 $1
	StrCmp $3 0 notfound
	StrLen $4 $2
	StrCmp $4 0 notfound

	StrCpy $5 0
searchloop:
	IntCmp $5 $4 notfound notfound
	StrCpy $0 $2 $3 $5
	StrCmp $0 $1 found
	IntOp $5 $5 + 1
	Goto searchloop

found:
	StrCpy $0 $2 "" $5
	Goto done

notfound:
	StrCpy $0 ""

done:
	Pop $5
	Pop $4
	Pop $3
	Pop $2
	Pop $1
	Exch $0
FunctionEnd

; Uninstaller version of StrStr
Function un.StrStr
	Exch $1
	Exch
	Exch $2
	Push $3
	Push $4
	Push $5

	StrLen $3 $1
	StrCmp $3 0 notfound
	StrLen $4 $2
	StrCmp $4 0 notfound

	StrCpy $5 0
searchloop:
	IntCmp $5 $4 notfound notfound
	StrCpy $0 $2 $3 $5
	StrCmp $0 $1 found
	IntOp $5 $5 + 1
	Goto searchloop

found:
	StrCpy $0 $2 "" $5
	Goto done

notfound:
	StrCpy $0 ""

done:
	Pop $5
	Pop $4
	Pop $3
	Pop $2
	Pop $1
	Exch $0
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

Section "DisplayXR Runtime" SecRuntime
	SectionIn RO  ; Required section

	SetOutPath "$INSTDIR"

	; Install runtime files
	File "${BIN_DIR}\DisplayXRClient.dll"

	; Install service (needed for Chrome WebXR and other sandboxed apps)
	File /nonfatal "${BIN_DIR}\displayxr-service.exe"

	; Install manifest
	File "${OUTPUT_DIR}\DisplayXR_win64.json"

	; Install switcher if available
	File /nonfatal "${BIN_DIR}\DisplayXRSwitcher.exe"

	; Install runtime DLL dependencies
	File /nonfatal "${BIN_DIR}\*.dll"

	; Create AppData directories
	CreateDirectory "$APPDATA\DisplayXR"

	; Write registry keys
	WriteRegStr HKLM "Software\DisplayXR\Runtime" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\DisplayXR\Runtime" "Version" "${VERSION}"

	; Set as active OpenXR runtime
	SetRegView 64
	WriteRegStr HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime" "$INSTDIR\DisplayXR_win64.json"
	SetRegView 32

	; Add install directory to system PATH
	; This is needed so OpenXR apps can find DisplayXRClient.dll's dependencies
	; (vulkan-1.dll, SDL2.dll, etc.) when loading the runtime
	Push $INSTDIR
	Call AddToPath

	; Enable D3D11 native compositor by default
	; This bypasses Vulkan and avoids D3D11<->Vulkan interop issues on Intel GPUs
	WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
		"OXR_ENABLE_D3D11_NATIVE_COMPOSITOR" "1"

	; Broadcast environment change to running applications
	SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000

	; Write uninstaller
	WriteUninstaller "$INSTDIR\Uninstall.exe"

	; Add to Add/Remove Programs
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"DisplayName" "DisplayXR OpenXR Runtime"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"DisplayIcon" "$INSTDIR\DisplayXRClient.dll"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"Publisher" "DisplayXR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"NoRepair" 1

	; Calculate installed size
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR" \
		"EstimatedSize" "$0"

	; Auto-start displayxr-service at user logon via Run registry key.
	; Chrome's AppContainer sandbox blocks the OpenXR loader from launching the
	; service on-demand (ACCESS_DENIED), so we pre-launch it at logon (issue #68).
	; Uses HKLM Run key so it starts for all users (installer already requires admin).
	; Also remove any legacy scheduled task from older installs.
	IfFileExists "$INSTDIR\displayxr-service.exe" 0 skip_service_autostart
		; Remove legacy scheduled task if it exists (from pre-v25.0.1 installs)
		nsExec::ExecToLog 'schtasks /delete /tn "DisplayXR Service" /f'
		Pop $0

		; Register in HKLM Run key (starts in user session with GPU/tray access)
		DetailPrint "Registering DisplayXR Service for auto-start..."
		WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" \
			"DisplayXR Service" "$\"$INSTDIR\displayxr-service.exe$\""

		; Start the service immediately so it's available without relogon
		DetailPrint "Starting DisplayXR Service..."
		Exec '"$INSTDIR\displayxr-service.exe"'
	skip_service_autostart:

	; Save installation log to install directory
	StrCpy $0 "$INSTDIR\install.log"
	Push $0
	Call DumpLog

SectionEnd

Section "Start Menu Shortcuts" SecShortcuts
	CreateDirectory "$SMPROGRAMS\DisplayXR"

	; Add switcher shortcut if installed
	IfFileExists "$INSTDIR\DisplayXRSwitcher.exe" 0 +2
		CreateShortCut "$SMPROGRAMS\DisplayXR\DisplayXR Runtime Switcher.lnk" "$INSTDIR\DisplayXRSwitcher.exe"

	CreateShortCut "$SMPROGRAMS\DisplayXR\Uninstall DisplayXR.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Stop the displayxr-service and remove auto-start registration (issue #68)
	; Kill any running instance first so we can delete the exe
	DetailPrint "Stopping DisplayXR Service..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-service.exe'
	; Remove Run key auto-start
	DetailPrint "Removing DisplayXR Service auto-start..."
	DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "DisplayXR Service"
	; Also remove legacy scheduled task if it exists (from older installs)
	nsExec::ExecToLog 'schtasks /delete /tn "DisplayXR Service" /f'

	; Remove files
	Delete "$INSTDIR\DisplayXRClient.dll"
	Delete "$INSTDIR\displayxr-service.exe"
	Delete "$INSTDIR\DisplayXRSwitcher.exe"
	Delete "$INSTDIR\DisplayXR_win64.json"
	Delete "$INSTDIR\install.log"

	; Remove any remaining DLLs
	Delete "$INSTDIR\*.dll"

	; Save uninstall log to temp before removing directory
	StrCpy $0 "$TEMP\DisplayXR_uninstall.log"
	Push $0
	Call un.DumpLog

	; Remove uninstaller
	Delete "$INSTDIR\Uninstall.exe"

	; Remove install directory (if empty)
	RMDir "$INSTDIR"
	RMDir "$PROGRAMFILES64\DisplayXR"

	; Remove AppData directory
	RMDir "$APPDATA\DisplayXR"

	; Remove Start Menu shortcuts
	Delete "$SMPROGRAMS\DisplayXR\DisplayXR Runtime Switcher.lnk"
	Delete "$SMPROGRAMS\DisplayXR\Uninstall DisplayXR.lnk"
	RMDir "$SMPROGRAMS\DisplayXR"

	; Remove DisplayXR registry keys
	DeleteRegKey HKLM "Software\DisplayXR\Runtime"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR"

	; Remove from Add/Remove Programs
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR"

	; Only remove ActiveRuntime if it points to our manifest
	SetRegView 64
	ReadRegStr $0 HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"
	StrCmp $0 "$INSTDIR\DisplayXR_win64.json" 0 +2
		DeleteRegValue HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"
	SetRegView 32

	; Remove install directory from system PATH
	Push $INSTDIR
	Call un.RemoveFromPath

	; Remove D3D11 native compositor environment variable
	DeleteRegValue HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
		"OXR_ENABLE_D3D11_NATIVE_COMPOSITOR"

	; Broadcast environment change
	SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000

SectionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecRuntime} "DisplayXR OpenXR runtime files (required)"
	!insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Create Start Menu shortcuts"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
	; Check for 64-bit Windows
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "DisplayXR requires 64-bit Windows."
		Abort
	${EndIf}
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2024-2026 DisplayXR"
VIAddVersionKey "FileDescription" "DisplayXR OpenXR Runtime Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"