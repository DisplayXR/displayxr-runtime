#!/bin/bash
# Create a macOS .app bundle for an OpenXR test app
# Usage: ./create_app_bundle.sh <artifact-dir> [output.app] [binary-name]
set -e

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.app] [binary-name]}"
APP_BUNDLE="${2:-SimCubeOpenXR.app}"
BINARY_NAME="${3:-cube_handle_vk_macos}"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"

# Derive display name from app bundle filename (e.g. "SimCubeOpenXR" from "SimCubeOpenXR.app")
BUNDLE_DISPLAY_NAME="$(basename "$APP_BUNDLE" .app)"
BUNDLE_ID="com.displayxr.$(echo "$BUNDLE_DISPLAY_NAME" | tr '[:upper:]' '[:lower:]')"

if [ ! -f "$ARTIFACT_DIR/bin/$BINARY_NAME" ]; then
    echo "Error: $BINARY_NAME binary not found in $ARTIFACT_DIR/bin/"
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
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${BUNDLE_DISPLAY_NAME}</string>
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
cat > "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME" <<LAUNCHER
#!/bin/bash
DIR="\$(cd "\$(dirname "\$0")/../Resources" && pwd)"
export XR_RUNTIME_JSON="\$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="\$DIR/lib:\${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="\$DIR/MoltenVK_icd.json"
export VK_DRIVER_FILES="\$DIR/MoltenVK_icd.json"
export SIM_DISPLAY_OUTPUT="\${SIM_DISPLAY_OUTPUT:-anaglyph}"
exec "\$DIR/$BINARY_NAME"
LAUNCHER
chmod +x "$APP_BUNDLE/Contents/MacOS/$BUNDLE_DISPLAY_NAME"

# --- Resources: binary and libraries ---
cp "$ARTIFACT_DIR/bin/$BINARY_NAME" "$APP_BUNDLE/Contents/Resources/"
cp "$ARTIFACT_DIR"/lib/libopenxr_displayxr* "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR"/lib/libopenxr_loader* "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR/lib/libvulkan.1.dylib" "$APP_BUNDLE/Contents/Resources/lib/"
cp "$ARTIFACT_DIR/lib/libMoltenVK.dylib" "$APP_BUNDLE/Contents/Resources/lib/"

# --- Resources: manifests with paths relative to .app bundle ---
cat > "$APP_BUNDLE/Contents/Resources/openxr_displayxr.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR Runtime",
        "library_path": "lib/$(basename "$ARTIFACT_DIR"/lib/libopenxr_displayxr*)"
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
