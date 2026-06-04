#!/usr/bin/env bash
# Component → repo + per-platform asset-glob table.
#
# Sourced by scripts/setup-displayxr.sh (#283). Future user-meta-installer
# scripts in displayxr-installer (#284) should source the same file so the
# repo list and asset name patterns stay in lockstep with versions.json's
# tag pins.
#
# Each component is described by parallel variables keyed by component name:
#   COMPONENT_REPO_<name>                     GitHub repo (owner/name)
#   COMPONENT_PKG_MACOS_<name>                macOS asset glob, or "" if no macOS asset today
#   COMPONENT_EXE_WINDOWS_<name>              Windows asset glob, or "" if no Windows asset today
#   COMPONENT_INSTALL_MARKER_MACOS_<name>     Absolute file path that must exist after a
#                                             successful install on macOS. Empty = skip check.
#   COMPONENT_INSTALL_MARKER_WINDOWS_<name>   Registry key whose existence proves a
#                                             successful install on Windows. Empty = skip check.
#   COMPONENT_PIN_KEY_<name>                  versions.json key holding this component's tag,
#                                             when it differs from <name> (e.g. nested under
#                                             "demos"). Empty = the pin key equals <name>.
#
# A blank platform glob means "warn-and-skip on this platform" — used today
# for shell/leia/mcp on macOS.
#
# Demos are *installed from their prebuilt release asset*, exactly like
# runtime/shell/leia/mcp — never built from source. So `--with-demos` is
# insensitive to a contributor's build environment (Vulkan SDK, graphics
# toolchains, …): the binary was already compiled by the demo's own CI.
# Demos that ship no release installer are source-only and belong under
# `--with-demo-sources` (clone), not in DEMO_COMPONENTS below.
#
# `scripts/setup-displayxr.bat` mirrors these tables inline at the top of the
# file (Windows batch can't source bash). Keep both in sync when bumping
# components or release contracts.

# Bash 3.x ships on macOS; associative arrays need 4.x. Use parallel key-prefix
# vars instead so the file works under /bin/bash without forcing a brew bash.

# --- runtime ---
COMPONENT_REPO_runtime="DisplayXR/displayxr-runtime"
COMPONENT_PKG_MACOS_runtime="DisplayXR-Installer-*.pkg"
COMPONENT_EXE_WINDOWS_runtime="DisplayXRSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_runtime="/Library/Application Support/DisplayXR/DisplayProcessors/200-sim-display.json"
COMPONENT_INSTALL_MARKER_WINDOWS_runtime="HKLM\\Software\\DisplayXR\\Runtime"

# --- shell ---
# macOS shell port deferred per CLAUDE.md M6. Empty macOS glob → warn+skip.
COMPONENT_REPO_shell="DisplayXR/displayxr-shell-releases"
COMPONENT_PKG_MACOS_shell=""
COMPONENT_EXE_WINDOWS_shell="DisplayXRShellSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_shell=""
COMPONENT_INSTALL_MARKER_WINDOWS_shell="HKLM\\Software\\DisplayXR\\WorkspaceControllers\\shell"

# --- leia_plugin ---
# Leia SR display processor is Windows-only by design (vendor SDK is
# Windows-only). Empty macOS glob → warn+skip.
COMPONENT_REPO_leia_plugin="DisplayXR/displayxr-leia-plugin"
COMPONENT_PKG_MACOS_leia_plugin=""
COMPONENT_EXE_WINDOWS_leia_plugin="DisplayXRLeiaSRSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_leia_plugin=""
COMPONENT_INSTALL_MARKER_WINDOWS_leia_plugin="HKLM\\Software\\DisplayXR\\DisplayProcessors\\leia-sr"

