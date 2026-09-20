#!/usr/bin/env bash
# =============================================================================
# DisplayXR — run the OpenXR CTS against the dev runtime build (Linux).
#
# Linux port of scripts/run_cts.ps1 (#1527 / #1523). Same pinned CTS, same test
# specs, same output names, same exclusivity envs — so a Linux arm's result
# file drops into the same submission package as a Windows one.
#
#   ./scripts/run_cts.sh -g vulkan2 --scope smoke --xvfb --software \
#       --quarantine-list scripts/cts_quarantine_software_tier.txt
#   ./scripts/run_cts.sh -g vulkan --scope full            # real-GPU box, real X session
#
# WHAT IS *NOT* PORTED, and why — the .ps1 is ~600 lines and most of them are
# Windows-only ceremony:
#
#   * No registry. The Khronos loader ignores XR_RUNTIME_JSON in an ELEVATED
#     process, which is the entire reason run_cts.ps1 snapshots and rewrites
#     HKLM ActiveRuntime, the API-layer keys and the Vulkan ICD/layer keys.
#     Linux has no such downgrade: XR_RUNTIME_JSON, XR_API_LAYER_PATH,
#     VK_ADD_LAYER_PATH and VK_DRIVER_FILES are honoured as-is. Everything this
#     script sets is PROCESS env, so there is no machine state to restore and
#     no finally block that can leave the box mis-pointed.
#
#   * No interactive categories. Those need an operator at a 3D panel; that
#     procedure stays Windows-side (docs/reference/cts-interactive-procedure.md).
#
# WHAT *IS* Linux-specific:
#
#   * DISPLAY. The VK native compositor presents to an X11/XCB surface
#     (comp_vk_native_window_xcb.c). The CTS app passes no window binding, so
#     the runtime SELF-CREATES the window — which needs an X server, not a
#     window manager: xcb_map_window works on a bare server, and FOCUSED does
#     not depend on X input focus (oxr_session_gfx_vk_native.c sets
#     compositor_focused unconditionally for the in-process native path, so the
#     SYNCHRONIZED -> VISIBLE -> FOCUSED ladder turns purely on frame
#     submission). --xvfb starts a throwaway Xvfb and tears it down on exit.
#
#   * DXR_WINDOW_FULLSCREEN=0, NOT XRT_COMPOSITOR_START_WINDOWED. The latter is
#     read in exactly one place, comp_d3d11_window.cpp — it is a D3D11 knob and
#     setting it on Linux does nothing at all. The Linux equivalent is
#     DXR_WINDOW_FULLSCREEN, read by comp_vk_native_window_xcb.c. Without a WM
#     the EWMH _NET_WM_STATE_FULLSCREEN property and the
#     _NET_WM_FULLSCREEN_MONITORS client message have nobody
#     listening, so the fullscreen request is a silent no-op. Turning it off
#     explicitly keeps the window at the size the compositor asked for and
#     keeps the log honest instead of claiming a fullscreen that never happened.
#
#   * Runtime logs go to STDERR, not to a file. u_file_logging is Windows-only
#     (%LOCALAPPDATA%\DisplayXR), so the graphics-identity scrape reads the
#     captured stderr rather than a log directory.
# =============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

GRAPHICS="vulkan"
SCOPE=""
TEST_SPEC=""
API_VERSION="1.1"
PLUGIN="sim-display"
TAG=""
TIMEOUT_SEC=1800
QUARANTINE_LIST=""
SOFTWARE=OFF
CONFORMANCE_LAYER=OFF
USE_XVFB=OFF
XVFB_GEOMETRY="${XVFB_GEOMETRY:-1920x1080x24}"
OUT_DIR="${TMPDIR:-/tmp}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
INTERACTION_PROFILE=""

usage() { sed -n '2,45p' "$0"; }

