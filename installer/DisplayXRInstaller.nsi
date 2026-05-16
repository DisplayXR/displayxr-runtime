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
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!insertmacro un.WordFind

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
; White icon so the installer .exe is visible in dark mode Explorer/taskbar
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

; AddToPath — appends a directory to the system PATH if no segment of PATH
; already names it. Walks segments (split on ';') and compares with
; case-insensitive StrCmp ignoring a trailing backslash, so it's robust
; against the casing/trailing-slash variations old installer versions wrote.
;
; Replaces an older implementation that used StrStr (case-sensitive
; substring search) to dedupe, which mismatched whenever earlier installs
; had written the entry with different casing, accumulating duplicates over
; time. The previous implementation also used a REG_EXPAND_SZ-safe
; System::Call read of the registry; that part is preserved here.
;
; Usage: Push "C:\path\to\add"
;        Call AddToPath
Function AddToPath
	Exch $0  ; Path to add
	Push $1  ; Current PATH
	Push $2  ; Scratch (status / temp char)
	Push $3  ; Current segment
	Push $4  ; Target without trailing backslash
	Push $5  ; Current segment without trailing backslash
	Push $6  ; Scan index
	Push $7  ; Temp char / strlen scratch
	Push $8  ; System::Call buffer pointer
	Push $9  ; Registry handle

	SetRegView 64

	; Open the Environment registry key (KEY_READ).
	; 0x80000002 = HKEY_LOCAL_MACHINE, 0x20019 = KEY_READ
	System::Call 'Advapi32::RegOpenKeyExW(i 0x80000002, w "SYSTEM\CurrentControlSet\Control\Session Manager\Environment", i 0, i 0x20019, *i .r9) i .r2'
	StrCmp $2 0 0 atp_reg_fallback

	System::Call 'Advapi32::RegQueryValueExW(i r9, w "Path", i 0, i 0, i 0, *i .r6) i .r2'
	StrCmp $2 0 0 atp_reg_close_fallback

	System::Alloc $6
	Pop $8
	System::Call 'Advapi32::RegQueryValueExW(i r9, w "Path", i 0, i 0, i r8, *i r6) i .r2'
	StrCmp $2 0 0 atp_reg_free_fallback

	System::Call '*$8(&w${NSIS_MAX_STRLEN} .r1)'
	System::Free $8
	System::Call 'Advapi32::RegCloseKey(i r9)'
	Goto atp_got_path

atp_reg_free_fallback:
	System::Free $8
atp_reg_close_fallback:
	System::Call 'Advapi32::RegCloseKey(i r9)'
atp_reg_fallback:
	ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"

atp_got_path:
	; Defensive fallback: if PATH read returned empty (a real environment
	; never has an empty PATH), try the process env so we don't overwrite
	; PATH with just our entry. If that's also empty, treat as fresh-system
	; and just write our path.
	StrCmp $1 "" atp_try_expanded atp_normalize_target
atp_try_expanded:
	ReadEnvStr $1 PATH
	StrCmp $1 "" atp_empty_path atp_normalize_target

atp_normalize_target:
	; Strip a single trailing backslash from the target so we don't add
	; "C:\foo" when PATH already has "C:\foo\" (or vice versa).
	StrCpy $4 $0
	StrLen $7 $4
	IntOp $7 $7 - 1
	StrCpy $6 $4 1 $7
	StrCmp $6 "\" 0 +2
		StrCpy $4 $4 $7

	StrCmp $4 "" atp_done  ; refuse to append an empty target

	; Walk PATH segments — if any matches the target (case-insensitive,
	; ignoring trailing backslash), do not append.
	StrCpy $2 $1  ; $2 = remaining unscanned PATH

atp_loop:
	StrCmp $2 "" atp_append  ; ran out of segments, no match — append
	StrCpy $6 0
	StrCpy $3 ""
atp_find_semi:
	StrCpy $7 $2 1 $6
	StrCmp $7 "" atp_seg_end
	StrCmp $7 ";" atp_seg_end
	IntOp $6 $6 + 1
	Goto atp_find_semi
