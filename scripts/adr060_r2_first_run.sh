#!/usr/bin/env bash
# adr060_r2_first_run.sh — first hardware run of ADR-060 R2/R3 (windowless weave
# DP for present-owners) on a Windows dev box, from Git Bash, ELEVATED (the push
# writes Program Files and HKLM). One paste: snapshot -> build -> push -> run the
# browser page -> grep the service log -> restore the snapshot.
#
# It does NOT judge phase (that needs a seated, still viewer — see
# docs/roadmap/texture-target-r2-r3-notes.md, "phase oracle"). It checks which
# path the service took, and that nothing else changed.
#
# Usage:
#   scripts/adr060_r2_first_run.sh --expect leia|leia-l4|sim --browser <chrome.exe> --url <T1 page> [--seconds 25] [--keep]
#
#   --expect leia  The active DP is the Leia plug-in WITHOUT L4 (today's
#                  release). Pass = the service declined the windowless path (the
#                  plug-in gives no hint, so no extra weaver is even created) and
#                  the client weaves on the window-bound DP exactly as before
#                  (the no-regression check).
#   --expect leia-l4  Leia WITH the L4 bit (later). Pass = the windowless lines.
#   --expect sim   sim-display is the active DP. Pass = the windowless lines.
#   --keep         Leave the R2 build installed (default: restore the snapshot).
#
# Preconditions (the script checks the first two):
#   - this checkout's HEAD contains the R2 + R3 commits (feat/r2-weave-windowless-phase);
#   - the runtime is installed in C:\Program Files\DisplayXR\Runtime;
#   - the active DP is what --expect says (`displayxr-cli dp list`; the script prints it).
#
# Heads-up first on a SHARED box: this restarts displayxr-service and closes the
# DisplayXR Browser and any cube test app. Only Chrome processes running from the
# --browser exe's directory are closed (push-runtime-pf.sh --browser-dir); anyone's
# ordinary Chrome on the box is left alone.
set -euo pipefail

EXPECT="" BROWSER="" URL="" SECONDS_RUN=25 KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --expect) EXPECT="$2"; shift 2 ;;
    --browser) BROWSER="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$EXPECT" in leia|leia-l4|sim) ;; *) echo "ERROR: --expect leia|leia-l4|sim" >&2; exit 2 ;; esac
[ -n "$BROWSER" ] && [ -n "$URL" ] || { echo "ERROR: --browser and --url are required" >&2; exit 2; }
[ -f "$BROWSER" ] || { echo "ERROR: browser not found: $BROWSER" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BROWSER_DIR="$(cd "$(dirname "$BROWSER")" && pwd)"

# Close the DisplayXR Browser only: chrome.exe processes whose image is under
# BROWSER_DIR. Never `taskkill /IM chrome.exe` — that closes every Chrome on the box.
close_dxr_browser() {
  local win
  win="$(cygpath -w "$BROWSER_DIR")"
  win="${win%\\}"
  case "$win" in *"'"*) echo "ERROR: browser dir must not contain a quote: $win" >&2; return 1 ;; esac
  powershell.exe -NoProfile -Command \
    "Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$win\\', [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force" \
    >/dev/null 2>&1 || true
}
PF="/c/Program Files/DisplayXR/Runtime"
STAMP="$(date +%Y%m%d-%H%M%S)"
SNAP="$ROOT/_adr060_snapshot_$STAMP"
LOGDIR="$(cygpath -u "$LOCALAPPDATA")/DisplayXR"

# 0. Is this the R2 + R3 build?
cd "$ROOT"
git log --oneline -20 | grep -q "ADR-060 R2" || { echo "ERROR: HEAD does not contain the ADR-060 R2 commit" >&2; exit 1; }
git log --oneline -20 | grep -q "ADR-060 R3" || echo "WARN: HEAD has R2 but not R3 — the woven-origin reply is not under test"
echo "== source: $(git log --oneline -1)"
[ -d "$PF" ] || { echo "ERROR: $PF not found" >&2; exit 1; }

