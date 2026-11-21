; Copyright 2026, The DisplayXR Project
; SPDX-License-Identifier: BSL-1.0
;
; Catalog-driven optional components for the runtime installer (ADR-064 §8).
;
; The installer carries the runtime core only. Optional components (a display
; processor for the connected display, conversion providers, input providers,
; the browser ...) are LISTED from the signed DisplayXR Catalog and DOWNLOADED
; by dxr-get when chosen. Nothing here verifies or runs a payload itself:
; dxr-get does (docs/specs/runtime/dxr-get.md), and this file only asks it
; what exists and then asks it to install what was ticked.
;
; Pieces, in the order the main script uses them:
;
;   !include Catalog.nsh             early: declares the variables
;   DXR_CATALOG_PAGE_DEFINES         before MUI_PAGE_COMPONENTS / MUI_PAGE_FINISH
;   DXR_CATALOG_SECTIONS             right after the runtime section
;   DXR_CATALOG_DESCRIPTIONS         inside MUI_FUNCTION_DESCRIPTION_BEGIN/END
;   DXR_CATALOG_FUNCTIONS            after every section
;   Call DxrCatInit                  in .onInit
;   DXR_CATALOG_UNINSTALL            in the uninstall section
;
; WHAT IS LISTED. One call, `dxr-get list --json --assume-runtime <this
; version> --assume-abi ...`: every catalog item with its state, whether it is
; recommended, whether this machine's hardware matches it, what would be
; downloaded, and whether it is compatible with the runtime being installed.
; catalog_plan.ps1 turns that into an INI file (NSIS has no JSON parser) and
; decides nothing itself beyond the ticks. The component list is a fixed
; set of hidden "slot" sections: NSIS sections exist at compile time, so each
; listed item takes one slot, gets its text and flags at run time, and unused
; slots stay hidden (a section with an empty name is not shown).
;
; PRE-TICKED: an item the catalog marks `recommended` (the open conversion
; provider, so "Convert to 3D" exists on every machine) and an item whose
; detection rule matched this machine with high confidence (the display
; processor of the connected display). Detection is dxr-get's; nothing here
; reads an EDID. An item already at the catalog version is shown as installed,
; greyed, and is not downloaded.
;
; NO CONNECTION. The core install never depends on any of this. If the catalog
; cannot be reached the page says so and offers nothing; if a download fails
; later the item is reported on the finish page and in
; $INSTDIR\catalog-skipped.txt, and Setup still succeeds (exit code 0).
;
; SILENT (/S). Nothing is listed, fetched or installed unless the caller asks:
;
;   /COMPONENTS=<list>   comma-separated item ids (or their aliases, e.g. the
;                        versions.json names) and the keywords `recommended`,
;                        `detected`, `default` (= recommended,detected), `none`.
;                        Interactive: replaces the default pre-ticks.
;                        Unattended, exactly `/COMPONENTS=default` is one
;                        `dxr-get install --default` (what the bundle passes):
;                        no listing, no PowerShell.
;   /CATALOG=<url|file>  another catalog, passed to dxr-get as --catalog. It
;                        must still be signed (dxr-get's rule), so this grants
;                        nothing; it is for testing.
;
; So an unattended install with no switch does exactly what it did before this
; file existed, which also means a runtime installer that dxr-get itself runs
; (`/S /NOSTART`) never calls back into dxr-get.
;
; NEVER THE RUNTIME. Every install this file triggers passes `--no-runtime`:
; dxr-get must not start a second runtime installer from inside this one. An
; item that needs a newer runtime than this one is refused (exit 41) and
; reported as skipped.
;
; WHEN IT IS NOT THERE. If dxr-get.exe was not built, DXR_CATALOG_UI is
; undefined and every macro below expands to nothing: the installer is what it
; was before this file. If the listing cannot be produced at run time
; (PowerShell blocked by policy, dxr-get failing), no slot is shown and the
; page looks as it did; an id given explicitly with /COMPONENTS= is still
; handed to `dxr-get install`.

!ifndef DXR_CATALOG_NSH
!define DXR_CATALOG_NSH

!include "Sections.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

; Slots in the component list. More listed items than slots: the ticked ones
; are kept and the page says how many are not shown.
!define DXR_CAT_SLOTS 8

; IDC_INTROTEXT of the components page (the text above the list).
!define DXR_CAT_IDC_TOP 1006

!define DXR_CAT_TOP_DEFAULT "Check the components you want to install and uncheck the components you don't want to install. Click Install to start."

!ifndef INNER
	!ifdef BIN_DIR
		!if /FileExists "${BIN_DIR}\dxr-get.exe"
			!define DXR_CATALOG_UI
		!endif
	!endif
!endif

; The class ABI majors of the runtime this installer carries, as
; "class=N,class=N,": the numbers the main script writes to Runtime\Abi
; (installer/CMakeLists.txt reads them from the class headers). dxr-get is told
; them with --assume-abi, since the registry still describes the OLD runtime
; when the page is shown. A define that is missing is simply not passed.
!ifdef ABI_DISPLAY_PROCESSOR
	!define DXR_CAT_ABI_DP "display_processor=${ABI_DISPLAY_PROCESSOR},"
!else
	!define DXR_CAT_ABI_DP ""
!endif
!ifdef ABI_INPUT_PROVIDER
	!define DXR_CAT_ABI_IN "input_provider=${ABI_INPUT_PROVIDER},"
!else
	!define DXR_CAT_ABI_IN ""
!endif
!ifdef ABI_CONVERSION_PROVIDER
	!define DXR_CAT_ABI_CV "conversion_provider=${ABI_CONVERSION_PROVIDER},"
!else
	!define DXR_CAT_ABI_CV ""
!endif
!ifdef ABI_TRACKING_PROVIDER
	!define DXR_CAT_ABI_TR "tracking_provider=${ABI_TRACKING_PROVIDER},"
!else
	!define DXR_CAT_ABI_TR ""
!endif
!define DXR_CAT_ABI "${DXR_CAT_ABI_DP}${DXR_CAT_ABI_IN}${DXR_CAT_ABI_CV}${DXR_CAT_ABI_TR}"

; Declared in every build: the page texts below name them.
Var DxrCatTop          ; text above the component list
Var DxrCatFinishExtra  ; appended to the finish page text
!ifdef DXR_CATALOG_UI
Var DxrCatState        ; "" = not asked yet | ok | offline | error
Var DxrCatCount        ; listed items (<= DXR_CAT_SLOTS)
Var DxrCatIni          ; catalog_plan.ps1 output
Var DxrCatSel          ; /COMPONENTS= value
Var DxrCatUrl          ; /CATALOG= value
Var DxrCatShown        ; 1 once the components page was shown
Var DxrCatOffline      ; 1 once dxr-get reported no connection during install
Var DxrCatSkipped      ; "name: reason" lines of what was not installed
Var DxrCatExe          ; the dxr-get.exe the install step runs
Var DxrCatDesc1
Var DxrCatDesc2
Var DxrCatDesc3
Var DxrCatDesc4
Var DxrCatDesc5
Var DxrCatDesc6
Var DxrCatDesc7
Var DxrCatDesc8
!endif

;--------------------------------
; Page texts. Variables, so the text can say what the catalog answered.
!macro DXR_CATALOG_PAGE_DEFINES
	!define MUI_COMPONENTSPAGE_TEXT_TOP "$DxrCatTop"
	!ifdef DXR_CATALOG_UI
		!define MUI_PAGE_CUSTOMFUNCTION_PRE DxrCatComponentsPre
		!define MUI_PAGE_CUSTOMFUNCTION_SHOW DxrCatComponentsShow
	!endif
!macroend

!macro DXR_CATALOG_FINISH_DEFINES
	!define MUI_FINISHPAGE_TEXT "$(MUI_TEXT_FINISH_INFO_TEXT)$DxrCatFinishExtra"
!macroend

;--------------------------------
; Sections: the slots, then the hidden section that installs what was chosen.
; The slots MUST stay consecutive: slot N is section index DxrCatSlot1 + N - 1.
!macro DXR_CATALOG_SECTIONS
!ifdef DXR_CATALOG_UI
	Section /o "" DxrCatSlot1
	SectionEnd
	Section /o "" DxrCatSlot2
	SectionEnd
	Section /o "" DxrCatSlot3
	SectionEnd
	Section /o "" DxrCatSlot4
	SectionEnd
	Section /o "" DxrCatSlot5
	SectionEnd
	Section /o "" DxrCatSlot6
	SectionEnd
	Section /o "" DxrCatSlot7
	SectionEnd
	Section /o "" DxrCatSlot8
	SectionEnd

	; Runs after the runtime section (dxr-get, the registry Version value and
	; the service are in place) and before the conversion-models section, which
	; prefers the pack of an installed open conversion provider.
	Section "-Catalog components" SecDxrCatalog
		Call DxrCatInstallChosen
	SectionEnd
!endif
!macroend

!macro DXR_CATALOG_DESCRIPTIONS
!ifdef DXR_CATALOG_UI
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot1} "$DxrCatDesc1"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot2} "$DxrCatDesc2"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot3} "$DxrCatDesc3"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot4} "$DxrCatDesc4"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot5} "$DxrCatDesc5"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot6} "$DxrCatDesc6"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot7} "$DxrCatDesc7"
	!insertmacro MUI_DESCRIPTION_TEXT ${DxrCatSlot8} "$DxrCatDesc8"
