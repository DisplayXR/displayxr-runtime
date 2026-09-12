#!/usr/bin/env bash
# android-oem-probe.sh -- OEM/ODM acceptance probe for the DisplayXR Android
# platform requirements in docs/specs/vendor/oem-android-platform-requirements.md.
#
# Run it on a bench unit over adb. It takes about a minute, is READ-ONLY with
# respect to device configuration -- it never installs, uninstalls, roots, writes
# a setting or a system property -- and it force-stops every app it launched
# before it exits.
#
#   ./scripts/android-oem-probe.sh                     # every sub-check
#   ./scripts/android-oem-probe.sh --only r6           # one sub-check
#   ./scripts/android-oem-probe.sh --only r6,s9 --app com.example.my3dapp
#   ./scripts/android-oem-probe.sh --list
#
# Each ask prints its raw evidence and one of:
#   PASS   the platform satisfies the ask, measured
#   FAIL   the platform does not, with the measured value  -> exit 1
#   INFO   reported, not gated (or not measurable read-only)
#   ERROR  the check could not be run at all               -> exit 2
#
# The only sub-check that needs a human is the R6/S9 mini-window step: it drives
# the app into the OEM's recents-card freeform window by TAPPING the card's
# freeform icon, and the icon's position is a launcher-layout constant. The
# defaults below were measured on the reference device; the screencap taken just
# before the tap is always saved, so a miss can be re-aimed with --tap X,Y.
#
# Requirements: adb, awk. Optional: aapt2 (Android build-tools) for the CNSDK
# stamps in the header -- without it they are reported "not checked", never guessed.

set -u

DEFAULT_APP=com.displayxr.model_viewer_vk_android
RUNTIME_PKG=org.freedesktop.monado.openxr_runtime.out_of_process
# Informational only; absence is never a failure (a non-Leia vendor has its own).
VENDOR_PKGS="com.leialoft.display.config com.leia.headtrackingservice"

APP=$DEFAULT_APP
ONLY=
TAP=
OUTDIR=
SERIAL=${ANDROID_SERIAL:-}
LAUNCH_WAIT=12
SETTLE=7

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

list_checks() {
    cat <<'EOF'
sub-checks (--only takes a comma-separated list; default: all)

  env   Environment header: model, Android version, panel size + rotation,
        installed runtime versionName and CNSDK loader stamps.  (always runs)
  r6    REQUIRED R6 -- 1:1 panel pixels.  Launches --app, drives it into the OEM
        mini-window via the recents-card freeform icon, and measures the task
        leash transform, the app layer's displayFrame vs its buffer, the
        composition type and the WM task bounds against the panel.
  s9    STRONGLY REC. S9 -- is the container scale visible to the app?  Reuses
        r6's measurement; reports every app-visible source probed.  Always INFO.
  r8    REQUIRED R8 -- process/service policy for the runtime service: resident,
        foreground-service type, oom adj, freezer state, plus kill-ranking inputs
        and recent SIGNALED exits.  R8.5 (kill ranking) needs the soak in the spec;
        a PASS here covers R8.1-R8.4 only.
  s4    STRONGLY REC. S4 -- ADPF / PowerHAL hint sessions.
  s2    STRONGLY REC. S2 -- per-pixel click-through.  Partly manual; reports the
        read-only half (untrusted-touch opacity, ActivityRecordInputSink).
  s6    STRONGLY REC. S6 -- atomic window move + drag affordance.  Manual.
EOF
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --only) ONLY="$2"; shift 2 ;;
        --app) APP="$2"; shift 2 ;;
        --tap) TAP="$2"; shift 2 ;;
        --out) OUTDIR="$2"; shift 2 ;;
        --serial|-s) SERIAL="$2"; shift 2 ;;
        --list) list_checks ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1  (--help)" >&2; exit 2 ;;
    esac
done

command -v adb >/dev/null 2>&1 || { echo "adb not found." >&2; exit 2; }
ADB="adb"; [ -n "$SERIAL" ] && ADB="adb -s $SERIAL"
$ADB get-state >/dev/null 2>&1 || { echo "no device (serial='${SERIAL:-<any>}')." >&2; exit 2; }

[ -n "$OUTDIR" ] || OUTDIR=$(mktemp -d /tmp/dxr-oem-probe.XXXXXX)
mkdir -p "$OUTDIR"

want() {
    [ -z "$ONLY" ] && return 0
    case ",$ONLY," in *,"$1",*) return 0 ;; esac
    return 1
}

# --- result table -----------------------------------------------------------
RESULTS=""
FAILS=0
ERRORS=0
record() {  # record <id> <verdict> <one-line summary>
    RESULTS="${RESULTS}$1|$2|$3
"
    case "$2" in
        FAIL) FAILS=$((FAILS + 1)) ;;
        ERROR) ERRORS=$((ERRORS + 1)) ;;
    esac
}
hdr() { printf '\n=== %s ===\n' "$1"; }
ev()  { printf '  %s\n' "$*"; }        # evidence line
kv()  { printf '  %-34s %s\n' "$1" "$2"; }

