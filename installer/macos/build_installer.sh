#!/bin/bash
# Build macOS .pkg installer for DisplayXR OpenXR Runtime
# Usage: ./installer/macos/build_installer.sh <artifact-dir> [output.pkg]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.pkg]}"
OUTPUT_PKG="${2:-DisplayXR-Installer.pkg}"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"

if [ ! -d "$ARTIFACT_DIR" ]; then
    echo "Error: artifact directory '$ARTIFACT_DIR' not found"
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== Building DisplayXR macOS Installer ==="
echo "Artifact dir: $ARTIFACT_DIR"
echo "Version: $VERSION"

# --- 1. Prepare runtime payload ---
echo "--- Preparing runtime payload ---"
RUNTIME_ROOT="$WORK_DIR/payload-runtime/Library/Application Support/DisplayXR"
mkdir -p "$RUNTIME_ROOT/lib"
mkdir -p "$RUNTIME_ROOT/share/vulkan/icd.d"

# Find and copy the runtime dylib. Issue #256 / ADR-019 promoted this
# from MODULE to SHARED + PREFIX="", so the produced file is
# `openxr_displayxr.dylib` (no `lib` prefix). Match the legacy
# `libopenxr_displayxr*` shape too for transition safety, same as
# scripts/build_macos.sh.
RUNTIME_LIB=$(find "$ARTIFACT_DIR/lib" -maxdepth 1 \
  \( -name "openxr_displayxr.dylib" -o -name "openxr_displayxr.so" -o -name "libopenxr_displayxr*" \) \
  -type f | head -1)
if [ -z "$RUNTIME_LIB" ] || [ ! -f "$RUNTIME_LIB" ]; then
    echo "Error: runtime dylib not found in $ARTIFACT_DIR/lib/" >&2
    exit 1
fi
RUNTIME_BASENAME=$(basename "$RUNTIME_LIB")
cp "$RUNTIME_LIB" "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/lib/libvulkan.1.dylib" "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/lib/libMoltenVK.dylib" "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/share/vulkan/icd.d/MoltenVK_icd.json" "$RUNTIME_ROOT/share/vulkan/icd.d/"

# Ship the vendor plug-in dylib + JSON manifest (#267, #274). The
# runtime no longer link-includes drv_sim_display in production
# builds; without these the installed runtime has no display
# processor and xrCreateInstance fails. Manifest must land at the
# DisplayProcessors/ discovery root (NOT inside lib/...); the dylib
# can live anywhere binary_path points at.
INSTALL_PREFIX="/Library/Application Support/DisplayXR"
PLUGIN_PAYLOAD_DIR="$RUNTIME_ROOT/lib/displayxr/plugins"
MANIFEST_PAYLOAD_DIR="$RUNTIME_ROOT/DisplayProcessors"
PLUGIN_SRC="$ARTIFACT_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib"
if [ ! -f "$PLUGIN_SRC" ]; then
    echo "Error: sim-display plug-in not found at $PLUGIN_SRC" >&2
    echo "Re-run scripts/build_macos.sh first." >&2
    exit 1
fi
mkdir -p "$PLUGIN_PAYLOAD_DIR" "$MANIFEST_PAYLOAD_DIR"
cp "$PLUGIN_SRC" "$PLUGIN_PAYLOAD_DIR/"

# Fix up the plug-in's LC_RPATH so @rpath/openxr_displayxr.dylib
# resolves to the installed runtime, not the build-tree absolute path
# CMake bakes in for dev iteration. Layout at install time:
#   $INSTALL_PREFIX/lib/openxr_displayxr.dylib                 <- runtime
#   $INSTALL_PREFIX/lib/displayxr/plugins/PLUGIN.dylib         <- plug-in
# From the plug-in's @loader_path (.../lib/displayxr/plugins/), the
# runtime is at @loader_path/../.. (.../lib/).
PAYLOAD_PLUGIN="$PLUGIN_PAYLOAD_DIR/DisplayXR-SimDisplay.dylib"
chmod u+w "$PAYLOAD_PLUGIN"
# Drop every existing rpath that points into a build/_package tree;
# keep only system rpaths (Homebrew cjson etc. — install_name_tool is
# a no-op on those). We can't enumerate negatively, so just delete the
# known dev-iteration rpaths if present and let any survivor warn.
for r in $(otool -l "$PAYLOAD_PLUGIN" 2>/dev/null \
           | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' \
           | grep -E '^/.*displayxr-runtime/.*build|@loader_path' || true); do
    install_name_tool -delete_rpath "$r" "$PAYLOAD_PLUGIN" 2>/dev/null || true
done
install_name_tool -add_rpath "@loader_path/../.." "$PAYLOAD_PLUGIN"

cat > "$MANIFEST_PAYLOAD_DIR/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "$VERSION",
        "binary_path":  "$INSTALL_PREFIX/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib",
        "probe_order":  200
    }
}
EOF