!endif
!macroend

!macro DXR_CATALOG_UNINSTALL
	Delete "$INSTDIR\catalog-skipped.txt"
!macroend

;--------------------------------
; Functions. Inserted after the sections, because they name section indices.
!macro DXR_CATALOG_FUNCTIONS

; .onInit: defaults and the two switches. Asks nothing of the network.
Function DxrCatInit
	StrCpy $DxrCatTop "${DXR_CAT_TOP_DEFAULT}"
	StrCpy $DxrCatFinishExtra ""
!ifdef DXR_CATALOG_UI
	StrCpy $DxrCatState ""
	StrCpy $DxrCatCount 0
	StrCpy $DxrCatShown 0
	StrCpy $DxrCatOffline 0
	StrCpy $DxrCatSkipped ""
	StrCpy $DxrCatIni ""
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/COMPONENTS=" $DxrCatSel
	${If} ${Errors}
		StrCpy $DxrCatSel ""
	${EndIf}
	ClearErrors
	${GetOptions} $R0 "/CATALOG=" $DxrCatUrl
	${If} ${Errors}
		StrCpy $DxrCatUrl ""
	${EndIf}
	ClearErrors
	; Both values end up inside a quoted argument: refuse one that could end it.
	Push $DxrCatSel
	Call DxrCatNoQuote
	Pop $DxrCatSel
	Push $DxrCatUrl
	Call DxrCatNoQuote
	Pop $DxrCatUrl
