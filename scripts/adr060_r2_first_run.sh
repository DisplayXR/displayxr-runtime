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
#                                  [--leia-repo <displayxr-leia-plugin checkout on the L4 branch>]   (leia-l4 only)
#
#   --expect leia  The active DP is the Leia plug-in WITHOUT L4 (today's
#                  release). Pass = the service declined the windowless path (the
#                  plug-in gives no hint, so no extra weaver is even created) and
#                  the client weaves on the window-bound DP exactly as before
#                  (the no-regression check).
#   --expect leia-l4  Leia WITH L4, built from --leia-repo (feat/l4-phase-from-present-origin)
#                  against THIS runtime checkout, registered over the installed
#                  leia-sr row for the run (register_dev_plugin.bat), restored
#                  afterwards (reg import of the exported row; only the allowlist
#                  rows this run added are removed; released DLL hash + `dp list`
#                  verified). Pass = the windowless lines on the real SR weaver.
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

EXPECT="" BROWSER="" URL="" SECONDS_RUN=25 KEEP=0 LEIA_REPO=""
# The released leia-sr DLL the restore must land back on (2.18.0 on the panel box);
# override with --leia-released-sha when the box carries another release.
LEIA_RELEASED_SHA="154b7a87d94c8ff2b5e112c6e734cf014da381d7c125c26be9a0067538b35f12"
while [ $# -gt 0 ]; do
  case "$1" in
    --expect) EXPECT="$2"; shift 2 ;;
    --browser) BROWSER="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --leia-repo) LEIA_REPO="$2"; shift 2 ;;
    --leia-released-sha) LEIA_RELEASED_SHA="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$EXPECT" in leia|leia-l4|sim) ;; *) echo "ERROR: --expect leia|leia-l4|sim" >&2; exit 2 ;; esac
[ -n "$BROWSER" ] && [ -n "$URL" ] || { echo "ERROR: --browser and --url are required" >&2; exit 2; }
[ -f "$BROWSER" ] || { echo "ERROR: browser not found: $BROWSER" >&2; exit 2; }
if [ "$EXPECT" = leia-l4 ]; then
  [ -n "$LEIA_REPO" ] && [ -d "$LEIA_REPO/src/drv_leia" ] || { echo "ERROR: --expect leia-l4 needs --leia-repo <displayxr-leia-plugin checkout>" >&2; exit 2; }
  LEIA_REPO="$(cd "$LEIA_REPO" && pwd)"
  grep -q "runtime ADR-060 R2, L4" <<< "$(git -C "$LEIA_REPO" log --oneline -20)" \
    || { echo "ERROR: $LEIA_REPO HEAD does not carry the L4 commit (feat/l4-phase-from-present-origin)" >&2; exit 2; }
fi

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

