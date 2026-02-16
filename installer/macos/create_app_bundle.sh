#!/bin/bash
# Create a macOS .app bundle for SimCube OpenXR test app
# Usage: ./create_app_bundle.sh <artifact-dir> [output.app]
set -e

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.app]}"
APP_BUNDLE="${2:-SimCubeOpenXR.app}"
VERSION="${SRMONADO_VERSION:-1.0.0}"

if [ ! -f "$ARTIFACT_DIR/bin/sim_cube_openxr" ]; then
    echo "Error: sim_cube_openxr binary not found in $ARTIFACT_DIR/bin/"
    exit 1
fi

echo "Creating .app bundle: $APP_BUNDLE"

# Create bundle structure
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources/lib"

# --- PkgInfo ---
echo -n "APPL????" > "$APP_BUNDLE/Contents/PkgInfo"

# --- Info.plist ---
cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>SimCubeOpenXR</string>
    <key>CFBundleIdentifier</key>
    <string>com.leiainc.simcubeopenxr</string>
    <key>CFBundleName</key>
    <string>SimCube OpenXR</string>
    <key>CFBundleDisplayName</key>
    <string>SimCube OpenXR</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# --- Shell launcher (CFBundleExecutable) ---
cat > "$APP_BUNDLE/Contents/MacOS/SimCubeOpenXR" <<'LAUNCHER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")/../Resources" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/MoltenVK_icd.json"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
exec "$DIR/sim_cube_openxr"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/SimCubeOpenXR"

# --- Resources: binary and libraries ---
cp "$ARTIFACT_DIR/bin/sim_cube_openxr" "$APP_BUNDLE/Contents/Resources/"
cp "$ARTIFACT_DIR"/lib/libopenxr_monado* "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR"/lib/libopenxr_loader* "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR/lib/libvulkan.1.dylib" "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR/lib/libMoltenVK.dylib" "$APP_BUNDLE/Contents/Resources/lib/"

# --- Resources: manifests with paths relative to .app bundle ---
cat > "$APP_BUNDLE/Contents/Resources/openxr_monado.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado (SRMonado macOS)",
        "library_path": "lib/$(basename "$ARTIFACT_DIR"/lib/libopenxr_monado*)"
    }
}
EOF

cat > "$APP_BUNDLE/Contents/Resources/MoltenVK_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "lib/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF

echo ".app bundle created: $APP_BUNDLE"