!endif
FunctionEnd

!ifdef DXR_CATALOG_UI

; in: a string on the stack. out: the same string, or "" if it has a double quote.
Function DxrCatNoQuote
	Exch $0
	Push $1
	Push $2
	StrCpy $1 0
	dxr_cat_nq_loop:
		StrCpy $2 $0 1 $1
		StrCmp $2 "" dxr_cat_nq_done
		StrCmp $2 '"' dxr_cat_nq_bad
		IntOp $1 $1 + 1
		Goto dxr_cat_nq_loop
	dxr_cat_nq_bad:
		StrCpy $0 ""
	dxr_cat_nq_done:
	Pop $2
	Pop $1
	Exch $0
FunctionEnd

; Ask dxr-get what the catalog offers for this machine. Once per run.
; Leaves $DxrCatState, $DxrCatCount and the INI file. Scratch: $R0-$R3.
Function DxrCatQuery
	${If} $DxrCatState != ""
		Return
	${EndIf}
	StrCpy $DxrCatState "error"
	StrCpy $DxrCatCount 0
	InitPluginsDir
	; The page is shown before anything is installed, so the listing runs from
	; the copy of dxr-get this installer carries.
	File "/oname=$PLUGINSDIR\dxr-get.exe" "${BIN_DIR}\dxr-get.exe"
	File "/oname=$PLUGINSDIR\catalog_plan.ps1" "${__FILEDIR__}\catalog_plan.ps1"
	StrCpy $DxrCatIni "$PLUGINSDIR\catalog.ini"
	Delete "$DxrCatIni"

	StrCpy $R0 '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\catalog_plan.ps1" -DxrGet "$PLUGINSDIR\dxr-get.exe" -Out "$DxrCatIni" -RuntimeVersion ${VERSION} -Slots ${DXR_CAT_SLOTS}'
	; An empty quoted argument is dropped by powershell -File, so add these
	; only when they have a value.
	!if "${DXR_CAT_ABI}" != ""
		StrCpy $R0 '$R0 -AssumeAbi "${DXR_CAT_ABI}"'
	!endif
	${If} $DxrCatSel != ""
		StrCpy $R0 '$R0 -Select "$DxrCatSel"'
	${EndIf}
	${If} $DxrCatUrl != ""
		StrCpy $R0 '$R0 -Catalog "$DxrCatUrl"'
	${EndIf}
	nsExec::ExecToStack /TIMEOUT=90000 $R0
	Pop $R1
	Pop $R2
	ClearErrors
	ReadINIStr $R3 "$DxrCatIni" "catalog" "state"
	${If} ${Errors}
		; No file: PowerShell is missing, blocked by policy, or timed out.
		DetailPrint "Catalog: the component listing did not run ($R1); optional components are not offered."
		ClearErrors
		Return
	${EndIf}
	StrCpy $DxrCatState $R3
	ReadINIStr $DxrCatCount "$DxrCatIni" "catalog" "count"
	${If} $DxrCatCount == ""
		StrCpy $DxrCatCount 0
	${EndIf}
	ClearErrors