# 1. Snapshot what is installed now (top-level binaries, plug-ins, the Version value).
mkdir -p "$SNAP/plugins"
cp -f "$PF"/*.dll "$PF"/*.exe "$PF"/*.json "$SNAP/" 2>/dev/null || true
cp -f "$PF"/plugins/*.dll "$SNAP/plugins/" 2>/dev/null || true
reg query 'HKLM\Software\DisplayXR\Runtime' //v Version //reg:64 2>/dev/null | awk '/Version/{print $3}' > "$SNAP/Version.txt" || true
echo "== snapshot: $SNAP (Version $(cat "$SNAP/Version.txt" 2>/dev/null))"

restore() {
  if [ "$KEEP" = 1 ]; then
    echo "== --keep: the R2 build stays installed; snapshot kept at $SNAP"
    return
  fi
  echo "== restoring the snapshot"
  close_dxr_browser
  taskkill //F //IM displayxr-service.exe >/dev/null 2>&1 || true
  sleep 1
  cp -f "$SNAP"/*.dll "$SNAP"/*.exe "$SNAP"/*.json "$PF/" 2>/dev/null || echo "WARN: some files did not restore (locked?) — re-run the copy from $SNAP" >&2
  cp -f "$SNAP"/plugins/*.dll "$PF/plugins/" 2>/dev/null || true
  v="$(cat "$SNAP/Version.txt" 2>/dev/null)"
  [ -n "$v" ] && reg add 'HKLM\Software\DisplayXR\Runtime' //v Version //t REG_SZ //d "$v" //f //reg:64 >/dev/null 2>&1 || true
  explorer.exe "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe"
  sleep 3
  echo "== restored; service restarted non-elevated. Snapshot kept at $SNAP (delete when satisfied)"
}
trap restore EXIT

# 2. Build + push (the only sanctioned way onto Program Files).
cmd //c "scripts\\build_windows.bat build"
bash scripts/push-runtime-pf.sh --browser-dir "$BROWSER_DIR"

# 3. Which DP is active (informational; --expect must match it).
"$PF/displayxr-cli.exe" dp list 2>&1 | head -20 || true

# 4. Run the page. The service log of THIS run is the newest one after this point.
MARK="$(date +%s)"
"$BROWSER" --new-window "$URL" >/dev/null 2>&1 &
echo "== browser up; waiting ${SECONDS_RUN}s (drag the window across a seam if the run should test the split)"
sleep "$SECONDS_RUN"
close_dxr_browser
sleep 1

LOG="$(ls -t "$LOGDIR"/DisplayXR_displayxr-service.*.log 2>/dev/null | head -1)"
[ -n "$LOG" ] || { echo "FAIL: no service log under $LOGDIR" >&2; exit 1; }
[ "$(stat -c %Y "$LOG")" -ge "$MARK" ] || echo "WARN: the newest service log predates this run: $LOG"
echo "== service log: $LOG"

# 5. Judge the path from the log.
has() { grep -qF "$1" "$LOG"; }
show() { grep -nF "$1" "$LOG" | head -3 || true; }
fail=0
echo "-- R2 lines:"
show "ADR-060 R2"
case "$EXPECT" in
  leia)
    # The plug-in hint (no L4) skips the windowless try outright; a plug-in that
    # hints but whose instance then declines logs the second line instead.
    has "does not hint windowless phase" || has "does not declare PHASE_FROM_PRESENT_ORIGIN" \
      || { echo "FAIL: expected the windowless path to be declined (no hint / no bit)"; fail=1; }
    has "OWN WINDOWLESS display processor" && { echo "FAIL: went windowless on a plug-in without the bit"; fail=1; }
    has "weaves on its OWN display processor on the RENDER device" || has "panel DP" || echo "WARN: no window-bound weave-DP line found; check the log by hand"
    ;;
  leia-l4|sim)
    has "OWN WINDOWLESS display processor" || { echo "FAIL: no windowless weave DP"; fail=1; }
    has "phased from present origin" || { echo "FAIL: no present-origin push"; fail=1; }
    has "segmenting its window per screen" && echo "   (split rule (a) engaged: the offered screen list became the segment list)"
    ;;
esac
# Never acceptable on any arm:
for bad in "srCreateWeaverDX11 failed" "weave DP: factory refused" "DEVICE_REMOVED"; do
  has "$bad" && { echo "FAIL: '$bad' in the log"; show "$bad"; fail=1; }
done
[ "$fail" = 0 ] && echo "PASS ($EXPECT): the service took the expected path" || echo "FAIL ($EXPECT): see above; full log $LOG"
exit "$fail"
