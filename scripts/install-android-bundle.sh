#!/usr/bin/env bash
# ============================================================
# DisplayXR — one-command Android install (the bundle analogue)
# ============================================================
# The Windows/macOS/Linux "bundle" installer (DisplayXRBundle-*.exe/.pkg/
# .tar.gz) has no Android counterpart: Android ships one APK per component,
# each on its own GitHub release. This script is that counterpart — it
# DOWNLOADS every component at the versions.json pin and installs them.
#
#   install-android.sh        installs APKs you already have (explicit paths)
#   install-android-bundle.sh fetches them first, then calls the above
#
# Usage:
#   ./scripts/install-android-bundle.sh                 # runtime + all demos
#   ./scripts/install-android-bundle.sh --with-browser  # + DisplayXR Browser
#   ./scripts/install-android-bundle.sh --neutral       # vendor-neutral runtime
#   ./scripts/install-android-bundle.sh --force-reinstall
#   ./scripts/install-android-bundle.sh --list          # show what WOULD install
#
# --force-reinstall UNINSTALLS first, which WIPES APP DATA — including
# EarthView's saved Google Maps API key. Without it, installs are `adb
# install -r` upgrades and app data (the key) is preserved.
#
# Needs: gh (authenticated), adb, a connected device. No clone required —
# fetches versions.json from GitHub when run outside a checkout.
#
# Env: DXR_DEVICE (adb serial), DXR_REF (versions.json ref, default main)
# ============================================================
set -euo pipefail

REPO_RUNTIME=DisplayXR/displayxr-runtime
REF="${DXR_REF:-main}"
WITH_BROWSER=0; VARIANT=Leia; FORCE=(); LIST_ONLY=0
for a in "$@"; do
  case "$a" in
    --with-browser)    WITH_BROWSER=1 ;;
    --neutral)         VARIANT=neutral ;;
    --force-reinstall) FORCE=(--force-reinstall) ;;
    --list)            LIST_ONLY=1 ;;
    -h|--help)         sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

command -v gh  >/dev/null || { echo "gh CLI required (https://cli.github.com)"; exit 1; }
command -v adb >/dev/null || ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
ADB="${ADB:-adb}"; command -v "$ADB" >/dev/null || { echo "adb not found"; exit 1; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# versions.json: local checkout if present, else straight from GitHub.
if [ -f "$HERE/versions.json" ]; then
  cp "$HERE/versions.json" "$WORK/versions.json"; SRC="local checkout"
else
  gh api "repos/$REPO_RUNTIME/contents/versions.json?ref=$REF" --jq '.content' | base64 -d > "$WORK/versions.json"
  SRC="$REPO_RUNTIME@$REF"
fi
pin() { python3 -c "import json,sys;print(json.load(open('$WORK/versions.json')).get(sys.argv[1],''))" "$1"; }

RUNTIME_TAG=$(pin runtime)
echo "DisplayXR Android install — pins from $SRC"
echo "  runtime            $RUNTIME_TAG   (variant: $VARIANT)"

# component  repo                              versions.json field
COMPONENTS=(
  "modelviewer|DisplayXR/displayxr-demo-modelviewer|modelviewer_demo"
  "mediaplayer|DisplayXR/displayxr-demo-mediaplayer|mediaplayer_demo"
  "gaussiansplat|DisplayXR/displayxr-demo-gaussiansplat|gauss_demo"
  "earthview|DisplayXR/displayxr-demo-earthview|earthview_demo"
  "avatar|DisplayXR/displayxr-demo-avatar|avatar_demo"
)
[ "$WITH_BROWSER" = 1 ] && COMPONENTS+=("browser|DisplayXR/displayxr-browser|browser")

for c in "${COMPONENTS[@]}"; do
  IFS='|' read -r NAME REPO FIELD <<< "$c"
  printf "  %-18s %s\n" "$NAME" "$(pin "$FIELD")"
done
[ "$LIST_ONLY" = 1 ] && exit 0

echo
echo ">> downloading (this is the part install-android.sh does NOT do)"
mkdir -p "$WORK/apk"
# Runtime: the Leia variant carries the vendor plug-in (ADR-038); neutral does not.
PAT="*-${VARIANT}-*android-arm64.apk"; [ "$VARIANT" = neutral ] && PAT="DisplayXR-Runtime-*android-arm64.apk"
gh release download "$RUNTIME_TAG" -R "$REPO_RUNTIME" -p "$PAT" -D "$WORK/apk" --clobber
if [ "$VARIANT" = neutral ]; then rm -f "$WORK"/apk/*-Leia-*.apk; fi
RUNTIME_APK=$(ls "$WORK"/apk/*android-arm64.apk | head -1)
echo "   runtime: $(basename "$RUNTIME_APK")"

APPS=()
for c in "${COMPONENTS[@]}"; do
  IFS='|' read -r NAME REPO FIELD <<< "$c"
  TAG=$(pin "$FIELD")
  [ -z "$TAG" ] && { echo "   $NAME: no pin — skipped"; continue; }
  if gh release download "$TAG" -R "$REPO" -p "*.apk" -D "$WORK/apk" --clobber 2>/dev/null; then
    A=$(ls -t "$WORK"/apk/*.apk | command grep -viE "android-arm64.apk$" | head -1)
    [ -n "$A" ] && { APPS+=("$A"); echo "   $NAME: $(basename "$A")"; }
  else
    echo "   $NAME: $TAG has no .apk asset — skipped (not published for Android)"
  fi
done

echo
echo ">> installing via install-android.sh (launch-once + permission grants included)"
INSTALLER="$HERE/scripts/install-android.sh"
[ -x "$INSTALLER" ] || { INSTALLER="$WORK/install-android.sh"
  gh api "repos/$REPO_RUNTIME/contents/scripts/install-android.sh?ref=$REF" --jq '.content' | base64 -d > "$INSTALLER"
  chmod +x "$INSTALLER"; }
"$INSTALLER" "${FORCE[@]}" "$RUNTIME_APK" "${APPS[@]}"

cat <<'DONE'

Done. One manual step remains and it matters:

  OPEN THE "DisplayXR" APP ONCE.  (install-android.sh already did this for you,
  but do it again after any reboot-and-reinstall.) That single launch arms the
  OpenXR broker AND the runtime's IPC service; skipping it makes every app fail
  with XR_ERROR_RUNTIME_UNAVAILABLE, and makes the DisplayXR Browser's inline-3D
  render flat 2D with no error on screen.

Then launch any demo — Model Viewer is the fastest 3D sanity check.
DONE
