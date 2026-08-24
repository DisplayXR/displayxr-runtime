#!/usr/bin/env bash
# push-runtime-pf.sh — copy the dev runtime build (_package/bin) into
# C:\Program Files\DisplayXR\Runtime SAFELY. Exists because a partial hand-copy
# (service exe updated, client DLLs not) arms a time bomb: the client/service
# git-tag gate (ipc_client_check_git_tag) rejects every xrCreateInstance the
# moment the service restarts, and every app goes black with no obvious cause
# (seen 2026-08-14; cost an hour of misdirected browser debugging).
#
# What it does, in order:
#   1. Refuses to run unless _package/bin exists and holds a runtime build.
#   2. Kills known CLIENT processes that would lock DLLs (browser, test apps).
#      Never touches displayxr-service (it is restarted at the end, not left down).
#   3. Copies the FULL binary set. Excludes:
#        - displayxr-shell.exe   (ships from displayxr-shell-pvt; _package copy is stale)
#        - displayxr-mcp.exe     (dev stub shadows the installed MCP adapter and
#                                 silently kills shell voice — the installed one
#                                 at MCP\bin is preserved)
#   4. VERIFIES client DLL and service exe now embed the same git tag.
#   5. Restarts the service non-elevated so a mismatch would surface NOW, loudly,
#      instead of on some future restart.
set -euo pipefail

PKG="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/_package/bin"
PF="/c/Program Files/DisplayXR/Runtime"
[ -f "$PKG/DisplayXRClient.dll" ] || { echo "ERROR: $PKG has no runtime build (run scripts/build_windows.bat build first)" >&2; exit 1; }
[ -d "$PF" ] || { echo "ERROR: $PF not found (install the runtime once first)" >&2; exit 1; }

tag_of() { grep -aoE "v[0-9]+\.[0-9]+\.[0-9]+-[0-9]+-g[0-9a-f]+|v[0-9]+\.[0-9]+\.[0-9]+" "$1" | head -1; }

echo "== build tag in _package: $(tag_of "$PKG/DisplayXRClient.dll")"

# 2. Kill client processes that hold DisplayXRClient.dll (NOT the service).
for exe in chrome.exe cube_handle_d3d11_win.exe cube_hosted_d3d11_win.exe \
           cube_handle_d3d12_win.exe cube_handle_gl_win.exe cube_handle_vk_win.exe \
           displayxr-shell.exe; do
  taskkill //F //IM "$exe" >/dev/null 2>&1 || true
done
sleep 1

# 3. Full copy, with exclusions.
copied=0
for f in "$PKG"/*.dll "$PKG"/*.exe "$PKG"/*.json; do
  [ -f "$f" ] || continue
  base="$(basename "$f")"
  case "$base" in displayxr-shell.exe|displayxr-mcp.exe) continue ;; esac
  cp -f "$f" "$PF/$base" || { echo "ERROR: could not copy $base (locked?) — PF may be INCONSISTENT; rerun until clean" >&2; exit 1; }
  copied=$((copied+1))
done
echo "== copied $copied files"

# Keep the installed MCP adapter in place (voice resolves it CWD-relative from
# the Runtime dir; the file must EXIST there but must be the MCP\bin build).
if [ -f "/c/Program Files/DisplayXR/MCP/bin/displayxr-mcp.exe" ]; then
  cp -f "/c/Program Files/DisplayXR/MCP/bin/displayxr-mcp.exe" "$PF/displayxr-mcp.exe"
  echo "== MCP adapter: installed version enforced"
fi

# 4. Verify client and service now agree.
CT="$(tag_of "$PF/DisplayXRClient.dll")"
ST="$(tag_of "$PF/displayxr-service.exe")"
echo "== PF client=$CT service=$ST"
[ -n "$CT" ] && [ "$CT" = "$ST" ] || { echo "ERROR: client/service tag mismatch after copy — DO NOT leave it like this" >&2; exit 1; }

# 5. Restart the service so any residual mismatch surfaces immediately.
taskkill //F //IM displayxr-service.exe >/dev/null 2>&1 || true
sleep 1
explorer.exe "C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe"
sleep 3
tasklist //FI "IMAGENAME eq displayxr-service.exe" 2>/dev/null | grep -qi displayxr-service.exe \
  && echo "== service restarted (non-elevated), versions consistent: $CT" \
  || { echo "ERROR: service did not come back — start it manually: explorer.exe \"C:\\Program Files\\DisplayXR\\Runtime\\displayxr-service.exe\"" >&2; exit 1; }