while [ $# -gt 0 ]; do
  case "$1" in
    -g|--graphics)        GRAPHICS="$2"; shift 2 ;;
    --scope)              SCOPE="$2"; shift 2 ;;
    --test-spec)          TEST_SPEC="$2"; shift 2 ;;
    --api-version)        API_VERSION="$2"; shift 2 ;;
    --plugin)             PLUGIN="$2"; shift 2 ;;
    --tag)                TAG="$2"; shift 2 ;;
    --timeout)            TIMEOUT_SEC="$2"; shift 2 ;;
    --quarantine-list)    QUARANTINE_LIST="$2"; shift 2 ;;
    --interaction-profile) INTERACTION_PROFILE="$2"; shift 2 ;;
    --out-dir)            OUT_DIR="$2"; shift 2 ;;
    --build-dir)          BUILD_DIR="$2"; shift 2 ;;
    --xvfb-geometry)      XVFB_GEOMETRY="$2"; shift 2 ;;
    --software)           SOFTWARE=ON; shift ;;
    --conformance-layer)  CONFORMANCE_LAYER=ON; shift ;;
    --xvfb)               USE_XVFB=ON; shift ;;
    -h|--help)            usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ---- test spec ---------------------------------------------------------------
# Byte-identical to the strings cts.yml resolves for the Windows arms; an arm
# that ran a different slice is not comparable with one that did not. The full
# lane runs the whole non-interactive suite with nothing excluded by name.
#
# Catch2 trap, same as the .ps1: patterns inside ONE filter are ANDed (every
# m_required matches, no m_forbidden matches); a COMMA starts a SECOND filter
# and filters are OR'd — so an exclusion appended after a comma excludes
# nothing. Append "~name" with NO comma.
if [ -z "$TEST_SPEC" ]; then
  case "${SCOPE:-full}" in
    smoke)
      TEST_SPEC='Swapchains,SessionState,ValidateEnvironment,validApiLayer,XR_KHR_visibility_mask,XR_KHR_composition_layer_cylinder,multithreading,[actions]~[interactive]'
      ;;
    full)
      TEST_SPEC='exclude:[interactive]'
      ;;
    *) echo "ERROR: --scope must be smoke or full (got '$SCOPE')" >&2; exit 2 ;;
  esac
fi

[ -n "$TAG" ] || TAG="${GRAPHICS}_${API_VERSION}"
STEM="cts_$TAG"
XML="$OUT_DIR/$STEM.xml"
CONSOLE="$OUT_DIR/${STEM}_console.log"
STDOUT_LOG="$OUT_DIR/${STEM}_stdout.log"
# conformance_cli prints its frame-timing block on STDOUT, not through the
# Catch2 console reporter — same split as Windows, same reason the scrape reads
# the stdout log.
RUNTIME_LOG="$OUT_DIR/${STEM}_runtime.log"
IDENTITY="$OUT_DIR/${STEM}_graphics_identity.txt"

mkdir -p "$OUT_DIR"
rm -f "$XML" "$CONSOLE" "$STDOUT_LOG" "$RUNTIME_LOG" "$IDENTITY"

# ---- locate the binaries -----------------------------------------------------
CTS_BUILD="$ROOT/build-cts/build"
CTS_EXE="$(find "$CTS_BUILD" -type f -name conformance_cli -perm -u+x 2>/dev/null | head -1)"
DEV_MANIFEST="$BUILD_DIR/openxr_displayxr-dev.json"
PLUGIN_DIR="$BUILD_DIR/_plugins"

for p in "$CTS_EXE" "$DEV_MANIFEST"; do
  if [ -z "$p" ] || [ ! -e "$p" ]; then
    echo "ERROR: missing: ${p:-conformance_cli (not found under $CTS_BUILD)}" >&2
    exit 1
  fi
done