# --- mcp_tools ---
# displayxr-mcp ships a Windows installer (NSIS) and a macOS .pkg
# (productbuild). On macOS the postinstall flips the capability bit at
# /Library/Application Support/DisplayXR/Capabilities/MCP/Enabled —
# that's also the marker setup-displayxr.sh checks for installer success.
COMPONENT_REPO_mcp_tools="DisplayXR/displayxr-mcp"
COMPONENT_PKG_MACOS_mcp_tools="DisplayXRMCP-*.pkg"
COMPONENT_EXE_WINDOWS_mcp_tools="DisplayXRMCPSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_mcp_tools="/Library/Application Support/DisplayXR/Capabilities/MCP/Enabled"
COMPONENT_INSTALL_MARKER_WINDOWS_mcp_tools="HKLM\\Software\\DisplayXR\\Capabilities\\MCP"

# --- gauss_demo ---
# Gaussian-splat viewer demo. macOS .pkg first shipped in
# displayxr-demo-gaussiansplat v1.4.0 (2026-05-24, #311). Both installers
# (macOS .pkg + Windows .exe) are now built + attached by CI on a v* tag
# (demo #14). Note: the macOS install marker has a space in the path —
# consumers must quote when testing existence.
COMPONENT_REPO_gauss_demo="DisplayXR/displayxr-demo-gaussiansplat"
COMPONENT_PKG_MACOS_gauss_demo="DisplayXRGaussianSplat-*.pkg"
COMPONENT_EXE_WINDOWS_gauss_demo="DisplayXRGaussianSplatSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_gauss_demo="/Applications/Gaussian Splat Viewer.app"
COMPONENT_INSTALL_MARKER_WINDOWS_gauss_demo="HKLM\\Software\\DisplayXR\\Demos\\GaussianSplat"
# Pin key defaults to the component name (top-level "gauss_demo" in
# versions.json) — no COMPONENT_PIN_KEY override needed for the flat schema.

# Demo components installed by --with-demos (prebuilt release assets only).
# Space-separated; add a demo here once it ships a release installer that CI
# attaches on a v* tag. The `setup-displayxr.bat` mirror keeps its own copy of
# this list — keep both in sync.
DEMO_COMPONENTS="gauss_demo modelviewer_demo mediaplayer_demo"

# --- modelviewer_demo ---
# glTF 2.0 PBR model viewer demo (displayxr-demo-modelviewer). Windows-only
# today — the macOS app is not yet ported, so the macOS glob/marker are empty
# → warn+skip on macOS. First release v0.1.0 (2026-06-01).
COMPONENT_REPO_modelviewer_demo="DisplayXR/displayxr-demo-modelviewer"
COMPONENT_PKG_MACOS_modelviewer_demo=""
COMPONENT_EXE_WINDOWS_modelviewer_demo="DisplayXRModelViewerSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_modelviewer_demo=""
COMPONENT_INSTALL_MARKER_WINDOWS_modelviewer_demo="HKLM\\Software\\DisplayXR\\Demos\\ModelViewer"

# --- mediaplayer_demo ---
# Stereo media player demo (displayxr-demo-mediaplayer). Plays SBS image/video
# on the 3D display via the OpenXR extension wire protocol — no vendor SR SDK.
# Both installers (macOS .pkg + Windows .exe) are built + attached by CI on a
# v* tag. First release v1.0.0. Note: the macOS install marker has a space in
# the path — consumers must quote when testing existence.
COMPONENT_REPO_mediaplayer_demo="DisplayXR/displayxr-demo-mediaplayer"
COMPONENT_PKG_MACOS_mediaplayer_demo="DisplayXRMediaPlayer-*.pkg"
COMPONENT_EXE_WINDOWS_mediaplayer_demo="DisplayXRMediaPlayerSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_mediaplayer_demo="/Applications/Stereo Media Player.app"
COMPONENT_INSTALL_MARKER_WINDOWS_mediaplayer_demo="HKLM\\Software\\DisplayXR\\Demos\\MediaPlayer"

# Helper: look up a per-component field for the current platform.
#   $1 = component name (runtime, shell, leia_plugin, mcp_tools, gauss_demo)
#   $2 = field (REPO, PKG_MACOS, EXE_WINDOWS, INSTALL_MARKER_MACOS, PIN_KEY)
# Prints the value (may be empty).
component_field() {
    local var="COMPONENT_${2}_${1}"
    printf '%s' "${!var-}"
}