FunctionEnd

; Give each listed item a slot; hide the rest. Scratch: $R0-$R5.
Function DxrCatPopulate
	StrCpy $R0 0
	${DoWhile} $R0 < ${DXR_CAT_SLOTS}
		IntOp $R0 $R0 + 1
		IntOp $R1 ${DxrCatSlot1} + $R0
		IntOp $R1 $R1 - 1
		${If} $DxrCatState == "ok"
		${AndIf} $R0 <= $DxrCatCount
			ReadINIStr $R2 "$DxrCatIni" "item$R0" "label"
			ReadINIStr $R3 "$DxrCatIni" "item$R0" "selected"
			ReadINIStr $R4 "$DxrCatIni" "item$R0" "ro"
			StrCpy $R5 0
			${If} $R3 == 1
				IntOp $R5 $R5 | ${SF_SELECTED}
			${EndIf}
			${If} $R4 == 1
				IntOp $R5 $R5 | ${SF_RO}
			${EndIf}
			SectionSetText $R1 "$R2"
			SectionSetFlags $R1 $R5
		${Else}
			SectionSetText $R1 ""
			SectionSetFlags $R1 0
		${EndIf}
	${Loop}
	ReadINIStr $DxrCatDesc1 "$DxrCatIni" "item1" "desc"
	ReadINIStr $DxrCatDesc2 "$DxrCatIni" "item2" "desc"
	ReadINIStr $DxrCatDesc3 "$DxrCatIni" "item3" "desc"
	ReadINIStr $DxrCatDesc4 "$DxrCatIni" "item4" "desc"
	ReadINIStr $DxrCatDesc5 "$DxrCatIni" "item5" "desc"
	ReadINIStr $DxrCatDesc6 "$DxrCatIni" "item6" "desc"
	ReadINIStr $DxrCatDesc7 "$DxrCatIni" "item7" "desc"
	ReadINIStr $DxrCatDesc8 "$DxrCatIni" "item8" "desc"
	ClearErrors
FunctionEnd

; The line above the list: that ticked items are downloaded, that this needs a
; connection, and how much. Recomputed on every tick. Scratch: $R0-$R4.
Function DxrCatUpdateTop
	StrCpy $R3 0 ; KB of the ticked downloads
	StrCpy $R4 0 ; how many
	StrCpy $R0 0
	${DoWhile} $R0 < $DxrCatCount
		IntOp $R0 $R0 + 1
		IntOp $R1 ${DxrCatSlot1} + $R0
		IntOp $R1 $R1 - 1
		SectionGetFlags $R1 $R2
		IntOp $R2 $R2 & ${SF_SELECTED}
		${If} $R2 != 0
			ReadINIStr $R2 "$DxrCatIni" "item$R0" "size_kb"
			${If} $R2 == ""
				StrCpy $R2 0
			${EndIf}
			IntOp $R3 $R3 + $R2
			IntOp $R4 $R4 + 1
		${EndIf}
	${Loop}
	ClearErrors
	ReadINIStr $R2 "$DxrCatIni" "catalog" "more"
	${If} $R2 == ""
	${OrIf} $R2 == 0
		StrCpy $R2 ""
	${Else}
		StrCpy $R2 " $R2 more are not shown."
	${EndIf}
	ClearErrors
	${If} $R4 == 0
		StrCpy $DxrCatTop "Items marked 'download' come from the DisplayXR Catalog and need an internet connection. None is selected: nothing will be downloaded.$R2"
	${Else}
		IntOp $R3 $R3 + 1023
		IntOp $R3 $R3 / 1024
		StrCpy $DxrCatTop "Selected items marked 'download' will be downloaded during setup: an internet connection is required. Total download: $R3 MB.$R2"
	${EndIf}
