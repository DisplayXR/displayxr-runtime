#!/bin/bash
# Build macOS .pkg installer for SRMonado OpenXR Runtime
# Usage: ./installer/macos/build_installer.sh <artifact-dir> [output.pkg]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.pkg]}"
OUTPUT_PKG="${2:-SRMonado-Installer.pkg}"
VERSION="${SRMONADO_VERSION:-1.0.0}"

if [ ! -d "$ARTIFACT_DIR" ]; then
    echo "Error: artifact directory '$ARTIFACT_DIR' not found"
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== Building SRMonado macOS Installer ==="
echo "Artifact dir: $ARTIFACT_DIR"
echo "Version: $VERSION"

# --- 1. Prepare runtime payload ---
echo "--- Preparing runtime payload ---"
RUNTIME_ROOT="$WORK_DIR/payload-runtime/Library/Application Support/SRMonado"
mkdir -p "$RUNTIME_ROOT/lib"
mkdir -p "$RUNTIME_ROOT/share/vulkan/icd.d"

cp "$ARTIFACT_DIR"/lib/libopenxr_monado* "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/lib/libvulkan.1.dylib" "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/lib/libMoltenVK.dylib" "$RUNTIME_ROOT/lib/"
cp "$ARTIFACT_DIR/share/vulkan/icd.d/MoltenVK_icd.json" "$RUNTIME_ROOT/share/vulkan/icd.d/"

# Detect the runtime library filename
RUNTIME_BASENAME=$(basename "$ARTIFACT_DIR"/lib/libopenxr_monado*)

# Generate manifest with absolute library_path for installed location
cat > "$RUNTIME_ROOT/openxr_monado.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado (SRMonado macOS)",
        "library_path": "/Library/Application Support/SRMonado/lib/$RUNTIME_BASENAME"
    }
}
EOF

# Generate MoltenVK ICD with absolute path for installed location
cat > "$RUNTIME_ROOT/share/vulkan/icd.d/MoltenVK_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/Library/Application Support/SRMonado/lib/libMoltenVK.dylib",
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
    --identifier com.leiainc.srmonado.runtime \
    --version "$VERSION" \
    --install-location / \
    "$WORK_DIR/runtime.pkg"

# --- 3. Build .app bundle for test app ---
echo "--- Building .app bundle ---"
if [ -f "$ARTIFACT_DIR/bin/sim_cube_openxr" ]; then
    "$SCRIPT_DIR/create_app_bundle.sh" "$ARTIFACT_DIR" "$WORK_DIR/SimCubeOpenXR.app"

    # --- 4. Build test app component pkg ---
    echo "--- Building test app component ---"
    pkgbuild --component "$WORK_DIR/SimCubeOpenXR.app" \
        --identifier com.leiainc.srmonado.testapp \
        --version "$VERSION" \
        --install-location /Applications \
        "$WORK_DIR/testapp.pkg"
    HAS_TESTAPP=true
else
    echo "Warning: sim_cube_openxr not found, skipping test app component"
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
    <title>SRMonado OpenXR Runtime</title>
    <organization>com.leiainc</organization>
    <os-version min="13.0" />
    <license file="LICENSE" />
    <welcome file="welcome.html" />
    <choices-outline>
        <line choice="runtime" />
    </choices-outline>
    <choice id="runtime" visible="true" start_selected="true" enabled="false"
        title="SRMonado Runtime"
        description="OpenXR runtime with Vulkan compositor (required)">
        <pkg-ref id="com.leiainc.srmonado.runtime" />
    </choice>
    <pkg-ref id="com.leiainc.srmonado.runtime" version="1.0" onConclusion="none">runtime.pkg</pkg-ref>
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