atp_seg_end:
	StrCpy $3 $2 $6
	IntOp $6 $6 + 1
	StrCpy $2 $2 "" $6
	StrCmp $3 "" atp_loop  ; skip empty segments

	; Strip trailing backslash from the segment for comparison
	StrCpy $5 $3
	StrLen $7 $5
	IntOp $7 $7 - 1
	StrCpy $6 $5 1 $7
	StrCmp $6 "\" 0 +2
		StrCpy $5 $5 $7

	StrCmp $5 $4 atp_already_present  ; case-insensitive
	Goto atp_loop

atp_already_present:
	DetailPrint "Path already exists in system PATH, skipping"
	Goto atp_done

atp_empty_path:
	; Truly empty PATH on a fresh system — write just our entry.
	StrCpy $1 ""

atp_append:
	StrCmp $1 "" 0 +3
		StrCpy $0 "$0"
		Goto atp_write
	StrCpy $0 "$1;$0"
atp_write:
	WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$0"
	DetailPrint "Added to system PATH"

atp_done:
	SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
	SetRegView 32
	Pop $9
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

; un.RemoveFromPath — removes every segment of the system PATH that matches
; the target directory. Case-insensitive (NSIS StrCmp is case-insensitive by
; default), trailing-backslash-tolerant, REG_EXPAND_SZ-safe. Replaces the
; older implementation which called a broken un.StrLower helper that mangled
; the normalized-target string via a misused CharLowerW system call.
;
; Usage: Push "C:\path\to\remove"
;        Call un.RemoveFromPath
Function un.RemoveFromPath
  Exch $0 ; Target path
  Push $1 ; Full PATH
  Push $2 ; Rebuilt PATH
  Push $3 ; Current Segment
  Push $4 ; Target without trailing backslash
  Push $5 ; Current Segment without trailing backslash
  Push $6 ; Scan index
  Push $7 ; Temp char / strlen scratch
  Push $8 ; System::Call buffer pointer
  Push $9 ; Registry handle

  SetRegView 64

  ; --- Read PATH via System::Call (REG_EXPAND_SZ-safe; mirrors AddToPath).
  ; Plain ReadRegStr can return empty on REG_EXPAND_SZ values and truncates
  ; PATHs longer than NSIS_MAX_STRLEN. Both leave $1 empty, the loop below
  ; produces an empty rebuilt $2, and the safety guard then skips the write
  ; so dupes accumulate forever. We do still fall back to ReadRegStr if the
  ; System::Call path errors out, with the same safety guard catching the
  ; truncated-string case.
  ; 0x80000002 = HKEY_LOCAL_MACHINE, 0x20019 = KEY_READ
  System::Call 'Advapi32::RegOpenKeyExW(i 0x80000002, w "SYSTEM\CurrentControlSet\Control\Session Manager\Environment", i 0, i 0x20019, *i .r9) i .r2'
  StrCmp $2 0 0 rfp_reg_fallback

  System::Call 'Advapi32::RegQueryValueExW(i r9, w "Path", i 0, i 0, i 0, *i .r6) i .r2'
  StrCmp $2 0 0 rfp_reg_close_fallback

  System::Alloc $6
  Pop $8
  System::Call 'Advapi32::RegQueryValueExW(i r9, w "Path", i 0, i 0, i r8, *i r6) i .r2'
  StrCmp $2 0 0 rfp_reg_free_fallback

  System::Call '*$8(&w${NSIS_MAX_STRLEN} .r1)'
  System::Free $8
  System::Call 'Advapi32::RegCloseKey(i r9)'
  Goto rfp_got_path

rfp_reg_free_fallback:
  System::Free $8
rfp_reg_close_fallback:
  System::Call 'Advapi32::RegCloseKey(i r9)'
rfp_reg_fallback:
  ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"

rfp_got_path:

  ; Strip a single trailing backslash from the target so "C:\foo" matches
  ; "C:\foo\" segments. StrCmp below is case-insensitive in NSIS — no
  ; lowercasing required.
  StrCpy $4 $0
  StrLen $7 $4
  IntOp $7 $7 - 1
  StrCpy $6 $4 1 $7
  StrCmp $6 "\" 0 +2
    StrCpy $4 $4 $7

  StrCmp $4 "" done_cleanup
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

  StrCmp $3 "" loop_segments  ; Skip empty segments

  ; Strip trailing backslash for the case-insensitive comparison
  StrCpy $5 $3
  StrLen $7 $5
  IntOp $7 $7 - 1
  StrCpy $6 $5 1 $7
  StrCmp $6 "\" 0 +2
    StrCpy $5 $5 $7

  StrCmp $5 $4 is_match  ; case-insensitive

  ; Keep
  StrCmp $2 "" 0 +3
    StrCpy $2 "$3"
    Goto loop_segments
  StrCpy $2 "$2;$3"
  Goto loop_segments