FunctionEnd

; Components page, before it is shown: ask once, fill the slots, set the text.
Function DxrCatComponentsPre
	${If} $DxrCatState == ""
		Banner::show /NOUNLOAD "Checking the DisplayXR Catalog for optional components..."
		Call DxrCatQuery
		Banner::destroy
		Call DxrCatPopulate
		${If} $DxrCatState == "ok"
		${AndIf} $DxrCatCount > 0
			Call DxrCatUpdateTop
		${ElseIf} $DxrCatState == "offline"
			StrCpy $DxrCatTop "The DisplayXR Catalog could not be reached (no internet connection?). Setup installs the DisplayXR Runtime only. Run Setup again later to add optional components."
		${EndIf}
	${EndIf}
	StrCpy $DxrCatShown 1
FunctionEnd

; Components page, once its controls exist: write the text above the list
; explicitly, so it does not depend on when NSIS expands the page's own string.
Function DxrCatComponentsShow
	FindWindow $R0 "#32770" "" $HWNDPARENT
	GetDlgItem $R0 $R0 ${DXR_CAT_IDC_TOP}
	SendMessage $R0 ${WM_SETTEXT} 0 "STR:$DxrCatTop"
FunctionEnd

Function .onSelChange
	${If} $DxrCatState == "ok"
	${AndIf} $DxrCatCount > 0
		Call DxrCatUpdateTop
		FindWindow $R0 "#32770" "" $HWNDPARENT
		GetDlgItem $R0 $R0 ${DXR_CAT_IDC_TOP}
		SendMessage $R0 ${WM_SETTEXT} 0 "STR:$DxrCatTop"
	${EndIf}
FunctionEnd

; Install one item through dxr-get. in: $R8 = id, $R9 = name shown to the user.
; Never aborts: a failure is added to $DxrCatSkipped. Scratch: $R1-$R4.
Function DxrCatInstallOne
	${If} $DxrCatOffline == 1
		StrCpy $DxrCatSkipped "$DxrCatSkipped$R9: no connection$\r$\n"
		Return
	${EndIf}
	DetailPrint "Catalog: $R9 - downloading and installing (dxr-get install $R8)"
	StrCpy $R1 '"$DxrCatExe"'
	${If} $DxrCatUrl != ""
		StrCpy $R1 '$R1 --catalog "$DxrCatUrl"'
	${EndIf}
	; /NOSTART: the caller restarts the service itself, so dxr-get must not.
	StrCpy $R2 ""
	${GetParameters} $R3
	ClearErrors
	${GetOptions} $R3 "/NOSTART" $R4
	${IfNot} ${Errors}
		StrCpy $R2 " --no-start-service"
	${EndIf}
	ClearErrors
	; --no-runtime: never a second runtime installer inside this one.
	nsExec::ExecToLog '$R1 install $R8 --yes --no-runtime$R2'
	Pop $R3
	${If} $R3 == 0
		DetailPrint "Catalog: $R9 - done."
		Return
	${EndIf}
	; Exit codes: docs/specs/runtime/dxr-get.md.
	StrCpy $R4 "dxr-get reported $R3"
	${If} $R3 == 10
		StrCpy $R4 "no connection, or the catalog could not be read"
		StrCpy $DxrCatOffline 1
	${ElseIf} $R3 == 11
		StrCpy $R4 "the catalog failed its signature check"
		StrCpy $DxrCatOffline 1
	${ElseIf} $R3 == 20
		StrCpy $R4 "the download failed"
	${ElseIf} $R3 == 21
		StrCpy $R4 "the download failed verification and was discarded"
	${ElseIf} $R3 == 30
		StrCpy $R4 "its installer reported an error"
	${ElseIf} $R3 == 40
		StrCpy $R4 "the catalog has no such component, or no signed Windows build of it"
	${ElseIf} $R3 == 41
		StrCpy $R4 "it is not compatible with this version of the DisplayXR Runtime, or needs a newer one"
	${ElseIf} $R3 == 50
		StrCpy $R4 "dxr-get was not running elevated"
	${EndIf}
	DetailPrint "Catalog: $R9 - NOT installed: $R4."
	StrCpy $DxrCatSkipped "$DxrCatSkipped$R9: $R4$\r$\n"
