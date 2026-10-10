#!/bin/bash
# Print the path of the Linux displayxr-dashboard binary to package, for
# package_linux.sh and package_deb_linux.sh. The dashboard (src/dashboard,
# Avalonia / .NET 9) is a self-contained single file: no .NET runtime on the
# target, the Skia / HarfBuzz natives inside the bundle (extracted at first run
# to $DOTNET_BUNDLE_EXTRACT_BASE_DIR, default ~/.net — never next to the
# binary, so a read-only install prefix is fine).
#
#   DXR_DASHBOARD_BIN=<file>  a prebuilt binary (CI: the DashboardLinux job's
#                             artifact, so the .deb and the tarball ship the
#                             same bits)
#   otherwise                 `dotnet publish` it into $1 when the .NET 9 SDK is
#                             on PATH
#   neither                   print nothing and exit 0 (a runtime-only package,
#                             with a WARN) — unless DXR_REQUIRE_DASHBOARD=1,
#                             which CI sets: then it is an error.
#
# Usage: resolve_dashboard_bin.sh <scratch-dir>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:?usage: $0 <scratch-dir>}"

missing() {
    if [ "${DXR_REQUIRE_DASHBOARD:-0}" = 1 ]; then
        echo "error: $1 (DXR_REQUIRE_DASHBOARD=1)" >&2
        exit 1
    fi
    echo "WARN: $1 — packaging WITHOUT displayxr-dashboard." >&2
    exit 0
}

if [ -n "${DXR_DASHBOARD_BIN:-}" ]; then
    [ -f "$DXR_DASHBOARD_BIN" ] || missing "DXR_DASHBOARD_BIN=$DXR_DASHBOARD_BIN does not exist"
    echo "$DXR_DASHBOARD_BIN"
    exit 0
fi

command -v dotnet >/dev/null 2>&1 || missing "no DXR_DASHBOARD_BIN and no dotnet SDK on PATH"

case "$(uname -m)" in
x86_64) RID=linux-x64 ;;
aarch64 | arm64) RID=linux-arm64 ;;
*) missing "no .NET runtime identifier for $(uname -m)" ;;
esac

VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)"
echo "==> dotnet publish displayxr-dashboard ($RID, $VERSION) -> $OUT" >&2
dotnet publish "$ROOT/src/dashboard/DisplayXR.Dashboard/DisplayXR.Dashboard.csproj" \
    -c Release -r "$RID" --self-contained true \
    -p:PublishSingleFile=true -p:PublishTrimmed=false -p:EnableCompressionInSingleFile=true \
    -p:InformationalVersion="$VERSION" -p:IncludeSourceRevisionInInformationalVersion=false \
    -o "$OUT" --nologo >&2 || missing "dotnet publish failed"
[ -f "$OUT/displayxr-dashboard" ] || missing "dotnet publish produced no $OUT/displayxr-dashboard"
echo "$OUT/displayxr-dashboard"