is_match:
  Goto loop_segments  ; Drop

done_loop:
  ; Re-read original PATH and only write if we actually changed it.
  ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  StrCmp $2 "" done_cleanup  ; rebuilt empty — refuse to wipe PATH
  StrCmp $2 $1 done_cleanup  ; unchanged
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$2"

done_cleanup:
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  SetRegView 32

  Pop $9
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

	; Force 64-bit registry view so HKLM\Software\DisplayXR\* lands in the
	; non-redirected view (NSIS is 32-bit and would otherwise redirect into
	; HKLM\Software\WOW6432Node\DisplayXR\*, breaking the contract with the
	; 64-bit service which reads via KEY_WOW64_64KEY).
	SetRegView 64

	SetOutPath "$INSTDIR"

	; Kill any running DisplayXR processes before overwriting (avoids write/sharing error).
	; Workspace controllers (e.g. the DisplayXR shell) are installed as separate
	; products and have their own installers; we don't reach into their processes
	; here. The uninstall path runs cascade-uninstall first which handles their
	; lifecycle.
	nsExec::ExecToLog 'taskkill /f /im displayxr-service.exe'
	Pop $0
	; Wait for processes to fully exit and release pipe/file handles
	Sleep 2000

	; Install runtime files
	File "${BIN_DIR}\DisplayXRClient.dll"

	; Install service (needed for Chrome WebXR and other sandboxed apps)
	File /nonfatal "${BIN_DIR}\displayxr-service.exe"

	; Install manifest
	File "${OUTPUT_DIR}\DisplayXR_win64.json"

	; Workspace controllers (the DisplayXR shell, third-party verticals, …)
	; ship as their own products with dedicated installers — see
	; docs/specs/workspace-controller-registration.md. The runtime owns no
	; specific workspace app and has nothing to install here.

	; Install WebXR Bridge v2 host (metadata sideband for Chrome's native WebXR, issue #139)
	File /nonfatal "${BIN_DIR}\displayxr-webxr-bridge.exe"

	; Install switcher if available
	File /nonfatal "${BIN_DIR}\DisplayXRSwitcher.exe"

	; Install runtime DLL dependencies (exclude vulkan-1.dll — the system copy
	; in SYSTEM32 is sufficient, and shipping our own risks version conflicts
	; and interaction with third-party Vulkan implicit layers. See issue #105.)
	File /nonfatal /x "vulkan-1.dll" "${BIN_DIR}\*.dll"

	; Create AppData directories
	CreateDirectory "$APPDATA\DisplayXR"

	; Write registry keys
	WriteRegStr HKLM "Software\DisplayXR\Runtime" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\DisplayXR\Runtime" "Version" "${VERSION}"

	; Set as active OpenXR runtime (still in 64-bit view from section-start
	; SetRegView; explicit no-op kept for clarity).
	WriteRegStr HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime" "$INSTDIR\DisplayXR_win64.json"

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

	; Workspace controllers add their own Start Menu shortcuts from their
	; respective installers.

	; Add switcher shortcut if installed
	IfFileExists "$INSTDIR\DisplayXRSwitcher.exe" 0 +2
		CreateShortCut "$SMPROGRAMS\DisplayXR\DisplayXR Runtime Switcher.lnk" "$INSTDIR\DisplayXRSwitcher.exe"

	CreateShortCut "$SMPROGRAMS\DisplayXR\Uninstall DisplayXR.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Same 64-bit view as the install section — see Section "DisplayXR Runtime".
	SetRegView 64

	; -----------------------------------------------------------------
	; Cascade-uninstall registered workspace controllers FIRST.
	;
	; Each workspace app (the DisplayXR shell, third-party verticals, …)
	; has registered its UninstallString at
	; HKLM\Software\DisplayXR\WorkspaceControllers\<id>. Run each one
	; silently before we touch the runtime files; their installers own
	; their own files and registry entries.
	;
	; We collect all UninstallStrings up-front into a pipe-delimited
	; buffer ($R0) because each chained uninstaller deletes its own
	; subkey, so EnumRegKey indices would shift mid-iteration.
	; -----------------------------------------------------------------
	DetailPrint "Discovering registered workspace controllers..."
	StrCpy $R0 ""
	StrCpy $9 0
	cascade_collect_loop:
		EnumRegKey $1 HKLM "Software\DisplayXR\WorkspaceControllers" $9
		StrCmp $1 "" cascade_collect_done
		ReadRegStr $2 HKLM "Software\DisplayXR\WorkspaceControllers\$1" "UninstallString"
		${If} $2 != ""
			${If} $R0 == ""
				StrCpy $R0 "$2"
			${Else}
				StrCpy $R0 "$R0|$2"
			${EndIf}
		${EndIf}
		IntOp $9 $9 + 1
		Goto cascade_collect_loop
	cascade_collect_done:

	${If} $R0 != ""
		${un.WordFind} "$R0" "|" "#" $R9
		StrCpy $R8 1
		cascade_run_loop:
			IntCmp $R8 $R9 0 0 cascade_run_done
			${un.WordFind} "$R0" "|" "+$R8" $R7
			${If} $R7 != ""
				DetailPrint "Uninstalling workspace controller: $R7"
				nsExec::ExecToLog '"$R7" /S'
				Pop $5
			${EndIf}
			IntOp $R8 $R8 + 1
			Goto cascade_run_loop
		cascade_run_done:
	${EndIf}

	; Drop the parent key (cleans any orphan entries whose uninstallers
	; failed to remove themselves).
	DeleteRegKey HKLM "Software\DisplayXR\WorkspaceControllers"
	; -----------------------------------------------------------------

	; Stop the displayxr-service and remove auto-start registration (issue #68)
	; Kill any running instance first so we can delete the exe
	DetailPrint "Stopping DisplayXR Service..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-service.exe'
	; Remove Run key auto-start
	DetailPrint "Removing DisplayXR Service auto-start..."
	DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "DisplayXR Service"
	; Also remove legacy scheduled task if it exists (from older installs)
	nsExec::ExecToLog 'schtasks /delete /tn "DisplayXR Service" /f'

	; Remove files. We name the high-value executables explicitly (so a
	; failed Delete prints a clear log line), then wildcard-sweep the rest
	; — including unowned bins that landed via cmake --install (cli, mcp,
	; gui) that the NSI doesn't directly bundle but which accumulate in
	; $INSTDIR across versions if we don't sweep them.
	Delete "$INSTDIR\DisplayXRClient.dll"
	Delete "$INSTDIR\displayxr-service.exe"
	Delete "$INSTDIR\displayxr-webxr-bridge.exe"
	Delete "$INSTDIR\DisplayXRSwitcher.exe"
	Delete "$INSTDIR\DisplayXR_win64.json"
	Delete "$INSTDIR\install.log"

	; Sweep remaining executables, DLLs, and manifests so RMDir can succeed
	Delete "$INSTDIR\*.exe"
	Delete "$INSTDIR\*.dll"
	Delete "$INSTDIR\*.json"

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

	; Remove Start Menu shortcuts (workspace-controller shortcuts are
	; removed by their own cascade-uninstaller; nothing left to clean here)
	Delete "$SMPROGRAMS\DisplayXR\DisplayXR Runtime Switcher.lnk"
	Delete "$SMPROGRAMS\DisplayXR\Uninstall DisplayXR.lnk"
	RMDir "$SMPROGRAMS\DisplayXR"

	; Remove DisplayXR registry keys
	DeleteRegKey HKLM "Software\DisplayXR\Runtime"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR"

	; Remove from Add/Remove Programs
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXR"

	; Only remove ActiveRuntime if it points to our manifest (still in 64-bit view).
	ReadRegStr $0 HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"
	StrCmp $0 "$INSTDIR\DisplayXR_win64.json" 0 +2
		DeleteRegValue HKLM "Software\Khronos\OpenXR\1" "ActiveRuntime"

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