FunctionEnd

; Unattended /COMPONENTS=default: `dxr-get install --default`. Scratch: $R1-$R4.
Function DxrCatInstallDefault
	StrCpy $DxrCatExe "$INSTDIR\dxr-get.exe"
	${IfNot} ${FileExists} "$DxrCatExe"
		DetailPrint "Catalog: dxr-get.exe is not installed; /COMPONENTS=default was not acted on."
		StrCpy $DxrCatSkipped "$DxrCatSkipped/COMPONENTS=default: dxr-get.exe is not installed$\r$\n"
		Return
	${EndIf}
	StrCpy $R1 '"$DxrCatExe"'
	${If} $DxrCatUrl != ""
		StrCpy $R1 '$R1 --catalog "$DxrCatUrl"'
	${EndIf}
	StrCpy $R2 ""
	${GetParameters} $R3
	ClearErrors
	${GetOptions} $R3 "/NOSTART" $R4
	${IfNot} ${Errors}
		StrCpy $R2 " --no-start-service"
	${EndIf}
	ClearErrors
	DetailPrint "Catalog: installing the default set (dxr-get install --default)"
	nsExec::ExecToLog '$R1 install --default --yes --no-runtime$R2'
	Pop $R3
	${If} $R3 == 0
		DetailPrint "Catalog: default set - done (items dxr-get skipped are listed above)."
		Return
	${EndIf}
	StrCpy $R4 "dxr-get reported $R3"
	${If} $R3 == 10
		StrCpy $R4 "no connection, or the catalog could not be read"
	${ElseIf} $R3 == 11
		StrCpy $R4 "the catalog failed its signature check"
	${ElseIf} $R3 == 20
		StrCpy $R4 "a download failed"
	${ElseIf} $R3 == 21
		StrCpy $R4 "a download failed verification and was discarded"
	${ElseIf} $R3 == 30
		StrCpy $R4 "a component's installer reported an error"
	${EndIf}
	DetailPrint "Catalog: default set - NOT fully installed: $R4."
	StrCpy $DxrCatSkipped "$DxrCatSkipped/COMPONENTS=default (the recommended set and what matches this computer's hardware): $R4$\r$\n"
FunctionEnd