sh_() { $ADB shell "$@" 2>/dev/null | tr -d '\r'; }

# --- shared device state ----------------------------------------------------
PANEL_W=; PANEL_H=; ROT=
read_display() {
    local cur
    cur=$(sh_ dumpsys window displays | grep -m1 -oE 'cur=[0-9]+x[0-9]+' | cut -d= -f2)
    PANEL_W=${cur%x*}; PANEL_H=${cur#*x}
    ROT=$(sh_ dumpsys window displays | grep -m1 -oE 'mRotation=[0-9]' | cut -d= -f2)
}

LAUNCHED=          # packages this script started, force-stopped on exit
cleanup() {
    local p
    for p in $LAUNCHED; do $ADB shell am force-stop "$p" >/dev/null 2>&1; done
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# ENVIRONMENT HEADER
# ---------------------------------------------------------------------------
find_aapt2() {
    ls -d "$HOME"/Library/Android/sdk/build-tools/*/aapt2 \
          "${ANDROID_HOME:-/nonexistent}"/build-tools/*/aapt2 \
          "${ANDROID_SDK_ROOT:-/nonexistent}"/build-tools/*/aapt2 2>/dev/null | sort -V | tail -1
}
AAPT2=$(find_aapt2)

pkg_version() { sh_ dumpsys package "$1" | grep -m1 versionName | tr -d ' ' | cut -d= -f2; }

# stamp <pkg> <meta-data name> -> value ("" absent, "?" unreadable)
# Android 13 does not print <meta-data> in `dumpsys package`, so the honest read
# is to pull the installed APK and parse its manifest on the host.
stamp() {
    local p t out
    [ -n "$AAPT2" ] || { echo "?"; return; }
    p=$(sh_ pm path "$1" | sed 's/^package://' | head -1)
    [ -n "$p" ] || { echo "?"; return; }
    t=$(mktemp -d); $ADB pull "$p" "$t/base.apk" >/dev/null 2>&1 || { rm -rf "$t"; echo "?"; return; }
    out=$("$AAPT2" dump xmltree --file AndroidManifest.xml "$t/base.apk" 2>/dev/null \
          | grep -A1 "\"$2\"" | grep -oE ':value\([^)]*\)="[^"]*"' | head -1 | sed -E 's/.*="([^"]*)"/\1/')
    rm -rf "$t"; echo "$out"
}

check_env() {
    hdr "Environment"
    read_display
    kv "device"          "$(sh_ getprop ro.product.model) ($(sh_ getprop ro.product.device)), serial ${SERIAL:-$($ADB get-serialno 2>/dev/null | tr -d '\r')}"
    kv "android"         "$(sh_ getprop ro.build.version.release) (SDK $(sh_ getprop ro.build.version.sdk)), build $(sh_ getprop ro.build.id)"
    kv "panel (physical)" "$(sh_ wm size | sed 's/^Physical size: //')  density $(sh_ wm density | sed 's/^Physical density: //')"
    case "$ROT" in
        1|3) kv "display now"  "${PANEL_W}x${PANEL_H}  mRotation=$ROT  (LANDSCAPE)" ;;
        *)   kv "display now"  "${PANEL_W}x${PANEL_H}  mRotation=$ROT  (portrait -- note: an app with a fixed portrait orientation drives the display here)" ;;
    esac
    kv "runtime $RUNTIME_PKG" "$(pkg_version $RUNTIME_PKG)"
    if [ -n "$AAPT2" ]; then
        kv "  CNSDK_LOADER_VERSION" "$(stamp $RUNTIME_PKG com.displayxr.CNSDK_LOADER_VERSION)"
        kv "  CNSDK_LOADER_BUILD"   "$(stamp $RUNTIME_PKG com.displayxr.CNSDK_LOADER_BUILD)"
    else
        kv "  CNSDK loader stamps" "NOT CHECKED (no aapt2 on this host; set ANDROID_HOME)"
    fi
    local v
    for v in $VENDOR_PKGS; do
        local pv; pv=$(pkg_version "$v")
        [ -n "$pv" ] && kv "vendor $v" "$pv"
    done
    kv "app under test"  "$APP  $(pkg_version "$APP" | sed 's/^/v/')"
    kv "artifacts"       "$OUTDIR"
}

# ---------------------------------------------------------------------------
# R6 + S9 -- the mini-window measurement
# ---------------------------------------------------------------------------
R6_STATE=            # unmeasured | measured
R6_SCALE_X=; R6_SCALE_Y=; R6_SWAP=no
R6_TASKID=; R6_TASK_TR=; R6_DF=; R6_BUF=; R6_FCC=; R6_WMB=; R6_LAYER=

# Default recents freeform-icon tap, measured on the reference device. The card
# layout -- and therefore the icon -- moves with the recents orientation and with
# how many cards are stacked, so both are read first. Override with --tap X,Y;
# the pre-tap screencap is saved either way.
default_tap() {  # default_tap <shot_w> <shot_h> <ncards>
    if [ "$1" -lt "$2" ]; then                     # recents drawn portrait
        [ "$3" -ge 2 ] && echo "964,256" || echo "1150,475"
    else                                            # recents drawn landscape
        [ "$3" -ge 2 ] && echo "1505,215" || echo "1895,302"
    fi
}

png_size() {  # png_size <file> -> "W H"
    od -An -tu1 -j16 -N8 "$1" 2>/dev/null | awk '{printf "%d %d\n", $1*16777216+$2*65536+$3*256+$4, $5*16777216+$6*65536+$7*256+$8}'
}

# Parse `dumpsys SurfaceFlinger` once. Emits, for the task leash and for the
# app's own buffer layer:
#   TASKTR <flags>        the task leash's geomLayerTransform, verbatim
#   TASKSC <sx> <sy>      its scale factors
#   TASKDF <l> <t> <r> <b> <fcc>
#   APP <dfl> <dft> <dfr> <dfb> <scw> <sch> <fcc> <name...>
# The app's buffer layer is identified structurally -- the composited layer with a
# non-degenerate source crop whose display frame lies inside the task's -- so this
# works for any app, whatever its layer is called.
parse_sf() {  # parse_sf <task-id> < dumpsys-SurfaceFlinger
    awk -v tid="$1" '
    # --- layer tree: the task leash transform ---
    /^\* Layer .*\(Task=/ {
        want = ($0 ~ ("\\(Task=" tid "#")); if (want) { tstate = 1 } next
    }
    tstate == 1 && /geomLayerTransform/ {
        line = $0; sub(/.*geomLayerTransform /, "", line); print "TASKTR " line
        getline; sx = $1; getline; sy = $2
        printf "TASKSC %.6f %.6f\n", sx, sy
        tstate = 0; next
    }
    # --- composition state: display frames + source crops ---
    /^  - Output Layer 0x/ {
        name = $0; sub(/.*0x[0-9a-f]+\(/, "", name); sub(/\)$/, "", name); cur = name; next
    }
    cur != "" && /forceClientComposition=/ {
        fcc = "?"; if ($0 ~ /forceClientComposition=true/) fcc = "true"; else fcc = "false"
        df = $0; sub(/.*displayFrame=\[/, "", df); sub(/\].*/, "", df); split(df, D, " ")
        sc = $0; sub(/.*sourceCrop=\[/, "", sc); sub(/\].*/, "", sc); split(sc, S, " ")
        if (cur ~ ("^Task=" tid "#")) {
            printf "TASKDF %d %d %d %d %s\n", D[1], D[2], D[3], D[4], fcc
            tl = D[1]; tt = D[2]; tr = D[3]; tb = D[4]; havetask = 1
        } else {
            w = S[3] - S[1]; h = S[4] - S[2]
            if (w > 0 && h > 0) {
                n++; N[n] = cur; DL[n] = D[1]; DT[n] = D[2]; DR[n] = D[3]; DB[n] = D[4]
                SW[n] = w; SH[n] = h; FC[n] = fcc
            }
        }
        cur = ""; next
    }
    END {
        best = 0; barea = -1
        for (i = 1; i <= n; i++) {
            if (havetask && (DL[i] < tl - 2 || DT[i] < tt - 2 || DR[i] > tr + 2 || DB[i] > tb + 2)) continue
            a = SW[i] * SH[i]; if (a > barea) { barea = a; best = i }
        }
        if (best) printf "APP %d %d %d %d %d %d %s %s\n", DL[best], DT[best], DR[best], DB[best], SW[best], SH[best], FC[best], N[best]
    }'
}

