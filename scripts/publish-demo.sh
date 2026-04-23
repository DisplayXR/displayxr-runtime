#!/usr/bin/env bash
# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Package one demo into the standalone-repo layout under a target directory.
# Shared by local dry-runs and CI (.github/workflows/publish-demo-<name>.yml).
#
# Usage:
#   scripts/publish-demo.sh <demo-name> <target-dir>
#
# Supported demo names: gaussiansplat
#
# Example:
#   scripts/publish-demo.sh gaussiansplat /tmp/demo-scaffold
#   cmake -S /tmp/demo-scaffold -B /tmp/demo-build -G Ninja
#   cmake --build /tmp/demo-build

set -euo pipefail

DEMO_NAME="${1:-}"
TARGET="${2:-}"
if [[ -z "$DEMO_NAME" || -z "$TARGET" ]]; then
    echo "usage: $0 <demo-name> <target-dir>" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

mkdir -p "$TARGET"

# Per-demo packaging: each `case` fills out the target layout.
case "$DEMO_NAME" in
    gaussiansplat)
        SRC_MACOS="$ROOT/demos/gaussian_splatting_handle_vk_macos"
        SRC_WIN="$ROOT/demos/gaussian_splatting_handle_vk_win"
        SRC_3DGS="$ROOT/demos/3dgs_common"
        SRC_COMMON="$ROOT/test_apps/common"
        SRC_OPENXR="$ROOT/src/external/openxr_includes"

        # Wipe only the dirs we manage; keep any .github/LICENSE baseline
        # the consumer may have hand-placed on first repo setup.
        rm -rf "$TARGET/macos" "$TARGET/windows" "$TARGET/3dgs_common" \
               "$TARGET/common" "$TARGET/openxr_includes"

        rsync -a --delete --exclude='build/' --exclude='.DS_Store' \
            "$SRC_MACOS/" "$TARGET/macos/"
        rsync -a --delete --exclude='build/' --exclude='.DS_Store' \
            "$SRC_WIN/" "$TARGET/windows/"
        rsync -a --delete --exclude='build/' --exclude='.DS_Store' \
            "$SRC_3DGS/" "$TARGET/3dgs_common/"

        # Curated subset of test_apps/common/. Windows-only sources are
        # kept (sr_common_base guards them with if(WIN32)); textures are
        # dropped since neither gaussian splat demo uses them.
        mkdir -p "$TARGET/common"
        cp "$SRC_COMMON"/display3d_view.{h,c} "$TARGET/common/"
        cp "$SRC_COMMON"/camera3d_view.{h,c}  "$TARGET/common/"
        cp "$SRC_COMMON"/view_params.h        "$TARGET/common/"
        cp "$SRC_COMMON"/input_handler.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/leia_math.h          "$TARGET/common/"
        cp "$SRC_COMMON"/stb_image.h          "$TARGET/common/"
        cp "$SRC_COMMON"/logging.{h,cpp}      "$TARGET/common/"
        cp "$SRC_COMMON"/window_manager.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/xr_session_common.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/hud_renderer.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/text_overlay.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/d3d11_renderer.{h,cpp} "$TARGET/common/"
        cp "$SRC_COMMON"/displayxr_manifest.cmake "$TARGET/common/" 2>/dev/null || true
        if [[ -f "$SRC_COMMON/SimulatedRealityVulkanBeta.dll" ]]; then
            cp "$SRC_COMMON/SimulatedRealityVulkanBeta.dll" "$TARGET/common/"
        fi

        # Vendor OpenXR extension headers.
        mkdir -p "$TARGET/openxr_includes/openxr"
        cp -r "$SRC_OPENXR/openxr/." "$TARGET/openxr_includes/openxr/"

        # Rename standalone CMakeLists variants in place.
        mv "$TARGET/macos/CMakeLists.standalone.txt"        "$TARGET/macos/CMakeLists.txt"
        mv "$TARGET/windows/CMakeLists.standalone.txt"      "$TARGET/windows/CMakeLists.txt"
        mv "$TARGET/3dgs_common/CMakeLists.standalone.txt"  "$TARGET/3dgs_common/CMakeLists.txt"

        # Build common/CMakeLists.txt from the standalone variant (it wasn't
        # rsynced because we cherry-picked files above).
        cp "$SRC_COMMON/CMakeLists.standalone.txt" "$TARGET/common/CMakeLists.txt"

        # Drop the *in-tree* scaffolding that's been synced by mistake.
        rm -f "$TARGET/macos/CMakeLists.top.txt" \
              "$TARGET/macos/README.standalone.md"

        # Top-level CMake + README + LICENSE.
        cp "$SRC_MACOS/CMakeLists.top.txt"     "$TARGET/CMakeLists.txt"
        cp "$SRC_MACOS/README.standalone.md"   "$TARGET/README.md"
        cp "$ROOT/LICENSE"                     "$TARGET/LICENSE"

        # A minimal standalone build script for each platform.
        mkdir -p "$TARGET/scripts"
        cat > "$TARGET/scripts/build_macos.sh" <<'BUILDMAC'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo ""
echo "Run: ./build/macos/gaussian_splatting_handle_vk_macos"
BUILDMAC
        chmod +x "$TARGET/scripts/build_macos.sh"

        cat > "$TARGET/scripts/build_windows.bat" <<'BUILDWIN'
@echo off
setlocal
cd /d "%~dp0\.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :error
cmake --build build || goto :error
echo.
echo Run: build\windows\gaussian_splatting_handle_vk_win.exe
exit /b 0
:error
exit /b 1
BUILDWIN
        ;;
    *)
        echo "unknown demo name: $DEMO_NAME" >&2
        exit 2
        ;;
esac

echo ""
echo "=== Packaged '$DEMO_NAME' into $TARGET ==="
echo "Next steps:"
echo "  cmake -S \"$TARGET\" -B \"$TARGET/build\" -G Ninja"
echo "  cmake --build \"$TARGET/build\""
