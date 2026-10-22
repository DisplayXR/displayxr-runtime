#!/bin/bash
# Build macOS .pkg installer for DisplayXR OpenXR Runtime
# Usage: ./installer/macos/build_installer.sh <artifact-dir> [output.pkg]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARTIFACT_DIR="${1:?Usage: $0 <artifact-dir> [output.pkg]}"
VERSION="${DISPLAYXR_VERSION:-1.0.0}"
# Default to a versioned filename so the artifact carries its version
# in its name (mirrors Windows' DisplayXRSetup-X.Y.Z.NNN.exe). The
# explicit $2 form is still honored for callers that want a fixed
# output path.
OUTPUT_PKG="${2:-DisplayXR-Installer-$VERSION.pkg}"

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

# cJSON is a direct dependency of the runtime + plug-in. build_macos.sh
# bundles it into the artifact's lib/; fall back to Homebrew for older
# artifacts built before that change.
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
if [ -f "$ARTIFACT_DIR/lib/libcjson.1.dylib" ]; then
    cp -L "$ARTIFACT_DIR/lib/libcjson.1.dylib" "$RUNTIME_ROOT/lib/"
elif [ -f "$BREW_PREFIX/lib/libcjson.1.dylib" ]; then
    cp -L "$BREW_PREFIX/lib/libcjson.1.dylib" "$RUNTIME_ROOT/lib/"
else
    echo "Warning: libcjson.1.dylib not found; installed runtime may keep an absolute Homebrew dependency"
fi

# Defensive Vulkan rpath fixup. `scripts/build_macos.sh` already runs
# the equivalent install_name_tool steps against the artifact dir, but
# repeat here to be robust against external callers that hand us an
# artifact dir built by something else (or by an older script). All
# operations are idempotent — install_name_tool -change is a no-op
# when the source path isn't present; install_name_tool -id always
# writes (cheap).
#
# Each install_name_tool invocation invalidates the dylib's code
# signature; on modern macOS, loading an invalidly-signed dylib via
# dlopen results in immediate SIGKILL ("Code Signature Invalid").
# Re-sign ad-hoc after every modification — `codesign --force --sign -`
# is a no-op-equivalent for already-correctly-signed dylibs.
chmod u+w "$RUNTIME_ROOT/lib/libvulkan.1.dylib"
install_name_tool -id @rpath/libvulkan.1.dylib "$RUNTIME_ROOT/lib/libvulkan.1.dylib" 2>/dev/null || true
codesign --force --sign - "$RUNTIME_ROOT/lib/libvulkan.1.dylib" 2>/dev/null || true

chmod u+w "$RUNTIME_ROOT/lib/libMoltenVK.dylib"
install_name_tool -id @rpath/libMoltenVK.dylib "$RUNTIME_ROOT/lib/libMoltenVK.dylib" 2>/dev/null || true
codesign --force --sign - "$RUNTIME_ROOT/lib/libMoltenVK.dylib" 2>/dev/null || true

if [ -f "$RUNTIME_ROOT/lib/libcjson.1.dylib" ]; then
  chmod u+w "$RUNTIME_ROOT/lib/libcjson.1.dylib"
  install_name_tool -id @rpath/libcjson.1.dylib "$RUNTIME_ROOT/lib/libcjson.1.dylib" 2>/dev/null || true
  codesign --force --sign - "$RUNTIME_ROOT/lib/libcjson.1.dylib" 2>/dev/null || true
fi

# Retarget the runtime's Vulkan + cJSON deps from the Homebrew absolute
# path ($BREW_PREFIX is /opt/homebrew on Apple Silicon, /usr/local on
# Intel) to @rpath.
chmod u+w "$RUNTIME_ROOT/lib/$RUNTIME_BASENAME"
install_name_tool -change "$BREW_PREFIX/opt/vulkan-loader/lib/libvulkan.1.dylib" \
                          @rpath/libvulkan.1.dylib \
                          "$RUNTIME_ROOT/lib/$RUNTIME_BASENAME" 2>/dev/null || true
install_name_tool -change "$BREW_PREFIX/opt/cjson/lib/libcjson.1.dylib" \
                          @rpath/libcjson.1.dylib \
                          "$RUNTIME_ROOT/lib/$RUNTIME_BASENAME" 2>/dev/null || true