check_r6() {
    hdr "R6 -- 1:1 panel pixels in the OEM mini-window (REQUIRED, PLATFORM)"
    if ! sh_ pm path "$APP" | grep -q package:; then
        ev "$APP is not installed -- pass an installed resizable 3D app with --app."
        record R6 ERROR "app $APP not installed"
        return
    fi

    read_display
    ev "panel ${PANEL_W}x${PANEL_H}, mRotation=$ROT before launch"

    $ADB shell am force-stop "$APP" >/dev/null 2>&1
    LAUNCHED="$LAUNCHED $APP"
    $ADB shell monkey -p "$APP" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    sleep "$LAUNCH_WAIT"
    local pid0; pid0=$(sh_ pidof "$APP")
    if [ -z "$pid0" ]; then
        ev "$APP did not start."
        record R6 ERROR "$APP did not start"
        return
    fi
    ev "launched $APP (pid $pid0), fullscreen"

    # Into recents, screencap, then the device's own freeform affordance.
    $ADB shell input keyevent KEYCODE_HOME >/dev/null 2>&1; sleep 2
    $ADB shell input keyevent KEYCODE_APP_SWITCH >/dev/null 2>&1; sleep 3
    local shot="$OUTDIR/recents.png"
    $ADB exec-out screencap -p > "$shot" 2>/dev/null
    local sw sh_px; read -r sw sh_px <<EOF
$(png_size "$shot")
EOF
    local ncards; ncards=$(sh_ dumpsys activity recents | grep -cE '\* Recent #.*type=standard')
    local tap="$TAP"; [ -n "$tap" ] || tap=$(default_tap "${sw:-0}" "${sh_px:-1}" "${ncards:-1}")
    ev "recents: ${sw}x${sh_px} screencap, $ncards app card(s) -> tapping freeform icon at ${tap}  [saved $shot]"

    # Preserve anything S4 needs before clearing the buffer for the toggle check.
    $ADB logcat -d 2>/dev/null | tr -d '\r' | grep -oE 'ADPF: .*' > "$OUTDIR/adpf.txt" 2>/dev/null
    $ADB logcat -c >/dev/null 2>&1
    $ADB shell input tap "${tap%,*}" "${tap#*,}" >/dev/null 2>&1
    sleep "$SETTLE"

    local taskline
    taskline=$(sh_ dumpsys activity activities | grep -E "\* Task\{[0-9a-f]+ #[0-9]+ .*:$APP " | head -1)
    R6_TASKID=$(printf '%s' "$taskline" | grep -oE '#[0-9]+' | head -1 | tr -d '#')
    if ! printf '%s' "$taskline" | grep -q 'mode=freeform'; then
        ev "task did not enter freeform: $taskline"
        ev "The tap missed the recents card's freeform icon (or this device has no such"
        ev "affordance).  Open $shot, read off the icon's pixel position, and re-run with"
        ev "  --only r6 --tap X,Y"
        record R6 ERROR "freeform toggle did not fire -- re-aim with --tap (see $shot)"
        return
    fi
    local ntog; ntog=$($ADB logcat -d 2>/dev/null | tr -d '\r' | grep -c 'toggleSwitchFromFullScreenToFreeform')
    ev "task #$R6_TASKID is mode=freeform (WM logged the freeform toggle $ntog time(s))"

    read_display
    ev "display during the measurement: ${PANEL_W}x${PANEL_H}, mRotation=$ROT"

    # --- SurfaceFlinger ---
    local sf; sf="$OUTDIR/surfaceflinger.txt"
    sh_ dumpsys SurfaceFlinger > "$sf"
    local parsed; parsed=$(parse_sf "$R6_TASKID" < "$sf")
    printf '%s\n' "$parsed" > "$OUTDIR/sf-parsed.txt"

    R6_TASK_TR=$(printf '%s\n' "$parsed" | awk '/^TASKTR /{sub(/^TASKTR /,""); print; exit}')
    local tsx tsy
    tsx=$(printf '%s\n' "$parsed" | awk '/^TASKSC /{print $2; exit}')
    tsy=$(printf '%s\n' "$parsed" | awk '/^TASKSC /{print $3; exit}')
    local tdf tfcc
    tdf=$(printf '%s\n' "$parsed" | awk '/^TASKDF /{printf "[%s %s %s %s]", $2,$3,$4,$5; exit}')
    tfcc=$(printf '%s\n' "$parsed" | awk '/^TASKDF /{print $6; exit}')
    local appline; appline=$(printf '%s\n' "$parsed" | awk '/^APP /{print; exit}')
    if [ -z "$appline" ]; then
        ev "could not find the app's composited buffer layer under task #$R6_TASKID."
        record R6 ERROR "no app buffer layer found in SurfaceFlinger"
        return
    fi
    local dfl dft dfr dfb bw bh
    dfl=$(printf '%s' "$appline" | awk '{print $2}'); dft=$(printf '%s' "$appline" | awk '{print $3}')
    dfr=$(printf '%s' "$appline" | awk '{print $4}'); dfb=$(printf '%s' "$appline" | awk '{print $5}')
    bw=$(printf '%s' "$appline" | awk '{print $6}');  bh=$(printf '%s' "$appline" | awk '{print $7}')
    R6_FCC=$(printf '%s' "$appline" | awk '{print $8}')
    R6_LAYER=$(printf '%s' "$appline" | awk '{$1=$2=$3=$4=$5=$6=$7=$8=""; sub(/^ +/,""); print}')
    R6_DF="[$dfl $dft $dfr $dfb]"; R6_BUF="${bw}x${bh}"

    # displayFrame is in the display's CURRENT orientation while the buffer is in
    # the app's; when the two disagree the axes swap.  Take whichever pairing is
    # self-consistent (both axes agreeing to <1%).
    local dw dh; dw=$((dfr - dfl)); dh=$((dfb - dft))
    read -r R6_SCALE_X R6_SCALE_Y R6_SWAP <<EOF
$(awk -v dw="$dw" -v dh="$dh" -v bw="$bw" -v bh="$bh" 'BEGIN{
    ax = dw/bw; ay = dh/bh; a = (ax>ay?ax-ay:ay-ax)
    sx = dw/bh; sy = dh/bw; s = (sx>sy?sx-sy:sy-sx)
    if (s < a) printf "%.4f %.4f yes\n", sx, sy; else printf "%.4f %.4f no\n", ax, ay
}')
EOF

    # --- window manager ---
    local wm; wm="$OUTDIR/window.txt"
    sh_ dumpsys window windows > "$wm"
    R6_WMB=$(awk -v pkg="$APP" '
        /^  Window #[0-9]+ Window\{/ { inblk = (index($0, pkg) > 0); next }
        inblk && /mBounds=Rect\(/ { b = $0; sub(/.*mBounds=Rect\(/, "", b); sub(/\).*/, "", b); print "Rect(" b ")"; exit }
    ' "$wm")

    ev ""
    kv "SF task #$R6_TASKID leash transform" "${R6_TASK_TR:-<not found>}"
    kv "  leash scale (sx, sy)"        "${tsx:-?}, ${tsy:-?}"
    kv "  leash displayFrame"          "${tdf:-?}"
    kv "  leash forceClientComposition" "${tfcc:-?}$([ "$tfcc" = true ] && echo '   <- the resample is a GPU bilinear filter')"
    kv "app layer"                     "$R6_LAYER"
    kv "  displayFrame"                "$R6_DF  = ${dw}x${dh} on screen"
    kv "  buffer / geomLayerBounds"    "$R6_BUF"
    kv "  measured scale (x, y)"       "$R6_SCALE_X, $R6_SCALE_Y$([ "$R6_SWAP" = yes ] && echo '   (axes swapped: display and buffer orientations differ)')"
    kv "  forceClientComposition"      "$R6_FCC$([ "$R6_FCC" = true ] && echo '   <- the resample is a GPU bilinear filter')"
    kv "WM task bounds"                "${R6_WMB:-<not found>} against a ${PANEL_W}x${PANEL_H} panel"
    ev "(SurfaceFlinger reports every frame in NATURAL-orientation display coordinates,"
    ev " which is why the leash frame can look transposed against a landscape panel.)"
    R6_STATE=measured

    # --- verdict ---
    local verdict; verdict=$(awk -v x="$R6_SCALE_X" -v y="$R6_SCALE_Y" 'BEGIN{
        dx = (x>1?x-1:1-x); dy = (y>1?y-1:1-y)
        print (dx <= 0.005 && dy <= 0.005) ? "PASS" : "FAIL"
    }')
    local outside=no
    if [ -n "$R6_WMB" ]; then
        outside=$(printf '%s' "$R6_WMB" | tr -d 'Rect()' | awk -F'[,-]' -v pw="$PANEL_W" -v ph="$PANEL_H" '{
            gsub(/ /,""); print ($3 > pw || $4 > ph) ? "yes" : "no" }')
    fi
    ev ""
    if [ "$verdict" = PASS ]; then
        ev "PASS -- the app's buffer maps 1:1 to panel pixels in the mini window."
        record R6 PASS "scale 1.00/1.00, no leash SCALE, displayFrame == buffer"
    else
        ev "FAIL -- the whole task is presented through a scaled leash, so the woven"
        ev "       buffer is resampled after the app presents it.  Bilinear filtering of"
        ev "       an interlaced image is not invertible: the 3D is destroyed, and no"
        ev "       app-side pre-compensation exists at any precision."
        [ "$outside" = yes ] && ev "       WM bounds ${R6_WMB} exceed the ${PANEL_W}x${PANEL_H} panel -- the hybrid-bounds tell"
        ev "       (spec R6: acceptable fixes are (a) do not scale a resizable 3D app,"
        ev "        (b) expose the scale -- S9, or (c) compose the weave in the platform -- S8)"
        record R6 FAIL "scale ${R6_SCALE_X}/${R6_SCALE_Y} (want 1.00), leash forceClientComposition=$tfcc"
    fi
}

check_s9() {
    hdr "S9 -- is the container scale visible to the app? (STRONGLY REC., PLATFORM)"
    if [ "$R6_STATE" != measured ]; then
        ev "S9 reads R6's measurement; run it in the same invocation (--only r6,s9)."
        record S9 ERROR "no R6 measurement available"
        return
    fi
    local n
    n=$(grep -icE 'mGlobalScale|mCompatScale|mOverrideScale|sizeCompat' "$OUTDIR/window.txt")
    kv "dumpsys window: scale fields"  "$n occurrence(s)$([ "$n" -eq 0 ] && echo '   <- the server-side scale is not reported at all')"
    kv "WM bounds reported to the app" "${R6_WMB:-?}   (origin physical, size logical -- a hybrid)"
    kv "actual composited rect"        "$R6_DF"
    kv "effective scale, measured"     "$R6_SCALE_X, $R6_SCALE_Y"
    local sset; sset=$(sh_ settings list global | grep -iE 'freeform|resizab|window.*scale' | tr '\n' ' ')
    kv "settings global (freeform)"    "${sset:-<none>}"
    local sprop; sprop=$(sh_ getprop | grep -iE 'freeform|miniwindow|window.*scale' | tr '\n' ' ')
    kv "system properties"             "${sprop:-<none carrying the scale>}"
    local cs; cs=$($ADB logcat -d 2>/dev/null | tr -d '\r' | grep -oE 'CONTAINER_SCALED: .*' | tail -1 | cut -c1-140)
    [ -n "$cs" ] && kv "runtime's own inference"    "$cs"
    ev ""
    ev "INFO -- the composited rect $R6_DF is nowhere in any app-visible source probed."
    ev "     The runtime therefore INFERS the scale from the hybrid-bounds tell and applies"
    ev "     a per-device constant; one firmware change to the mini-window scale silently"
    ev "     breaks 3D in every mini window.  S9 asks for the number, not a capability."
    record S9 INFO "scale $R6_SCALE_X not exposed by any app-visible source ($n scale fields in dumpsys window)"
}

# ---------------------------------------------------------------------------
# R8 -- process / service policy
# ---------------------------------------------------------------------------
fgs_type_names() {  # decode the android:foregroundServiceType bitmask
    awk -v v="$1" 'BEGIN{
        n = split("1 dataSync 2 mediaPlayback 4 phoneCall 8 location 16 connectedDevice 32 mediaProjection 64 camera 128 microphone 256 health 512 remoteMessaging 1024 systemExempted 2048 shortService 4096 fileManagement 1073741824 specialUse", A, " ")
        out = ""
        for (i = 1; i < n; i += 2) if (int(v / A[i]) % 2 == 1) out = out (out == "" ? "" : "|") A[i+1]
        print (out == "" ? "<none>" : out)
    }'
}

check_r8() {
    hdr "R8 -- process, service and app-op policy (REQUIRED, PLATFORM)"
    local pid; pid=$(sh_ pidof "$RUNTIME_PKG")
    if [ -z "$pid" ]; then
        ev "$RUNTIME_PKG is not running.  The runtime service is started on demand by"
        ev "the first OpenXR app; launch one (or run --only r6 first) and re-check."
        record R8 INFO "runtime service not resident (nothing has started it)"
        return
    fi
    local svc; svc="$OUTDIR/services.txt"
    sh_ dumpsys activity services "$RUNTIME_PKG" > "$svc"
    local proc; proc="$OUTDIR/processes.txt"
    sh_ dumpsys activity processes > "$proc"

    local uid blk
    uid=$(awk -v p="$RUNTIME_PKG" '$0 ~ ("ProcessRecord\\{[0-9a-f]+ [0-9]+:" p "/u0a") { m=$0; sub(/.*\/u0a/,"",m); sub(/[^0-9].*/,"",m); print 10000+m; exit }' "$proc")
    blk=$(awk -v pid="$pid" '$0 ~ ("ProcessRecord\\{[0-9a-f]+ " pid ":") && /\*APP\* UID/ {f=1} f{print; n++} n>60{exit}' "$proc")

    kv "process"                   "pid $pid, uid ${uid:-?}, resident"
    kv "oom adj"                   "$(printf '%s\n' "$blk" | grep -m1 -oE 'oom adj: .*' || echo '?')"
    kv "proc state"                "$(printf '%s\n' "$blk" | grep -m1 -oE 'curProcState=[0-9]+ .*lastStateTime=[^ ]*' || echo '?')"
    kv "foreground services"       "$(printf '%s\n' "$blk" | grep -m1 -oE 'mHasForegroundServices=[a-z]+' || echo '?')"
    kv "freezer"                   "$(printf '%s\n' "$blk" | grep -m1 -oE 'isFreezeExempt=[a-z]+ isPendingFreeze=[a-z]+ isFrozen=[a-z]+' || echo '?')"
    local fgn; fgn=$(grep -c 'isForeground=true' "$svc")
    kv "FGS records with isForeground" "$fgn"
    if [ -n "$AAPT2" ]; then
        local ft; ft=$(fgs_manifest_type)
        kv "manifest foregroundServiceType" "${ft:-<none declared>}"
    else
        kv "manifest foregroundServiceType" "NOT CHECKED (no aapt2)"
    fi
    # R8.5 -- memory-pressure kill ranking.  A DIFFERENT mechanism from the freezer
    # above: the freezer acts at curAdj >= 900, whereas this is LMK/OOM selecting a
    # process that sits at adj 0 because it is large.  Report the inputs and the
    # recent kill history; the verdict needs a soak, not a snapshot.
    local adj score rss memavail memtotal
    adj=$(sh_ cat "/proc/$pid/oom_score_adj" 2>/dev/null | tr -d '\r')
    score=$(sh_ cat "/proc/$pid/oom_score" 2>/dev/null | tr -d '\r')
    rss=$(sh_ awk "/VmRSS/{print \$2}" "/proc/$pid/status" 2>/dev/null | tr -d '\r')
    memavail=$(sh_ awk '/MemAvailable/{print $2}' /proc/meminfo | tr -d '\r')
    memtotal=$(sh_ awk '/MemTotal/{print $2}' /proc/meminfo | tr -d '\r')
    kv "kill ranking"              "oom_score_adj=${adj:-?} oom_score=${score:-?} VmRSS=${rss:-?} kB"
    kv "memory headroom"           "MemAvailable=${memavail:-?} of MemTotal=${memtotal:-?} kB"

    # Deaths the snapshot cannot see.  A self-directed Process.killProcess looks
    # IDENTICAL to an external kill here (reason=2 SIGNALED status=9, no am_kill),
    # so this counts candidates and does not attribute them.
    local sigkills; sigkills=$(sh_ dumpsys activity exit-info "$RUNTIME_PKG" 2>/dev/null \
        | grep -c 'reason=2 (SIGNALED).*status=9' || true)
    kv "SIGNALED status=9 in exit-info" "${sigkills:-0}  (candidates; see note below)"

    local frozen=no
    case "$blk" in *isFrozen=true*) frozen=yes ;; esac
    ev ""
    if [ "$frozen" = yes ]; then
        ev "FAIL -- the runtime service is FROZEN.  A synchronous binder call into a frozen"
        ev "       process kills it; a frozen display/runtime service takes its clients down."
        record R8 FAIL "runtime service is frozen"
    elif [ "${sigkills:-0}" -gt 0 ]; then
        ev "FAIL -- ${sigkills} SIGNALED/status=9 exit(s) on record for $RUNTIME_PKG."
        ev "       R8.1-R8.4 look fine in this snapshot, but the service has been killed."
        ev "       Rule out a self-directed Process.killProcess from the service's own log"
        ev "       before attributing these to platform policy -- they are forensically"
        ev "       identical.  If they are external, this is R8.5 (kill ranking)."
        record R8 FAIL "R8.5: ${sigkills} SIGNALED status=9 exits on record (adj=${adj:-?} score=${score:-?})"
    else
        ev "PASS (R8.1-R8.4 only) -- resident, non-isolated, holding a foreground service,"
        ev "     not frozen, and no SIGNALED exits on record."
        ev ""
        ev "     R8.5 IS NOT TESTED BY THIS PROBE.  Kill ranking is time- and load-dependent;"
        ev "     a clean snapshot at MemAvailable=${memavail:-?} kB mostly means the killer"
        ev "     never ran.  The verdict needs the 30-minute soak under induced memory"
        ev "     pressure in the spec (§2 R8, 'Acceptance test for R8.5')."
        ev "     Note also that shrinking the process does not help: 31 MB removed from a"
        ev "     ~260 MB tracking service left oom_score unchanged at 676 on one build."
        record R8 PASS "R8.1-R8.4 only; R8.5 untested (adj=${adj:-?} score=${score:-?}, MemAvailable=${memavail:-?} kB)"
    fi
}

