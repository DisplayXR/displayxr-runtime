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
#
# A blank platform glob means "warn-and-skip on this platform" — used today
# for shell/leia/mcp on macOS.
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
# displayxr-mcp ships a Windows installer today; macOS artifact is a future
# component. Empty macOS glob → warn+skip.
COMPONENT_REPO_mcp_tools="DisplayXR/displayxr-mcp"
COMPONENT_PKG_MACOS_mcp_tools=""
COMPONENT_EXE_WINDOWS_mcp_tools="DisplayXRMCPSetup-*.exe"
COMPONENT_INSTALL_MARKER_MACOS_mcp_tools=""
COMPONENT_INSTALL_MARKER_WINDOWS_mcp_tools="HKLM\\Software\\DisplayXR\\Capabilities\\MCP"

# Helper: look up a per-component field for the current platform.
#   $1 = component name (runtime, shell, leia_plugin, mcp_tools)
#   $2 = field (REPO, PKG_MACOS, EXE_WINDOWS, INSTALL_MARKER_MACOS)
# Prints the value (may be empty).
component_field() {
    local var="COMPONENT_${2}_${1}"
    printf '%s' "${!var-}"
}