# ---- quarantine list -> Catch2 `~name` exclusions ----------------------------
# ONLY the software tier gets one, exactly as on Windows: a real-GPU tier runs
# with an empty exclusion set by construction, so an entry in that file can
# never hide a defect where there is hardware to expose it. A missing file is a
# hard error, so a typo'd path can never silently mean "no exclusions".
if [ -n "$QUARANTINE_LIST" ]; then
  if [ "$SOFTWARE" != "ON" ]; then
    echo "QUARANTINE: --quarantine-list given without --software — IGNORED (real-GPU tiers run the full set)"
  elif [ ! -f "$QUARANTINE_LIST" ]; then
    echo "ERROR: quarantine list not found: $QUARANTINE_LIST" >&2
    exit 1
  else
    n=0
    while IFS= read -r line; do
      name="$(printf '%s' "$line" | sed 's/#.*$//' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
      [ -n "$name" ] || continue
      case "$name" in
        *[\ ,\[\]~\"]*) TEST_SPEC="${TEST_SPEC}~\"${name}\"" ;;
        *)              TEST_SPEC="${TEST_SPEC}~${name}" ;;
      esac
      echo "QUARANTINE: excluding '$name'"
      n=$((n + 1))
    done < "$QUARANTINE_LIST"
    if [ "$n" -eq 0 ]; then
      echo "QUARANTINE: $QUARANTINE_LIST is empty — nothing excluded by name"
    else
      echo "QUARANTINE: $n test(s) excluded from $QUARANTINE_LIST"
    fi
  fi
fi