; Install what was ticked (interactive) or asked for with /COMPONENTS=.
; The runtime is fully installed by now; nothing here can fail the install.
Function DxrCatInstallChosen
	${If} $DxrCatShown != 1
		; No page was shown: a silent install. Without /COMPONENTS= this is a no-op.
		${If} $DxrCatSel == ""
			Return
		${EndIf}
		DetailPrint "Catalog: /COMPONENTS=$DxrCatSel"
		${If} $DxrCatSel == "default"
			; The recommended set plus what the hardware matches, resolved and
			; installed by dxr-get in one call. An item it cannot install here is
			; skipped in its own report and does not fail the rest (exit 0).
			Call DxrCatInstallDefault
			Goto dxr_cat_report
		${EndIf}
		Call DxrCatQuery
	${EndIf}

	StrCpy $DxrCatExe "$INSTDIR\dxr-get.exe"
	${IfNot} ${FileExists} "$DxrCatExe"
		StrCpy $DxrCatExe "$PLUGINSDIR\dxr-get.exe"
	${EndIf}

	${If} $DxrCatState == "ok"
		StrCpy $R0 0
		${DoWhile} $R0 < $DxrCatCount
			IntOp $R0 $R0 + 1
			${If} $DxrCatShown == 1
				; What the user left ticked.
				IntOp $R1 ${DxrCatSlot1} + $R0
				IntOp $R1 $R1 - 1
				SectionGetFlags $R1 $R2
				IntOp $R5 $R2 & ${SF_SELECTED}
				IntOp $R6 $R2 & ${SF_RO}
				${If} $R6 != 0
					StrCpy $R5 0
				${EndIf}
			${Else}
				ReadINIStr $R5 "$DxrCatIni" "item$R0" "selected"
			${EndIf}
			${If} $R5 != 0
			${AndIf} $R5 != ""
				ReadINIStr $R8 "$DxrCatIni" "item$R0" "id"
				ReadINIStr $R9 "$DxrCatIni" "item$R0" "name"
				Push $R0
				Call DxrCatInstallOne
				Pop $R0
			${EndIf}
		${Loop}
	${EndIf}

	; Unattended and no listing: the keywords (recommended, detected, default)
	; could not be resolved. Say so; ids given by name are still tried below.
	${If} $DxrCatShown != 1
	${AndIf} $DxrCatState != "ok"
	${AndIf} ${FileExists} "$DxrCatIni"
		ReadINIStr $R1 "$DxrCatIni" "catalog" "message"
		ClearErrors
		DetailPrint "Catalog: no component listing ($DxrCatState). $R1"
		StrCpy $DxrCatSkipped "$DxrCatSkipped/COMPONENTS=$DxrCatSel: the catalog could not be listed, so only components named by id were attempted. $R1$\r$\n"
	${EndIf}

	; Ids asked for by name that the listing did not contain (or there was no
	; listing): dxr-get decides, and answers 40 for an id it does not know.
	; Space-separated, already filtered to [a-z0-9._-]. catalog_plan.ps1 leaves
	; the list empty when the catalog was refused (signature).
	${If} $DxrCatIni != ""
	${AndIf} ${FileExists} "$DxrCatIni"
		ReadINIStr $R7 "$DxrCatIni" "catalog" "passthrough"
		ClearErrors
		${DoWhile} $R7 != ""
			; Take the first word of $R7 into $R8, keep the rest in $R7.
			StrCpy $R1 0
			dxr_cat_word:
				StrCpy $R2 $R7 1 $R1
				StrCmp $R2 "" dxr_cat_word_end
				StrCmp $R2 " " dxr_cat_word_end
				IntOp $R1 $R1 + 1
				Goto dxr_cat_word
			dxr_cat_word_end:
			StrCpy $R8 $R7 $R1
			IntOp $R1 $R1 + 1
			StrCpy $R7 $R7 "" $R1
			${If} $R8 != ""
				StrCpy $R9 $R8
				Push $R7
				Call DxrCatInstallOne
				Pop $R7
			${EndIf}
		${Loop}
	${ElseIf} $DxrCatShown != 1
		DetailPrint "Catalog: the component listing is not available ($DxrCatState); /COMPONENTS= was not acted on."
		StrCpy $DxrCatSkipped "$DxrCatSkipped/COMPONENTS=$DxrCatSel: the component listing could not be run$\r$\n"
	${EndIf}

	dxr_cat_report:
	; Report. The file is for an unattended caller; the finish page for a person.
	${If} $DxrCatSkipped != ""
		ClearErrors
		FileOpen $R0 "$INSTDIR\catalog-skipped.txt" w
		${IfNot} ${Errors}
			FileWrite $R0 "DisplayXR Setup ${VERSION}: the runtime was installed. These optional components were NOT installed:$\r$\n$\r$\n"
			FileWrite $R0 "$DxrCatSkipped"
			FileWrite $R0 "$\r$\nTo add them, run Setup again with an internet connection, or from an elevated prompt:$\r$\n"
			FileWrite $R0 "  $\"$INSTDIR\dxr-get.exe$\" install <component>$\r$\n"
			FileClose $R0
		${EndIf}
		ClearErrors
		StrCpy $DxrCatFinishExtra "$\r$\n$\r$\nNot installed:$\r$\n$DxrCatSkipped"
		StrCpy $DxrCatFinishExtra "$DxrCatFinishExtra$\r$\nRun Setup again with an internet connection to add them (details: catalog-skipped.txt in the install folder)."
	${Else}
		Delete "$INSTDIR\catalog-skipped.txt"
		${If} $DxrCatState == "offline"
		${AndIf} $DxrCatShown == 1
			StrCpy $DxrCatFinishExtra "$\r$\n$\r$\nOptional components (among them the open 2D-to-3D conversion provider) were not offered, because the DisplayXR Catalog could not be reached. Run Setup again with an internet connection to add them."
		${EndIf}
	${EndIf}

	; The install log was written at the end of the runtime section; write it
	; again so it also holds the lines above.
	StrCpy $0 "$INSTDIR\install.log"
	Push $0
	Call DumpLog
FunctionEnd

!endif ; DXR_CATALOG_UI
!macroend

!endif ; DXR_CATALOG_NSH
