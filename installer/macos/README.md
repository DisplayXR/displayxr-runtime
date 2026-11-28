# DisplayXR macOS Installer

## Install

**Double-click** `DisplayXR-Installer.pkg` to launch the macOS installer wizard, or install from the command line:

```bash
sudo installer -pkg DisplayXR-Installer.pkg -target /
```

### What gets installed

| Component | Location |
|-----------|----------|
| DisplayXR Runtime | `/Library/Application Support/DisplayXR/` |
| OpenXR runtime registration | `/etc/xdg/openxr/1/active_runtime.json` |
| DisplayXR Dashboard | `/Applications/DisplayXR Dashboard.app` (part of the runtime component; needs the .NET 10 SDK at build time) |
| displayxr-cli | `/Library/Application Support/DisplayXR/bin/displayxr-cli` (linked from `/usr/local/bin`) |
| SimCube OpenXR Test App | `/Applications/SimCubeOpenXR.app` |

## Run the Test App

Double-click **SimCubeOpenXR** in `/Applications`, or from the terminal:

```bash
open /Applications/SimCubeOpenXR.app
```

By default it uses anaglyph (red-cyan) output. To change:

```bash
SIM_DISPLAY_OUTPUT=sbs open /Applications/SimCubeOpenXR.app
```

Options: `anaglyph`, `sbs`, `blend`

## Uninstall

```bash
sudo "/Library/Application Support/DisplayXR/uninstall.sh"
```

This removes the runtime, the DisplayXR Dashboard, the test app, the OpenXR registration, and installer receipts.

## Build the Installer Locally

```bash
# 1. Build the runtime and test app
./scripts/build_macos.sh

# 2. Create the _package directory (mimics CI artifact layout)
#    See .github/workflows/build-macos.yml for the full packaging steps

# 3. Build the .pkg installer
./installer/macos/build_installer.sh _package/DisplayXR-macOS DisplayXR-Installer.pkg
```
