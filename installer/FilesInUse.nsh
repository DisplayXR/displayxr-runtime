; FilesInUse.nsh -- name the programs holding the runtime DLL, and close them
; gracefully on request (#1733).
;
; The #1268 guard in DisplayXRInstaller.nsi refuses to touch a single file while
; anything still has DisplayXRClient.dll mapped. That refusal is right. What it
; could not do was say WHO: the message was "close every OpenXR application",
; and under the bundle (/S) even that was suppressed, leaving a bare exit code.
; The holder that actually bit (#1733) was Google Chrome's WebXR utility
; process, which nobody thinks of as an "OpenXR application".
;
; The Windows Restart Manager answers "who has this file open" with each
; holder's PID and friendly name ("Google Chrome"), across bitness -- this
; installer is 32-bit, the holders are 64-bit, and RM does not care. It is a
; plain rstrtmgr.dll export set, so System::Call is enough; no NSIS plug-in.
;
; RM EXPLAINS the lock; it does not prove anything. ClientDllWritable (the
; FileOpen-for-append test) stays the only proof that the file can be written,
; and every path below loops back to it.
;
; Deliberately NOT done: RmRestart after the install. This installer runs
; elevated, and relaunching the user's browser from here would hand it a
; High-integrity token. Chrome restores its own tabs on the next normal launch.

!ifndef DXR_FILES_IN_USE_NSH
!define DXR_FILES_IN_USE_NSH

!include "LogicLib.nsh"

; sizeof(RM_PROCESS_INFO): RM_UNIQUE_PROCESS (DWORD pid + FILETIME = 12) +
; WCHAR strAppName[256] (512) + WCHAR strServiceShortName[64] (128) +
; ApplicationType, AppStatus, TSSessionId, bRestartable (4 x 4 = 16) = 668.
; Every member is 4-byte aligned, so there is no padding.
!define DXR_RM_PROCESS_INFO_SIZE 668
!define DXR_RM_ERROR_MORE_DATA 234
; Cap the list so a pathological box cannot produce an unreadable dialog.
!define DXR_RM_MAX_LISTED 10

; Restart Manager session handle, or -1 when no session is open. The session
; is kept open between FilesInUseList and FilesInUseClose so the shutdown acts
; on exactly the processes the user was shown.
Var DxrRmSession

; Open a session and register DisplayXRClient.dll with it.
; Out: $DxrRmSession (-1 on failure).
Function DxrRmOpen
	Push $0
	Push $1
	Push $2
	Push $3
	Push $4

	StrCpy $DxrRmSession -1

	; strSessionKey is an out buffer of CCH_RM_SESSION_KEY+1 (33) WCHARs; a
	; System `w` out-parameter is NSIS_MAX_STRLEN long, which is ample.
	System::Call 'rstrtmgr::RmStartSession(*i .r0, i 0, w .r1) i .r2'
	${If} $2 != 0
		DetailPrint "Restart Manager unavailable (RmStartSession error $2)."
		Goto dxr_rm_open_done
	${EndIf}

	; rgsFileNames is an array of LPCWSTR: one inline wide-string buffer, then
	; a one-element pointer array pointing at it.
	System::Call '*(&w${NSIS_MAX_STRLEN} "$INSTDIR\DisplayXRClient.dll") p .r3'
	System::Call '*(p r3) p .r4'
	System::Call 'rstrtmgr::RmRegisterResources(i r0, i 1, p r4, i 0, p 0, i 0, p 0) i .r2'
	System::Free $4
	System::Free $3
	${If} $2 != 0
		DetailPrint "Restart Manager could not register DisplayXRClient.dll (error $2)."
		System::Call 'rstrtmgr::RmEndSession(i r0)'
		Goto dxr_rm_open_done
	${EndIf}

	StrCpy $DxrRmSession $0

dxr_rm_open_done:
	Pop $4
	Pop $3
	Pop $2
	Pop $1
	Pop $0
FunctionEnd

; End the session if one is open. Safe to call at any time.
Function FilesInUseEnd
	${If} $DxrRmSession != -1
	${AndIf} $DxrRmSession != ""
		System::Call 'rstrtmgr::RmEndSession(i $DxrRmSession)'
	${EndIf}
	StrCpy $DxrRmSession -1
FunctionEnd

; List the processes holding DisplayXRClient.dll.
; Out: $R1 = one "  - Name (PID n)" line per holder, CRLF-separated ("" if none
;            could be identified); $R2 = number of holders found.
; Leaves the session open for FilesInUseClose; call FilesInUseEnd when done.
Function FilesInUseList
	Push $0
	Push $2
	Push $3
	Push $4
	Push $5
	Push $6
	Push $7
	Push $8
	Push $9

	StrCpy $R1 ""
	StrCpy $R2 0

	Call FilesInUseEnd
	Call DxrRmOpen
	StrCmp $DxrRmSession -1 dxr_rm_list_done

	; Size query: nProcInfo = 0 returns ERROR_MORE_DATA and the count needed
	; (or success with 0 when nothing holds the file).
	System::Call 'rstrtmgr::RmGetList(i $DxrRmSession, *i .r5, *i 0 r6, p 0, *i .r7) i .r2'
	${If} $2 == 0
		Goto dxr_rm_list_done
	${EndIf}
	${If} $2 != ${DXR_RM_ERROR_MORE_DATA}
		DetailPrint "Restart Manager could not list the holders (RmGetList error $2)."
		Goto dxr_rm_list_done
	${EndIf}

	; The set can grow between the two calls; leave headroom and retry once
	; more on a second ERROR_MORE_DATA rather than loop forever.
	IntOp $6 $5 + 4
	IntOp $3 $6 * ${DXR_RM_PROCESS_INFO_SIZE}
	System::Alloc $3
	Pop $4
	System::Call 'rstrtmgr::RmGetList(i $DxrRmSession, *i .r5, *i r6 r6, p r4, *i .r7) i .r2'
	${If} $2 == ${DXR_RM_ERROR_MORE_DATA}
		System::Free $4
		IntOp $6 $5 + 16
		IntOp $3 $6 * ${DXR_RM_PROCESS_INFO_SIZE}
		System::Alloc $3
		Pop $4
		System::Call 'rstrtmgr::RmGetList(i $DxrRmSession, *i .r5, *i r6 r6, p r4, *i .r7) i .r2'
	${EndIf}
	${If} $2 != 0
		DetailPrint "Restart Manager could not list the holders (RmGetList error $2)."
		System::Free $4
		Goto dxr_rm_list_done
	${EndIf}

	; $6 now holds the number of RM_PROCESS_INFO entries filled in.
	StrCpy $R2 $6
	StrCpy $0 0
	${DoWhile} $0 < $6
		${If} $0 >= ${DXR_RM_MAX_LISTED}
			IntOp $9 $6 - $0
			StrCpy $R1 "$R1$\r$\n  - ...and $9 more"
			${Break}
		${EndIf}
		IntOp $3 $0 * ${DXR_RM_PROCESS_INFO_SIZE}
		IntPtrOp $3 $4 + $3
		; Process.dwProcessId, Process.ProcessStartTime (2 DWORDs), strAppName.
		System::Call '*$3(i .r8, i, i, &w256 .r9)'
		${If} $9 == ""
			StrCpy $9 "Unknown program"
		${EndIf}
		${If} $R1 != ""
			StrCpy $R1 "$R1$\r$\n"
		${EndIf}
		StrCpy $R1 "$R1  - $9 (PID $8)"
		IntOp $0 $0 + 1
	${Loop}
	System::Free $4

dxr_rm_list_done:
	Pop $9
	Pop $8
	Pop $7
	Pop $6
	Pop $5
	Pop $4
	Pop $3
	Pop $2
	Pop $0
FunctionEnd

; Ask the holders from the open session to close NORMALLY: no force flag, so
; an app with unsaved work can refuse, and one that does not answer is left
; running (the caller re-checks the DLL either way). This is the same request
; Windows Update and MSI's FilesInUse dialog send -- never a TerminateProcess.
; Out: $R0 = RmShutdown result (0 = every holder closed).
Function FilesInUseClose
	StrCpy $R0 -1
	${If} $DxrRmSession != -1
	${AndIf} $DxrRmSession != ""
		System::Call 'rstrtmgr::RmShutdown(i $DxrRmSession, i 0, p 0) i .R0'
		DetailPrint "Asked the programs to close (RmShutdown result $R0)."
	${EndIf}
FunctionEnd

!endif ; DXR_FILES_IN_USE_NSH
