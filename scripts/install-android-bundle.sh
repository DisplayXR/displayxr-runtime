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
#   ./scripts/install-android-bundle.sh --links         # print tap-ready download
#                                                       # URLs (no device needed)
#
# --links exists because the people who most need a correct install are the ones
# who CANNOT run this script: a non-developer holding only a tablet, who gets a
# list of links pasted into chat. Hand-writing that list is how runtime and
# browser drift apart, and a mismatched pair blacks out all 3D in the browser
# while every other app keeps working (runtime#1302). Generate the list; never
# retype it.
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
WITH_BROWSER=0; VARIANT=Leia; FORCE=(); LIST_ONLY=0; LINKS_ONLY=0
for a in "$@"; do
  case "$a" in
    --with-browser)    WITH_BROWSER=1 ;;
    --neutral)         VARIANT=neutral ;;
    --force-reinstall) FORCE=(--force-reinstall) ;;
    --list)            LIST_ONLY=1 ;;
    --links)           LINKS_ONLY=1; WITH_BROWSER=1 ;;
    -h|--help)         sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

command -v gh  >/dev/null || { echo "gh CLI required (https://cli.github.com)"; exit 1; }
if [ "$LINKS_ONLY" != 1 ]; then
  command -v adb >/dev/null || ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
  ADB="${ADB:-adb}"; command -v "$ADB" >/dev/null || { echo "adb not found"; exit 1; }
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# versions.json: local checkout if present, else straight from GitHub.
# --links emits URLs that get pasted to other people, so it always reads the
# PUBLISHED pins: a local checkout can be an old branch, and the stale links it
# would print are indistinguishable from correct ones at the far end. (Caught
# in review: a branch based on an older main emitted runtime v2.15.1 while main
# pinned v2.15.2.) Installing locally still prefers the checkout.
if [ -f "$HERE/versions.json" ] && [ "$LINKS_ONLY" != 1 ]; then
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

# --links: resolve each pin to its real release asset and print download URLs.
# Ordered runtime-first, with the launch-once step called out between runtime
# and apps, because that order is itself a requirement (install-android.sh).
if [ "$LINKS_ONLY" = 1 ]; then
  asset_url() { # repo tag pattern -> browser_download_url of the first match
    gh release view "$2" -R "$1" --json assets \
      --jq "[.assets[].name | select(test(\"$3\"))] | .[0] // empty" 2>/dev/null \
      | while read -r n; do [ -n "$n" ] && echo "https://github.com/$1/releases/download/$2/$n"; done
  }
  echo
  echo "=== Matched Android install set (generated from versions.json — do not hand-edit) ==="
  echo
  RT_PAT="DisplayXR-Runtime-$([ "$VARIANT" = Leia ] && echo 'Leia-' || echo '')[0-9].*android-arm64[.]apk"
  echo "1. Runtime $RUNTIME_TAG ($VARIANT)"
  echo "   $(asset_url "$REPO_RUNTIME" "$RUNTIME_TAG" "$RT_PAT")"
  echo
  echo "2. Open the \"DisplayXR\" app once before installing anything else."
  echo "   (Skipping this leaves every app failing with XR_ERROR_RUNTIME_UNAVAILABLE.)"
  echo
  echo "3. Allow: Settings > Apps > DisplayXR > Display over other apps."
  echo
  echo "4. Apps:"
  for c in "${COMPONENTS[@]}"; do
    IFS='|' read -r NAME REPO FIELD <<< "$c"
    TAG=$(pin "$FIELD"); [ -n "$TAG" ] || continue
    U=$(asset_url "$REPO" "$TAG" "[.]apk$")
    [ -n "$U" ] && printf "   %-14s %s\n" "$NAME" "$U" \
                || printf "   %-14s (no APK on %s)\n" "$NAME" "$TAG"
  done
  echo
  echo "Requires on-device Leia services 0.10.54 or newer (0.10.56+ recommended)."
  echo "The runtime and browser above are a MATCHED PAIR — the runtime<->browser"
  echo "IPC check is an exact version match, so do not mix in versions from"
  echo "another message or all 3D content in the browser renders black."
  exit 0
fi

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
    # Exclude the RUNTIME APK by exact path, not by an "android-arm64.apk$" suffix.
    # That suffix is not runtime-specific: the browser publishes as
    # DisplayXR-Browser-Preview-X.Y.Z-android-arm64.apk, so the old filter dropped it and
    # the browser was SILENTLY never installed -- the download SUCCEEDED, so the
    # desktop-only fallback below never fired either. Demos were unaffected (their names
    # carry no such suffix), which is why this went unnoticed.
    A=$(ls -t "$WORK"/apk/*.apk | command grep -vF "$RUNTIME_APK" | head -1)
    [ -n "$A" ] && { APPS+=("$A"); echo "   $NAME: $(basename "$A")"; }
  else
    # The pinned release may be desktop-only. The browser is the live case:
    # only preview-0.1.17 ever published an Android APK, so every later pin
    # (0.1.18+) would silently install nothing. Fall back to the newest
    # release that actually HAS one, and say so — an older-but-present
    # browser beats "--with-browser installed no browser".
    FB_TAG=""; FB_ASSET=""
    for t in $(gh release list -R "$REPO" --limit 15 --json tagName -q '.[].tagName' 2>/dev/null); do
      a=$(gh release view "$t" -R "$REPO" --json assets \
            --jq '[.assets[].name|select(endswith(".apk"))]|first // empty' 2>/dev/null)
      [ -n "$a" ] && { FB_TAG="$t"; FB_ASSET="$a"; break; }
    done
    if [ -n "$FB_TAG" ]; then
      echo "   $NAME: $TAG publishes no Android APK — falling back to $FB_TAG"
      gh release download "$FB_TAG" -R "$REPO" -p "$FB_ASSET" -D "$WORK/apk" --clobber 2>/dev/null || true
      A="$WORK/apk/$FB_ASSET"
      [ -f "$A" ] && { APPS+=("$A"); echo "   $NAME: $FB_ASSET (from $FB_TAG, older than the $TAG pin)"; }
    else
      echo "   $NAME: no release in the last 15 publishes an Android APK — skipped"
    fi
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