# Runtime needs @loader_path so it can resolve @rpath/lib*.dylib
# from its own directory. Idempotent: install_name_tool warns + exits 0
# if the rpath is already there.
install_name_tool -add_rpath @loader_path "$RUNTIME_ROOT/lib/$RUNTIME_BASENAME" 2>/dev/null || true
codesign --force --sign - "$RUNTIME_ROOT/lib/$RUNTIME_BASENAME" 2>/dev/null || true

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
# Drop every existing rpath that points into a build/_package tree.
# We can't enumerate negatively, so just delete the known dev-iteration
# rpaths if present and let any survivor warn.
for r in $(otool -l "$PAYLOAD_PLUGIN" 2>/dev/null \
           | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' \
           | grep -E '^/.*displayxr-runtime/.*build|@loader_path' || true); do
    install_name_tool -delete_rpath "$r" "$PAYLOAD_PLUGIN" 2>/dev/null || true
done
install_name_tool -add_rpath "@loader_path/../.." "$PAYLOAD_PLUGIN" 2>/dev/null || true

# Defensive Vulkan + cJSON -change on the embedded plug-in (same
# idempotent fixup applied to the runtime above). @loader_path/../..
# above doubles as the rpath that resolves @rpath/lib*.dylib to
# .../lib/lib*.dylib at the install location.
install_name_tool -change "$BREW_PREFIX/opt/vulkan-loader/lib/libvulkan.1.dylib" \
                          @rpath/libvulkan.1.dylib \
                          "$PAYLOAD_PLUGIN" 2>/dev/null || true
install_name_tool -change "$BREW_PREFIX/opt/cjson/lib/libcjson.1.dylib" \
                          @rpath/libcjson.1.dylib \
                          "$PAYLOAD_PLUGIN" 2>/dev/null || true
# Re-sign after all install_name_tool ops to avoid SIGKILL at dlopen.
codesign --force --sign - "$PAYLOAD_PLUGIN" 2>/dev/null || true

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

# --- Service binary + LaunchAgent (macOS spatial shell, #61) ---
# The installed spatial shell (DisplayXRShell .pkg) attaches to displayxr-service
# as an IPC client. Ship the service (built HYBRID by build_macos.sh --installer)
# and a system LaunchAgent that auto-starts it in the user's GUI session at login
# — the macOS parallel of Windows' HKLM\...\Run autostart. Present only when the
# artifact came from a SERVICE build; a non-service .pkg ships runtime-only.
SERVICE_SRC="$ARTIFACT_DIR/bin/displayxr-service"
if [ -f "$SERVICE_SRC" ]; then
    mkdir -p "$RUNTIME_ROOT/bin"
    cp "$SERVICE_SRC" "$RUNTIME_ROOT/bin/displayxr-service"
    chmod 0755 "$RUNTIME_ROOT/bin/displayxr-service"
    # Re-sign defensively (a tar round-trip in CI can drop the signature).
    codesign --force --sign - "$RUNTIME_ROOT/bin/displayxr-service" 2>/dev/null || true
    # ADR-053 P2: the conversion-host satellite, spawned by the service from bin/.
    if [ -f "$ARTIFACT_DIR/bin/displayxr-conversion-host" ]; then
        cp "$ARTIFACT_DIR/bin/displayxr-conversion-host" "$RUNTIME_ROOT/bin/displayxr-conversion-host"
        chmod 0755 "$RUNTIME_ROOT/bin/displayxr-conversion-host"
        codesign --force --sign - "$RUNTIME_ROOT/bin/displayxr-conversion-host" 2>/dev/null || true
    fi

    LAUNCHAGENT_DIR="$WORK_DIR/payload-runtime/Library/LaunchAgents"
    mkdir -p "$LAUNCHAGENT_DIR"
    # The service's bundled Vulkan loader needs the runtime's MoltenVK ICD (in a
    # private path the loader doesn't search), so the agent sets VK_ICD_FILENAMES.
    # ProcessType Interactive + the gui/<uid> bootstrap (postinstall) put it in
    # the Aqua session so its menu-bar status item shows.
    #
    # KeepAlive = {SuccessfulExit = false}: relaunch only on a crash (non-zero
    # exit / signal death), NOT on a clean quit. Both user-quit paths exit 0 —
    # Cmd+Q calls exit(0), and the menu-bar "Quit" raises SIGTERM which the
    # service catches and shuts down gracefully (main_loop returns 0). Plain
    # KeepAlive=true would respawn the service the instant the user quit it.
    cat > "$LAUNCHAGENT_DIR/com.displayxr.service.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.displayxr.service</string>
    <key>ProgramArguments</key>
    <array>
        <string>$INSTALL_PREFIX/bin/displayxr-service</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>VK_ICD_FILENAMES</key>
        <string>$INSTALL_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json</string>
        <key>VK_DRIVER_FILES</key>
        <string>$INSTALL_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>StandardOutPath</key>
    <string>/tmp/displayxr-service.out.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/displayxr-service.err.log</string>
</dict>
</plist>
PLIST
    echo "Staged displayxr-service + LaunchAgent into the runtime payload."
else
    echo "Note: displayxr-service not in artifact (non-SERVICE build); .pkg ships runtime only."
fi

# --- Desktop-capture helper + on-demand LaunchAgent (#2264) ---
# DisplayXRDesktopCapture.app is the ONE DisplayXR process that holds the macOS
# Screen Recording grant: a transparent session on a weaving DP (Leia) composes
# the desktop behind its window, and the in-process Metal compositor gets those
# frames from this helper over XPC (Mach service com.displayxr.desktop-capture).
# Unlike the service it ships on every .pkg — an in-process app needs it too.
#
# The agent has NO RunAtLoad: launchd starts the helper only when a compositor
# connects to the Mach service, and the helper exits by itself once the last
# client leaves. Opaque sessions never contact it, so a user who never runs a
# transparent app on a Leia panel never sees the permission prompt.
#
# Signing is load-bearing for the grant: macOS pins an ad-hoc signature's grant
# to its cdhash, so an unsigned (CI/contributor) .pkg loses the grant on every
# reinstall. The Developer ID signature from sign-tree below (when
# DXR_MACOS_APP_IDENTITY is set) pins it to bundle id + team instead, so it
# survives upgrades. The helper needs no entitlements: Screen Recording is a
# TCC grant, not an entitlement.
CAPTURE_SRC="$ARTIFACT_DIR/bin/DisplayXRDesktopCapture.app"
if [ -d "$CAPTURE_SRC" ]; then
    mkdir -p "$RUNTIME_ROOT/bin"
    rm -rf "$RUNTIME_ROOT/bin/DisplayXRDesktopCapture.app"
    cp -R "$CAPTURE_SRC" "$RUNTIME_ROOT/bin/"
    LAUNCHAGENT_DIR="$WORK_DIR/payload-runtime/Library/LaunchAgents"
    mkdir -p "$LAUNCHAGENT_DIR"
    cat > "$LAUNCHAGENT_DIR/com.displayxr.desktop-capture.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.displayxr.desktop-capture</string>
    <key>Program</key>
    <string>$INSTALL_PREFIX/bin/DisplayXRDesktopCapture.app/Contents/MacOS/DisplayXRDesktopCapture</string>
    <key>MachServices</key>
    <dict>
        <key>com.displayxr.desktop-capture</key>
        <true/>
    </dict>
    <key>LimitLoadToSessionType</key>
    <string>Aqua</string>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>StandardErrorPath</key>
    <string>/tmp/displayxr-desktop-capture.log</string>
</dict>
</plist>
PLIST
    echo "Staged DisplayXRDesktopCapture.app + on-demand LaunchAgent into the runtime payload."
else
    echo "Warning: DisplayXRDesktopCapture.app not in artifact; transparent sessions on a weaving DP will have no desktop backdrop."
fi

# --- 1b. Final code signature on the runtime payload (#280) ---
# MUST stay the last thing that touches the payload's Mach-Os: every
# install_name_tool above invalidated a signature (the ad-hoc re-signs only
# keep the intermediate tree loadable). With DXR_MACOS_APP_IDENTITY set this is
# a Developer ID + hardened-runtime signature, entitlements from
# installer/macos/entitlements/map; unset, it is the ad-hoc '-' signature
# contributor and fork-PR builds always had.
SIGN="$REPO_ROOT/scripts/macos_sign_notarize.sh"

# sign_tree <dir>: the final Mach-O signature. With DXR_MACOS_SIGN_FALLBACK=adhoc
# (set by CI on signed tag builds) a failed Developer ID signature — typically
# Apple's timestamp server being unreachable — degrades to the ad-hoc tree
# instead of failing the build: signing never gates publishing. The fallback is
# recorded in $DXR_MACOS_SIGN_STATUS_FILE so CI skips notarizing a tree that
# can no longer pass it.
sign_tree() {
    if "$SIGN" sign-tree "$1" --entitlements-map "$SCRIPT_DIR/entitlements/map"; then
        return 0
    fi
    if [ -n "${DXR_MACOS_APP_IDENTITY:-}" ] && [ "${DXR_MACOS_SIGN_FALLBACK:-}" = "adhoc" ]; then
        echo "::warning::Developer ID signing of $(basename "$1") failed — falling back to ad-hoc; this .pkg will NOT be notarized"
        [ -n "${DXR_MACOS_SIGN_STATUS_FILE:-}" ] && echo "adhoc-fallback" >> "$DXR_MACOS_SIGN_STATUS_FILE"
        DXR_MACOS_APP_IDENTITY="" "$SIGN" sign-tree "$1" --entitlements-map "$SCRIPT_DIR/entitlements/map"
        return $?
    fi
    return 1
}

# Every shipped Mach-O must resolve its deps on a Mac without Homebrew: an
# absolute build-box path (Homebrew prefix, /usr/local, /tmp, a home dir)
# loads on the build box and fails everywhere else. displayxr-conversion-host
# shipped naming /opt/homebrew/.../libvulkan.1.dylib for exactly that reason.
BAD_DEPS=""
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    d="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/homebrew|/usr/local|/tmp|/private/tmp|/Users)/' || true)"
    [ -n "$d" ] && BAD_DEPS="$BAD_DEPS
  ${f#"$WORK_DIR/payload-runtime"}: $(echo $d)"
done < <(find "$WORK_DIR/payload-runtime" -type f -print0)
if [ -n "$BAD_DEPS" ]; then
    echo "Error: payload Mach-Os depend on build-box paths:$BAD_DEPS" >&2
    exit 1
fi

sign_tree "$WORK_DIR/payload-runtime"

# --- 2. Build runtime component pkg ---
echo "--- Building runtime component ---"
# pkgbuild marks every bundle under --root relocatable by default, and a
# relocatable bundle is installed OVER any other copy of the same bundle id that
# Installer finds on the disk (a dev build tree, a moved copy) instead of into
# the payload path. DisplayXRDesktopCapture.app (#2264) is such a bundle and its
# LaunchAgent names the payload path, so pin every bundle in place.
COMPONENT_PLIST="$WORK_DIR/runtime-components.plist"
pkgbuild --analyze --root "$WORK_DIR/payload-runtime" "$COMPONENT_PLIST" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$COMPONENT_PLIST" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$COMPONENT_PLIST"
    i=$((i + 1))
done

pkgbuild --root "$WORK_DIR/payload-runtime" \
    --component-plist "$COMPONENT_PLIST" \
    --scripts "$SCRIPT_DIR/scripts/runtime" \
    --identifier com.displayxr.runtime \
    --version "$VERSION" \
    --install-location / \
    "$WORK_DIR/runtime.pkg"

# --- 3. Build .app bundle for test app ---
echo "--- Building .app bundle ---"
if [ -f "$ARTIFACT_DIR/bin/cube_handle_vk_macos" ]; then
    "$SCRIPT_DIR/create_app_bundle.sh" "$ARTIFACT_DIR" "$WORK_DIR/DisplayXRCube.app" cube_handle_vk_macos
    # Sign the bundle's nested Mach-Os, then the bundle itself (#280).
    sign_tree "$WORK_DIR/DisplayXRCube.app"

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

# --- 6. Sign the product archive, notarize, staple (#280) ---
# Each step runs only when its credentials are in the environment
# (DXR_MACOS_INSTALLER_IDENTITY, DXR_NOTARY_PROFILE or DXR_NOTARY_KEY_*);
# with none set the .pkg stays unsigned, exactly as before.
"$SIGN" finalize-pkg "$OUTPUT_PKG"

echo "=== Installer built: $OUTPUT_PKG ==="
ls -lh "$OUTPUT_PKG"