fgs_manifest_type() {
    local p t v
    p=$(sh_ pm path "$RUNTIME_PKG" | sed 's/^package://' | head -1)
    [ -n "$p" ] || return
    t=$(mktemp -d); $ADB pull "$p" "$t/base.apk" >/dev/null 2>&1 || { rm -rf "$t"; return; }
    v=$("$AAPT2" dump xmltree --file AndroidManifest.xml "$t/base.apk" 2>/dev/null \
        | grep -oE 'foregroundServiceType\(0x[0-9a-f]+\)=0x[0-9a-f]+' | head -1 | sed -E 's/.*=0x0*//')
    rm -rf "$t"
    [ -n "$v" ] || return
    printf '0x%s = %s\n' "$v" "$(fgs_type_names "$((16#$v))")"
}

# ---------------------------------------------------------------------------
# S4 -- ADPF
# ---------------------------------------------------------------------------
check_s4() {
    hdr "S4 -- ADPF / PowerHAL hint sessions (STRONGLY REC., PLATFORM)"
    local ipower sfhint
    ipower=$(sh_ service list | grep -E 'android\.hardware\.power\.IPower/' | head -1)
    sfhint=$(sh_ dumpsys SurfaceFlinger | grep -m1 -iE 'use_adpf_cpu_hint')
    kv "IPower AIDL service"  "${ipower:-<not registered>}"
    kv "SurfaceFlinger"       "${sfhint:-<no adpf key>}"
    # The definitive read: the runtime dlsym's APerformanceHint and says what it got.
    local line
    line=$($ADB logcat -d 2>/dev/null | tr -d '\r' | grep -oE 'ADPF: .*' | tail -1)
    # R6 clears the log buffer to watch for the freeform toggle; it stashes any
    # ADPF line it saw first, so running the whole probe never loses the answer.
    [ -n "$line" ] || line=$(tail -1 "$OUTDIR/adpf.txt" 2>/dev/null)
    kv "runtime ADPF log"     "${line:-<none in the current log buffer>}"
    ev ""
    case "$line" in
        *"perf-hint session created"*)
            ev "PASS -- APerformanceHint_createSession() succeeded on this device."
            record S4 PASS "hint session created ($line)" ;;
        *"createSession failed"*|*"no PerformanceHintManager"*|*"unavailable"*)
            ev "FAIL -- the ADPF surface exists and does nothing: a weave-bound render loop"
            ev "       has no way to tell the governor its deadline, so DVFS downclocks"
            ev "       between bursts.  Measured cost on the reference device: ~1/3 of the"
            ev "       frame rate, worked around by pipelining the weave one frame."
            record S4 FAIL "$line" ;;
        *)
            ev "INFO -- not decided from this log buffer.  The definitive test is one call"
            ev "     from a render thread: APerformanceHint_getManager() non-NULL AND"
            ev "     APerformanceHint_createSession() NON-NULL.  The runtime performs it and"
            ev "     logs 'ADPF: ...' -- run an out-of-process (service-compositor) session"
            ev "     and re-read logcat, or see the spec's own test (§3 S4)."
            local reg=no; [ -n "$ipower" ] && reg=yes
            record S4 INFO "IPower AIDL registered=$reg; no ADPF line in the log buffer" ;;
    esac
}