# Generate manifest with absolute library_path for installed location
cat > "$RUNTIME_ROOT/openxr_displayxr.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR Runtime",
        "library_path": "/Library/Application Support/DisplayXR/lib/$RUNTIME_BASENAME"
    }
}
EOF

# Generate MoltenVK ICD with absolute path for installed location
cat > "$RUNTIME_ROOT/share/vulkan/icd.d/MoltenVK_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/Library/Application Support/DisplayXR/lib/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF

# Copy uninstall script into runtime payload
cp "$SCRIPT_DIR/uninstall.sh" "$RUNTIME_ROOT/"
chmod +x "$RUNTIME_ROOT/uninstall.sh"

# --- 2. Build runtime component pkg ---
echo "--- Building runtime component ---"
pkgbuild --root "$WORK_DIR/payload-runtime" \
    --scripts "$SCRIPT_DIR/scripts/runtime" \
    --identifier com.displayxr.runtime \
    --version "$VERSION" \
    --install-location / \
    "$WORK_DIR/runtime.pkg"

# --- 3. Build .app bundle for test app ---
echo "--- Building .app bundle ---"
if [ -f "$ARTIFACT_DIR/bin/cube_handle_vk_macos" ]; then
    "$SCRIPT_DIR/create_app_bundle.sh" "$ARTIFACT_DIR" "$WORK_DIR/DisplayXRCube.app" cube_handle_vk_macos

    # --- 4. Build test app component pkg ---
    echo "--- Building test app component ---"
    pkgbuild --component "$WORK_DIR/DisplayXRCube.app" \
        --identifier com.displayxr.testapp \
        --version "$VERSION" \
        --install-location /Applications \
        "$WORK_DIR/testapp.pkg"
    HAS_TESTAPP=true
else
    echo "Warning: cube_handle_vk_macos not found, skipping test app component"
    HAS_TESTAPP=false
fi

# --- 5. Assemble final installer ---
echo "--- Building distribution installer ---"

# Choose Distribution.xml based on whether test app exists
if [ "$HAS_TESTAPP" = true ]; then
    DIST_XML="$SCRIPT_DIR/Distribution.xml"
else
    # Create a runtime-only distribution
    cat > "$WORK_DIR/Distribution-runtime-only.xml" <<'DISTEOF'
<?xml version="1.0" encoding="UTF-8"?>
<installer-gui-script minSpecVersion="2">
    <title>DisplayXR OpenXR Runtime</title>
    <organization>com.displayxr</organization>
    <os-version min="13.0" />
    <license file="LICENSE" />
    <welcome file="welcome.html" />
    <choices-outline>
        <line choice="runtime" />
    </choices-outline>
    <choice id="runtime" visible="true" start_selected="true" enabled="false"
        title="DisplayXR Runtime"
        description="OpenXR runtime with Vulkan compositor (required)">
        <pkg-ref id="com.displayxr.runtime" />
    </choice>
    <pkg-ref id="com.displayxr.runtime" version="1.0" onConclusion="none">runtime.pkg</pkg-ref>
</installer-gui-script>
DISTEOF
    DIST_XML="$WORK_DIR/Distribution-runtime-only.xml"
fi

productbuild --distribution "$DIST_XML" \
    --resources "$SCRIPT_DIR/resources" \
    --package-path "$WORK_DIR" \
    "$OUTPUT_PKG"

echo "=== Installer built: $OUTPUT_PKG ==="
ls -lh "$OUTPUT_PKG"
