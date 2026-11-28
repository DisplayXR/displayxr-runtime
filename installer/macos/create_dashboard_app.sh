#!/bin/bash
# Build "DisplayXR Dashboard.app" (src/dashboard, Avalonia / .NET 10, ADR-051)
# for the macOS runtime .pkg, which installs it into /Applications.
#
# Usage: create_dashboard_app.sh <out-dir> <version>
#   Writes <out-dir>/DisplayXR Dashboard.app and prints its path on stdout.
#
#   DXR_DASHBOARD_APP=<path.app>  a prebuilt bundle (copied as is)
#   otherwise                     `dotnet publish` it when the .NET 10 SDK is on PATH
#   neither                       print nothing, exit 0 (a .pkg without the
#                                 Dashboard, with a WARN) — unless
#                                 DXR_REQUIRE_DASHBOARD=1 (CI): then an error.
#
# Layout: self-contained (no .NET runtime needed on the Mac), single file for the
# managed code, but with the native libraries (Skia, HarfBuzz, AvaloniaNative)
# left BESIDE it instead of inside it (IncludeNativeLibrariesForSelfExtract=false,
# unlike Linux/Windows). Inside the file they would be extracted to ~/.net at
# first run with no Developer ID signature, which the hardened runtime's library
# validation refuses and notarization cannot see. And not a plain directory
# publish: codesign treats every file in Contents/MacOS as code, so ~200 managed
# .dll files there would each need a detached signature. This way
# Contents/MacOS holds only Mach-O files, each signed by sign-tree.
#
# The bundle carries no displayxr-cli: the Dashboard resolves the runtime's
# /Library/Application Support/DisplayXR/bin/displayxr-cli, the CLI beside the
# service, the only one the service admits as a status reader (#960).
#
# Architecture: osx-arm64, matching the runtime .pkg (built arm64-only on
# macos-15 runners with Homebrew's arm64 Vulkan/MoltenVK).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="${1:?usage: $0 <out-dir> <version>}"
VERSION="${2:?usage: $0 <out-dir> <version>}"
APP_NAME="DisplayXR Dashboard"
APP="$OUT_DIR/$APP_NAME.app"
# The .pkg's own floor (Distribution.xml <os-version min="13.0"/>).
MIN_MACOS="13.0"

missing() {
    if [ "${DXR_REQUIRE_DASHBOARD:-0}" = 1 ]; then
        echo "error: $1 (DXR_REQUIRE_DASHBOARD=1)" >&2
        exit 1
    fi
    echo "WARN: $1 — the .pkg ships WITHOUT the DisplayXR Dashboard." >&2
    exit 0
}

mkdir -p "$OUT_DIR"
rm -rf "$APP"

if [ -n "${DXR_DASHBOARD_APP:-}" ]; then
    [ -d "$DXR_DASHBOARD_APP/Contents/MacOS" ] || missing "DXR_DASHBOARD_APP=$DXR_DASHBOARD_APP is not an app bundle"
    cp -R "$DXR_DASHBOARD_APP" "$APP"
    echo "$APP"
    exit 0
fi

command -v dotnet >/dev/null 2>&1 || missing "no dotnet SDK on PATH"

# CFBundleShortVersionString must be numeric (X.Y.Z); the full describe string
# (a dev build's -N-gSHA) goes to CFBundleVersion's neighbour, the
# informational version the Dashboard prints with --version.
SHORT_VERSION="$(printf '%s' "$VERSION" | sed -E 's/^v?([0-9]+(\.[0-9]+){0,2}).*/\1/')"
printf '%s' "$SHORT_VERSION" | grep -Eq '^[0-9]+(\.[0-9]+){0,2}$' || SHORT_VERSION="1.0.0"
INFO_VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo "$VERSION")"

MACOS_DIR="$APP/Contents/MacOS"
RES_DIR="$APP/Contents/Resources"
mkdir -p "$MACOS_DIR" "$RES_DIR"

echo "==> dotnet publish displayxr-dashboard (osx-arm64, $INFO_VERSION) -> $MACOS_DIR" >&2
dotnet publish "$ROOT/src/dashboard/DisplayXR.Dashboard/DisplayXR.Dashboard.csproj" \
    -c Release -r osx-arm64 --self-contained true \
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=false \
    -p:PublishTrimmed=false -p:EnableCompressionInSingleFile=true \
    -p:InformationalVersion="$INFO_VERSION" -p:IncludeSourceRevisionInInformationalVersion=false \
    -o "$MACOS_DIR" --nologo >&2 || missing "dotnet publish failed"
[ -x "$MACOS_DIR/displayxr-dashboard" ] || missing "dotnet publish produced no $MACOS_DIR/displayxr-dashboard"
# Nothing in Contents/MacOS but what runs: no symbol files.
find "$MACOS_DIR" -name '*.pdb' -delete
# The NuGet natives come universal (x86_64 + arm64): keep the slice this app
# runs. And libAvaloniaNative's install name is a build-box path
# (/usr/local/lib/libAvalonia.Native.OSX.dylib), which build_installer.sh's
# "no build-box paths" payload check rightly flags; it is dlopen()ed by full
# path, so an @rpath id changes nothing at run time. Each edit invalidates the
# signature: re-sign ad-hoc now, sign-tree gives the final one.
for lib in "$MACOS_DIR"/*.dylib; do
    if lipo -info "$lib" 2>/dev/null | grep -q 'Architectures in the fat file'; then
        lipo "$lib" -thin arm64 -output "$lib.thin" && mv -f "$lib.thin" "$lib"
    fi
    chmod u+w,a+rx "$lib"
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null
    codesign --force --sign - "$lib" >/dev/null 2>&1
done

# Icon: the Dashboard's own artwork (the Linux menu icon is the same 256 px
# image as the Windows exe icon, assets/displayxr_app.ico).
ICON_SRC="$ROOT/scripts/linux/dashboard/displayxr-dashboard.png"
ICONSET="$(mktemp -d)/displayxr-dashboard.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256; do
    sips -z "$s" "$s" "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
done
for s in 16 32 128; do
    d=$((s * 2))
    sips -z "$d" "$d" "$ICON_SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$RES_DIR/displayxr-dashboard.icns"
rm -rf "$(dirname "$ICONSET")"

cat >"$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>$APP_NAME</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundleExecutable</key>
    <string>displayxr-dashboard</string>
    <key>CFBundleIconFile</key>
    <string>displayxr-dashboard</string>
    <key>CFBundleIdentifier</key>
    <string>com.displayxr.dashboard</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$SHORT_VERSION</string>
    <key>CFBundleVersion</key>
    <string>$SHORT_VERSION</string>
    <key>CFBundleGetInfoString</key>
    <string>$APP_NAME $INFO_VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>$MIN_MACOS</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.utilities</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright 2026, The DisplayXR Project</string>
</dict>
</plist>
PLIST
plutil -lint -s "$APP/Contents/Info.plist"

echo "$APP"