# ---------------------------------------------------------------------------
# S2 / S6 -- partly and wholly manual
# ---------------------------------------------------------------------------
check_s2() {
    hdr "S2 -- per-pixel click-through (STRONGLY REC., PLATFORM)"
    local op sink
    op=$(sh_ settings list global | grep -iE 'maximum_obscuring_opacity_for_touch|block_untrusted_touches' | tr '\n' ' ')
    sink=$(sh_ dumpsys SurfaceFlinger | grep -c 'ActivityRecordInputSink')
    kv "untrusted-touch policy" "${op:-<defaults; nothing set>}"
    kv "ActivityRecordInputSink layers" "$sink present"
    ev ""
    ev "INFO -- manual: see spec §3 S2.  What is left of this ask cannot be read out of"
    ev "     dumpsys: it is (a) whether a window may declare a per-REGION touchable area"
    ev "     (the API is reflection-blocklisted per-API, overlay windows included), and"
    ev "     (b) whether ActivityRecordInputSink can be scoped for a designated package."
    ev "     Both need the tap-inside / tap-outside experiment in the spec's own test."
    record S2 INFO "manual -- see spec §3 S2 (sink layers present: $sink)"
}

check_s6() {
    hdr "S6 -- window move atomic with the buffer; a drag affordance (STRONGLY REC.)"
    ev "INFO -- manual: see spec §3 S6.  Not measurable read-only: it needs a human to"
    ev "     drag a freeform window and watch whether the weave's phase origin tracks the"
    ev "     buffer within one frame, and freeform windows on the reference device have"
    ev "     neither a title bar nor a drag affordance, so the gesture does not exist to"
    ev "     be measured.  'am task resize' is NOT a substitute -- it desyncs WM from SF"
    ev "     (spec R6, #1087) and so tests the tooling rather than the platform."
    record S6 INFO "manual -- see spec §3 S6"
}

# ---------------------------------------------------------------------------
main() {
    printf 'DisplayXR OEM Android acceptance probe -- %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf 'spec: docs/specs/vendor/oem-android-platform-requirements.md\n'
    check_env
    want r6 && check_r6
    want s9 && check_s9
    want r8 && check_r8
    want s4 && check_s4
    want s2 && check_s2
    want s6 && check_s6

    hdr "Summary"
    printf '  %-5s %-6s %s\n' ASK VERDICT EVIDENCE
    printf '  %-5s %-6s %s\n' ----- ------ --------
    printf '%s' "$RESULTS" | while IFS='|' read -r id v s; do
        [ -n "$id" ] && printf '  %-5s %-6s %s\n' "$id" "$v" "$s"
    done
    printf '\n  artifacts: %s\n' "$OUTDIR"
    if [ "$ERRORS" -gt 0 ] && [ "$FAILS" -eq 0 ]; then
        printf '  %d check(s) could not be run.\n' "$ERRORS"; exit 2
    fi
    if [ "$FAILS" -gt 0 ]; then
        printf '  %d FAIL(s).\n' "$FAILS"; exit 1
    fi
    printf '  no FAILs.\n'
}

main