ALLOW_KEY='HKLM\Software\DisplayXR\Trust\Allowlist'
allowlist_names() { reg query "$ALLOW_KEY" //reg:64 2>/dev/null | awk '/REG_SZ/{print $1}' | sort || true; }

# 0. Is this the R2 + R3 build?
cd "$ROOT"
grep -q "ADR-060 R2" <<< "$(git log --oneline -20)" || { echo "ERROR: HEAD does not contain the ADR-060 R2 commit" >&2; exit 1; }
grep -q "ADR-060 R3" <<< "$(git log --oneline -20)" || echo "WARN: HEAD has R2 but not R3 — the woven-origin reply is not under test"
echo "== source: $(git log --oneline -1)"
[ -d "$PF" ] || { echo "ERROR: $PF not found" >&2; exit 1; }

# 1. Snapshot what is installed now (top-level binaries, plug-ins, the Version value).
mkdir -p "$SNAP/plugins"
cp -f "$PF"/*.dll "$PF"/*.exe "$PF"/*.json "$SNAP/" 2>/dev/null || true
cp -f "$PF"/plugins/*.dll "$SNAP/plugins/" 2>/dev/null || true
reg query 'HKLM\Software\DisplayXR\Runtime' //v Version //reg:64 2>/dev/null | awk '/Version/{print $3}' > "$SNAP/Version.txt" || true
# Trust\Allowlist value names BEFORE anything is pushed: push-runtime-pf.sh adds
# the dev plug-in digests (5 rows on the panel box) and register_dev_plugin.bat
# the dev Leia DLL's; the restore removes exactly the rows this run added.
allowlist_names > "$SNAP/allowlist.before"
echo "== snapshot: $SNAP (Version $(cat "$SNAP/Version.txt" 2>/dev/null))"

LEIA_KEY='HKLM\Software\DisplayXR\DisplayProcessors\leia-sr'

# leia-l4: put the exported released row back (allowlist_restore drops the dev
# DLL's row). No installer run: on the panel box an installer cascades, and a
# service an installer restarts can fall to sim-display.
leia_restore() {
  [ "$EXPECT" = leia-l4 ] && [ -f "$SNAP/leia-sr.reg" ] || return 0
  echo "== restoring the released leia-sr row"
  reg delete "$LEIA_KEY" //f //reg:64 >/dev/null 2>&1 || true
  reg import "$(cygpath -w "$SNAP/leia-sr.reg")" //reg:64 >/dev/null 2>&1 \
    || echo "WARN: reg import of $SNAP/leia-sr.reg failed — import it by hand" >&2
}

# Every arm: drop the Trust\Allowlist rows this run added (the runtime push's dev
# plug-in digests, and on leia-l4 the dev Leia DLL's), listing them in
# $SNAP/allowlist.removed. Rows that existed before the run are never touched.
allowlist_restore() {
  [ -f "$SNAP/allowlist.before" ] || return 0
  allowlist_names > "$SNAP/allowlist.after"
  local added
  added="$(comm -13 "$SNAP/allowlist.before" "$SNAP/allowlist.after" || true)"
  if [ -n "$added" ]; then
    echo "== Trust\\Allowlist rows added by this run (removing):"
    while read -r v; do
      [ -n "$v" ] || continue
      echo "   $v"
      echo "$v" >> "$SNAP/allowlist.removed"
      reg delete "$ALLOW_KEY" //v "$v" //f //reg:64 >/dev/null 2>&1 || echo "   WARN: could not delete $v" >&2
    done <<< "$added"
  else
    echo "== no Trust\\Allowlist rows were added by this run"
  fi
}

# leia-l4: after the service restart, the row and DLL must be the released ones.
leia_verify() {
  [ "$EXPECT" = leia-l4 ] && [ -f "$SNAP/leia-sr.reg" ] || return 0
  local bin sha
  bin="$(reg query "$LEIA_KEY" //v Binary //reg:64 2>/dev/null | awk -F'REG_SZ' '/Binary/{gsub(/^[ \t]+/,"",$2); print $2}')"
  sha="$(sha256sum "$(cygpath -u "$bin")" 2>/dev/null | awk '{print tolower($1)}')"
  echo "== leia-sr row Binary=$bin sha256=$sha"
  [ "$sha" = "$LEIA_RELEASED_SHA" ] && echo "   released DLL hash OK" || echo "   WARN: hash != the released $LEIA_RELEASED_SHA" >&2
  "$PF/displayxr-cli.exe" dp list 2>&1 | grep -i "leia" || true
  "$PF/displayxr-cli.exe" info 2>&1 | grep -iE "plug-?in|leia" | head -5 || true
}

restore() {
  if [ "$KEEP" = 1 ]; then
    echo "== --keep: the R2 build stays installed; snapshot kept at $SNAP"
    [ "$EXPECT" = leia-l4 ] && [ -f "$SNAP/leia-sr.reg" ] && \
      echo "== --keep: the DEV leia-sr stays registered; restore later with: reg import \"$(cygpath -w "$SNAP/leia-sr.reg")\" /reg:64 (allowlist rows before the run: $SNAP/allowlist.before)"
    return
  fi
  leia_restore
  allowlist_restore
  echo "== restoring the snapshot"
  close_dxr_browser
  taskkill //F //IM displayxr-service.exe >/dev/null 2>&1 || true
  sleep 1
  cp -f "$SNAP"/*.dll "$SNAP"/*.exe "$SNAP"/*.json "$PF/" 2>/dev/null || echo "WARN: some files did not restore (locked?) — re-run the copy from $SNAP" >&2
  cp -f "$SNAP"/plugins/*.dll "$PF/plugins/" 2>/dev/null || true
  v="$(cat "$SNAP/Version.txt" 2>/dev/null)"
  [ -n "$v" ] && reg add 'HKLM\Software\DisplayXR\Runtime' //v Version //t REG_SZ //d "$v" //f //reg:64 >/dev/null 2>&1 || true
  explorer.exe "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe" || true  # explorer.exe exits non-zero on success
  sleep 3
  echo "== restored; service restarted non-elevated. Snapshot kept at $SNAP (delete when satisfied)"
  leia_verify
}
trap restore EXIT

# 2. Build + push (the only sanctioned way onto Program Files).
cmd //c "scripts\\build_windows.bat build"
bash scripts/push-runtime-pf.sh --browser-dir "$BROWSER_DIR"

# 2b. leia-l4: build the L4 plug-in against THIS runtime checkout and register it
#     over the installed leia-sr row (export first, so the restore can put it back).
if [ "$EXPECT" = leia-l4 ]; then
  reg export "$LEIA_KEY" "$(cygpath -w "$SNAP/leia-sr.reg")" //y //reg:64 >/dev/null \
    || { echo "ERROR: could not export $LEIA_KEY — not touching the plug-in" >&2; exit 1; }
  echo "== leia-sr row exported to $SNAP/leia-sr.reg ($(wc -l < "$SNAP/allowlist.before") allowlist rows before the run)"
  # Through .cmd files: paths here may contain spaces, and quotes do not survive an
  # inline `cmd //c "..."` from MSYS intact.
  { printf '%s\r\n' '@echo off'
    printf 'set "DXR_RUNTIME_SOURCE_DIR=%s"\r\n' "$(cygpath -w "$ROOT")"
    printf 'cd /d "%s" || exit /b 1\r\n' "$(cygpath -w "$LEIA_REPO")"
    printf 'call scripts\\build-windows.bat build\r\n'
  } > "$SNAP/leia_build.cmd"
  # A depot_tools ninja first on PATH breaks the plug-in's CMake configure
  # ("ninja --version: no such file"; seen on the panel box): drop it for this build.
  LEIA_PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -vi 'depot_tools' | paste -sd: - || true)"
  PATH="${LEIA_PATH:-$PATH}" cmd //c "$(cygpath -w "$SNAP/leia_build.cmd")"
  DEV_DLL="$(find "$LEIA_REPO/_package" -name DisplayXR-LeiaSR.dll -newer "$SNAP/leia-sr.reg" 2>/dev/null | sed -n 1p)"
  # An incremental rebuild of a tree a peer already built relinks nothing, so no
  # DLL is newer than the export: fall back to the newest one and say so.
  [ -n "$DEV_DLL" ] || { DEV_DLL="$(ls -t $(find "$LEIA_REPO/_package" -name DisplayXR-LeiaSR.dll 2>/dev/null) 2>/dev/null | sed -n 1p)"; [ -n "$DEV_DLL" ] && echo "== no relink (up to date); using the newest build: $DEV_DLL (mtime $(date -r "$DEV_DLL" "+%F %T"))"; }
  [ -n "$DEV_DLL" ] || { echo "ERROR: no DisplayXR-LeiaSR.dll under $LEIA_REPO/_package" >&2; exit 1; }
  DEV_SHA="$(sha256sum "$DEV_DLL" | awk '{print tolower($1)}')"
  echo "== dev plug-in: $DEV_DLL sha256=$DEV_SHA (mtime $(date -r "$DEV_DLL" "+%F %T"))"
  printf '%s  %s\n' "$DEV_SHA" "$DEV_DLL" > "$SNAP/dev_dll.sha256"
  # EXPECT_DEV_SHA=<sha256>: refuse to register anything but the DLL a peer reported.
  if [ -n "${EXPECT_DEV_SHA:-}" ]; then
    [ "$DEV_SHA" = "$(printf '%s' "$EXPECT_DEV_SHA" | tr 'A-F' 'a-f')" ] && echo "   matches EXPECT_DEV_SHA" \
      || { echo "ERROR: dev DLL sha256 $DEV_SHA != EXPECT_DEV_SHA $EXPECT_DEV_SHA — not registering" >&2; exit 1; }
  else
    echo "   (EXPECT_DEV_SHA unset: compare by hand with the sha256 the builder reported)"
  fi
  { printf '%s\r\n' '@echo off'
    printf 'cd /d "%s" || exit /b 1\r\n' "$(cygpath -w "$ROOT")"
    printf 'call scripts\\register_dev_plugin.bat leia "%s"\r\n' "$(cygpath -w "$DEV_DLL")"
  } > "$SNAP/leia_register.cmd"
  cmd //c "$(cygpath -w "$SNAP/leia_register.cmd")"
  # The dev plug-in loads at the next service start.
  taskkill //F //IM displayxr-service.exe >/dev/null 2>&1 || true
  sleep 1
  explorer.exe "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe" || true  # exits non-zero on success
  sleep 3
fi

# 3. Which DP is active (informational; --expect must match it).
"$PF/displayxr-cli.exe" dp list 2>&1 | head -20 || true

# 4. Run the page. The service log of THIS run is the newest one after this point.
MARK="$(date +%s)"
# Launch NON-elevated through explorer.exe on a .cmd: a browser started from this
# elevated shell runs at High integrity against the Medium-integrity service, and
# Chromium launched that way can hang in ShowWindow. Fresh profile + logging.
mkdir -p "$SNAP/browser"
{ printf '%s\r\n' '@echo off' 'set XR_RUNTIME_JSON='
  printf 'start "" "%s" --user-data-dir="%s" --no-first-run --no-default-browser-check --enable-logging --log-file="%s" --v=1 "%s"\r\n' \
    "$(cygpath -w "$BROWSER")" "$(cygpath -w "$SNAP/browser/profile")" "$(cygpath -w "$SNAP/browser/chrome.log")" "$URL"
} > "$SNAP/browser/launch.cmd"
explorer.exe "$(cygpath -w "$SNAP/browser/launch.cmd")" || true
echo "== browser log: $SNAP/browser/chrome.log"
echo "== browser up; waiting ${SECONDS_RUN}s (drag the window across a seam if the run should test the split)"
sleep "$SECONDS_RUN"
close_dxr_browser
sleep 1

LOG="$(ls -t "$LOGDIR"/DisplayXR_displayxr-service.*.log 2>/dev/null | sed -n 1p)"  # head -1 = SIGPIPE (141) under pipefail
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
    if [ "$EXPECT" = leia-l4 ]; then
      has "externalRouting=1" || echo "WARN: no 'externalRouting=1' caps line from the plug-in in this log"
      has "hints windowless phase" || echo "WARN: no 'hints windowless phase' line (L4 >= 23365af logs it once)"
      has "does not hint windowless phase" && { echo "FAIL: the L4 plug-in gave no hint"; fail=1; }
      has "SR refused EXTERNAL routing for a WINDOWLESS" && { echo "FAIL: SR refused EXTERNAL for the windowless weaver (display-scoped phase)"; fail=1; }
    fi
    ;;
esac
# Never acceptable on any arm:
for bad in "srCreateWeaverDX11 failed" "weave DP: factory refused" "DEVICE_REMOVED"; do
  has "$bad" && { echo "FAIL: '$bad' in the log"; show "$bad"; fail=1; }
done
[ "$fail" = 0 ] && echo "PASS ($EXPECT): the service took the expected path" || echo "FAIL ($EXPECT): see above; full log $LOG"
exit "$fail"