# ---- X server ----------------------------------------------------------------
XVFB_PID=""
cleanup() {
  if [ -n "$XVFB_PID" ]; then
    kill "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if [ "$USE_XVFB" = "ON" ]; then
  command -v Xvfb >/dev/null 2>&1 || { echo "ERROR: --xvfb given but Xvfb is not installed (apt: xvfb)" >&2; exit 1; }
  XVFB_DISPLAY="${XVFB_DISPLAY:-:99}"
  # +extension RANDR is not the default on every Xvfb build, and the XCB window
  # helper resolves its target monitor through xcb_randr_get_monitors — without
  # RandR it logs "no RandR monitor at (x, y)" and falls back to windowed
  # placement. Harmless here (no WM to honour fullscreen anyway), but the
  # warning reads like a failure, so just provide the extension.
  Xvfb "$XVFB_DISPLAY" -screen 0 "$XVFB_GEOMETRY" +extension RANDR -nolisten tcp >"$OUT_DIR/${STEM}_xvfb.log" 2>&1 &
  XVFB_PID=$!
  export DISPLAY="$XVFB_DISPLAY"
  # Wait for the server to accept connections rather than sleeping a guess.
  for _ in $(seq 1 50); do
    if command -v xdpyinfo >/dev/null 2>&1; then
      xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
    else
      [ -e "/tmp/.X11-unix/X${XVFB_DISPLAY#:}" ] && break
    fi
    sleep 0.2
  done
  if ! kill -0 "$XVFB_PID" 2>/dev/null; then
    echo "ERROR: Xvfb exited immediately — log:" >&2
    cat "$OUT_DIR/${STEM}_xvfb.log" >&2 || true
    exit 1
  fi
  echo "XVFB: $DISPLAY ($XVFB_GEOMETRY), pid $XVFB_PID"
fi

if [ -z "${DISPLAY:-}" ]; then
  echo "ERROR: DISPLAY is unset and --xvfb was not given. The Linux compositor presents to an" >&2
  echo "       X11/XCB surface; with no X server xcb_connect fails and every session-creating" >&2
  echo "       test errors out. Pass --xvfb, or run inside an X session." >&2
  exit 1
fi

# ---- software Vulkan ICD (lavapipe) ------------------------------------------
# The hosted runner has no GPU. Mesa's lavapipe ships as
# /usr/share/vulkan/icd.d/lvp_icd.<arch>.json in mesa-vulkan-drivers; pinning
# VK_DRIVER_FILES to it makes the choice explicit AND excludes any other ICD
# that happens to be installed, so a result file can never be ambiguous about
# which implementation produced it (#1525's lesson: a CTS XML records the
# graphics PLUGIN, never the renderer that answered it).
ICD_JSON=""
if [ "$SOFTWARE" = "ON" ]; then
  for cand in \
      /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
      /usr/share/vulkan/icd.d/lvp_icd.aarch64.json \
      /usr/local/share/vulkan/icd.d/lvp_icd.x86_64.json; do
    [ -f "$cand" ] && { ICD_JSON="$cand"; break; }
  done
  if [ -z "$ICD_JSON" ]; then
    ICD_JSON="$(ls /usr/share/vulkan/icd.d/lvp_icd.*.json 2>/dev/null | head -1)"
  fi
  if [ -z "$ICD_JSON" ]; then
    echo "ERROR: --software given but no lavapipe ICD manifest found under /usr/share/vulkan/icd.d/." >&2
    echo "       Install mesa-vulkan-drivers." >&2
    exit 1
  fi
  # Both spellings: VK_DRIVER_FILES is the modern name, VK_ICD_FILENAMES the
  # legacy one older loaders still read. Setting only the new one on a loader
  # that predates it silently selects nothing.
  export VK_DRIVER_FILES="$ICD_JSON"
  export VK_ICD_FILENAMES="$ICD_JSON"
  # lavapipe is a CPU rasterizer; let it use every core the runner has.
  export LP_NUM_THREADS="${LP_NUM_THREADS:-$(nproc 2>/dev/null || echo 2)}"
  echo "SOFTWARE ICD: $ICD_JSON (LP_NUM_THREADS=$LP_NUM_THREADS)"
fi

# ---- the CTS's API layers ----------------------------------------------------
# Two OpenXR layers: XR_APILAYER_KHRONOS_runtime_conformance (requested with -L)
# and the conformance_test layer (validApiLayer requests it itself). Discover
# each manifest instead of hardcoding a path — openxr-cts-1.1.44+ moved the JSON
# copy from conformance_cli's binary dir to each layer's own TARGET_FILE_DIR, and
# under Ninja Multi-Config the library lands in a per-config subdirectory. Keep
# only the manifest whose declared library actually exists next to it, so a
# layout change surfaces as a loader error rather than a silent skip.
XR_LAYER_DIRS=""
CONF_LAYER_LIB=""
resolve_layer_manifest() { # $1 = json basename -> echoes the manifest path
  local want="$1" json lib dir
  while IFS= read -r json; do
    dir="$(dirname "$json")"
    lib="$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$json" | head -1)"
    [ -n "$lib" ] || continue
    case "$lib" in
      /*) : ;;
      *)  lib="$dir/$lib" ;;
    esac
    if [ -f "$lib" ]; then
      printf '%s\t%s\n' "$json" "$lib"
      return 0
    fi
  done < <(find "$CTS_BUILD" -type f -name "$want" 2>/dev/null)
  return 1
}

if [ "$CONFORMANCE_LAYER" = "ON" ]; then
  for want in XrApiLayer_runtime_conformance.json XrApiLayer_conformance_test_layer.json; do
    hit="$(resolve_layer_manifest "$want" || true)"
    if [ -z "$hit" ]; then
      echo "ERROR: could not locate a usable $want (with its library beside it) under $CTS_BUILD" >&2
      exit 1
    fi
    j="${hit%%$'\t'*}"; l="${hit##*$'\t'}"
    XR_LAYER_DIRS="${XR_LAYER_DIRS:+$XR_LAYER_DIRS:}$(dirname "$j")"
    [ "$want" = "XrApiLayer_runtime_conformance.json" ] && CONF_LAYER_LIB="$l"
    echo "RESOLVED layer manifest: $j -> $l"
  done
  export XR_API_LAYER_PATH="$XR_LAYER_DIRS"
fi

# ---- the CTS's VULKAN conformance layer --------------------------------------
# XR_APILAYER_KHRONOS_runtime_conformance ships a second face, the Vulkan layer
# VK_LAYER_OPENXR_xr_runtime_conformance, implemented in the same library. When
# the OpenXR layer is enabled the CTS's Vulkan graphics plugin HARD-REQUIRES it
# (graphics_plugin_vulkan.cpp XRC_CHECK_THROWs on `it != availableLayers.end()`),
# so every session-creating test in both Linux arms dies before doing real work
# if the Vulkan loader cannot see it. Nothing puts the generated manifest on the
# loader's search path, and under Ninja Multi-Config its relative library_path
# points at a directory the library is not in — so write our own copy carrying an
# ABSOLUTE path, exactly as run_cts.ps1 does.
VK_LAYER_DIR=""
if [ "$CONFORMANCE_LAYER" = "ON" ] && [ -n "$CONF_LAYER_LIB" ]; then
  VK_LAYER_DIR="$OUT_DIR/dxr_cts_vk_layer"
  mkdir -p "$VK_LAYER_DIR"
  cat > "$VK_LAYER_DIR/VkLayer_OPENXR_xr_runtime_conformance.json" <<EOF
{
    "file_format_version": "1.0.0",
    "layer": {
        "name": "VK_LAYER_OPENXR_xr_runtime_conformance",
        "type": "GLOBAL",
        "library_path": "$CONF_LAYER_LIB",
        "api_version": "1.0.0",
        "implementation_version": "1",
        "description": "API Layer to validate OpenXR runtime conformance",
        "disable_environment": { "OPENXR_xr_runtime_conformance_disabled": "1" }
    }
}
EOF
  echo "WROTE Vulkan layer manifest: $VK_LAYER_DIR/VkLayer_OPENXR_xr_runtime_conformance.json"
fi

# VK_LAYER_DXR_queue_lock (#902): the runtime injects this layer for the
# shared-queue late-weave repaint path on single-graphics-queue devices —
# lavapipe is exactly that shape. Same dev-tree manifest the generated
# run_*.sh scripts point at.
DXR_VK_LAYER_DIR="$BUILD_DIR/src/xrt/targets/vk_layer"
VK_ADD=""
[ -n "$VK_LAYER_DIR" ] && VK_ADD="$VK_LAYER_DIR"
[ -d "$DXR_VK_LAYER_DIR" ] && VK_ADD="${VK_ADD:+$VK_ADD:}$DXR_VK_LAYER_DIR"
[ -n "$VK_ADD" ] && export VK_ADD_LAYER_PATH="$VK_ADD"

# `~implicit~`, NOT `*`: the blanket form also hides EXPLICIT layers from
# vkEnumerateInstanceLayerProperties, including the CTS's own
# VK_LAYER_OPENXR_xr_runtime_conformance that the Vulkan plugin hard-requires.
export VK_LOADER_LAYERS_DISABLE="~implicit~"

# ---- runtime + plug-in selection ---------------------------------------------
export XR_RUNTIME_JSON="$DEV_MANIFEST"
[ -d "$PLUGIN_DIR" ] && export XRT_PLUGIN_SEARCH_PATH="$PLUGIN_DIR"
# Reduce per-instance overhead/noise: the CTS creates hundreds of instances and
# MCP spins a named-pipe server per instance. Not under test.
export DISPLAYXR_MCP=0
# #1545: run the lane EXCLUSIVE — the named display plug-in is the only one
# loaded, and input providers are not discovered at all. On POSIX, discovery is
# XRT_PLUGIN_SEARCH_PATH + JSON manifests (docs/specs/runtime/plugin-discovery.md),
# so there is no ProbeOrder to lower; DXR_PLUGIN_EXCLUSIVE does the whole job.
if [ "$PLUGIN" != "none" ]; then
  export DXR_PLUGIN_EXCLUSIVE="$PLUGIN"
fi
export DXR_INPUT_PROVIDERS=0
# No window manager under Xvfb (and none needed): see the header note.
export DXR_WINDOW_FULLSCREEN="${DXR_WINDOW_FULLSCREEN:-0}"

echo "RUNTIME:  XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
echo "PLUGINS:  XRT_PLUGIN_SEARCH_PATH=${XRT_PLUGIN_SEARCH_PATH:-(unset)} DXR_PLUGIN_EXCLUSIVE=${DXR_PLUGIN_EXCLUSIVE:-(unset)} DXR_INPUT_PROVIDERS=$DXR_INPUT_PROVIDERS"
echo "DISPLAY:  $DISPLAY"

# ---- run ---------------------------------------------------------------------
# OpenXR-CTS renamed --apiVersion -> --minApiVersion in openxr-cts-1.1.44+; the
# old spelling is rejected as an unrecognised token and conformance_cli exits
# before running a single test (#726).
CLI_ARGS=("$TEST_SPEC" -G "$GRAPHICS" --minApiVersion "$API_VERSION"
          --reporter "ctsxml::out=$XML" --reporter "console::out=$CONSOLE")
[ "$CONFORMANCE_LAYER" = "ON" ] && CLI_ARGS+=(-L XR_APILAYER_KHRONOS_runtime_conformance)
# Always pass -I explicitly, never rely on the CTS default: the default is
# injected into globalData AFTER Options is snapshotted, so the ctsxml
# <cts:enabledInteractionProfiles> element comes out EMPTY.
[ -n "$INTERACTION_PROFILE" ] && CLI_ARGS+=(-I "${INTERACTION_PROFILE#/interaction_profiles/}")

CTS_EXE_DIR="$(dirname "$CTS_EXE")"
echo "RUN: conformance_cli ${CLI_ARGS[*]}"
echo "CWD: $CTS_EXE_DIR"

# CWD must be the exe's own dir: the CTS loads its data assets (brdf_lut.png,
# *.glb, ...) relative to the working directory (#830).
RC=0
if [ "$TIMEOUT_SEC" -gt 0 ] 2>/dev/null && command -v timeout >/dev/null 2>&1; then
  ( cd "$CTS_EXE_DIR" && timeout --signal=TERM --kill-after=30s "${TIMEOUT_SEC}s" \
      "$CTS_EXE" "${CLI_ARGS[@]}" >"$STDOUT_LOG" 2>"$RUNTIME_LOG" ) || RC=$?
  if [ "$RC" = "124" ] || [ "$RC" = "137" ]; then
    echo "TIMEOUT after ${TIMEOUT_SEC}s - killed."
  fi
else
  ( cd "$CTS_EXE_DIR" && "$CTS_EXE" "${CLI_ARGS[@]}" >"$STDOUT_LOG" 2>"$RUNTIME_LOG" ) || RC=$?
fi
echo "EXITCODE: $RC"

# Replay conformance_cli's stdout into the job log in ONE write (piping 40k+
# lines line-by-line takes minutes).
[ -s "$STDOUT_LOG" ] && cat "$STDOUT_LOG"

# ---- graphics identity -------------------------------------------------------
# "Every result file records the renderer/device that produced it." A CTS XML
# records the graphics PLUGIN, never the implementation that answered it, so a
# lavapipe run and a run on a real GPU are indistinguishable after the fact.
{
  echo "tag:        $TAG"
  echo "graphics:   $GRAPHICS (OpenXR $API_VERSION)"
  echo "plugin:     $PLUGIN"
  echo "exclusive:  DXR_PLUGIN_EXCLUSIVE=${DXR_PLUGIN_EXCLUSIVE:-} DXR_INPUT_PROVIDERS=${DXR_INPUT_PROVIDERS:-}"
  echo "display:    DISPLAY=$DISPLAY  xvfb=$USE_XVFB  geometry=${XVFB_GEOMETRY}"
  if [ "$SOFTWARE" = "ON" ]; then
    echo "software:   lavapipe via $ICD_JSON"
    sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/            icd library_path: \1/p' "$ICD_JSON" | head -1
  else
    echo "software:   (none — running against whatever ICD the box provides)"
  fi
  if command -v vulkaninfo >/dev/null 2>&1; then
    echo "vulkaninfo --summary:"
    vulkaninfo --summary 2>/dev/null | sed -n '1,40p' | sed 's/^/  /'
  else
    echo "vulkaninfo: (not installed — apt: vulkan-tools)"
  fi
  # The runtime's own identity line, from the captured stderr (u_file_logging
  # is Windows-only, so there is no log directory to sweep on Linux).
  echo "renderer:"
  if [ -s "$RUNTIME_LOG" ]; then
    grep -aE 'Vulkan selected GPU|XCB: created|no RandR monitor|render adapter: no adapter survived' "$RUNTIME_LOG" |
      sed 's/^[[:space:]]*\[[^]]*\][[:space:]]*//' | sort -u | head -10 | sed 's/^/  /'
  else
    echo "  (no runtime stderr captured)"
  fi
} > "$IDENTITY" 2>&1

echo "--- graphics identity ---"
cat "$IDENTITY"

echo "XML:      $XML"
echo "CONSOLE:  $CONSOLE"
echo "STDOUT:   $STDOUT_LOG"
echo "RUNTIME:  $RUNTIME_LOG"
echo "IDENTITY: $IDENTITY"
