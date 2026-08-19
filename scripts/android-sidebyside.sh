#!/usr/bin/env bash
#
# android-sidebyside.sh — bring two DisplayXR Android apps up SIDE BY SIDE in
# freeform on an NP02J-class device, deterministically, with NO resize after each
# app's OpenXR session is live (a post-session surface replacement was the #1040
# crash vector).
#
# This is the manual test vehicle for concurrent multi-app weaving (ADR-036 D3,
# #1031): each app is handed its own satellite compositor process (":dxrN") by
# the runtime's slot broker, so the two windows weave at the same time.
#
# THE RACE, AND HOW THIS SCRIPT KILLS IT
# --------------------------------------
# `am start --windowingMode 5` lands the first launch fullscreen; an immediate
# second call flips the task to freeform. `am task resize` then has to land
# BEFORE MonadoView's surfaceCreated()/passAppSurface() — a window of ~1 s that a
# plain `sleep`-and-hope sequence loses roughly half the time (both apps come up
# 2560x1600 stacked).
#
# `am start` on this device exposes NO launch-time bounds option (checked: the
# only window flags are --windowingMode / --activityType / --display; there is no
# --task-bounds / --activity-bounds), so the bounds cannot be handed over at
# launch. Instead we make the race unwinnable by FREEZING the app:
#
#   1. am start x2 (fullscreen -> freeform), then SIGSTOP the app process the
#      instant `pidof` resolves it (~230 ms on this device — comfortably before
#      the app's Java main thread creates its window).
#   2. With the app frozen, no window and no surface can exist. The task record
#      does exist, so `am task resize` is applied by system_server alone.
#   3. Verify the task's mBounds equals the target.
#   4. SIGCONT. The app creates its window ONCE, already at the target size, and
#      the OpenXR session/swapchain is built against the final surface.
#
# So the resize is not merely "early" — it is provably before any surface exists.
# Requires root adb (for SIGSTOP/SIGCONT); falls back to a tight poll-resize loop
# if `adb root` is unavailable.
#
# USAGE
#   scripts/android-sidebyside.sh [--left A|B] [--right A|B] [--kill]
#                                 [--gap N] [--top N] [--timeout SEC]
#                                 [--no-stage]
#
#   --no-stage  Skip the SIGSTOP staging entirely: launch + resize A, wait for
#               its session to go live, THEN launch B — so A really does take
#               the onActivityPaused + NativeWindowResized bounce and answers
#               with xrEndSession/xrBeginSession. This is the regression test
#               for #1041 (A must stay on "mode 1 (grid 2x1)" and stay woven)
#               and exercises the post-session surface replacement of #1040.
#
#   A = com.displayxr.cube_handle_vk_android/.MainActivity
#   B = com.displayxr.cube_handle_vk_android.b/...MainActivity
#       (build B with:  ./gradlew :test_apps:cube_handle_vk_android:assembleDebug \
#                           -PdxrAppIdSuffix=b)
#
#   Defaults: --left A --right B.  Slot assignment is the BROKER's — lowest free
#   slot, with a package that already owns one keeping it — so swapping
#   --left/--right swaps which app lands in :dxr0 too. `setprop debug.dxr.slot N`
#   pins every client to one slot; unset it to test the real path.
#
# Prints one status line per app: task id, window frame, satellite pid + slot,
# the broker's decision, whether that satellite is presenting (OOP_PRESENT_TS
# observed), its content mode and its swapchain surface format.
#
set -uo pipefail

ADB="${ADB:-adb}"

PKG_A="com.displayxr.cube_handle_vk_android"
ACT_A="$PKG_A/.MainActivity"
PKG_B="com.displayxr.cube_handle_vk_android.b"
ACT_B="$PKG_B/com.displayxr.cube_handle_vk_android.MainActivity"

LEFT="A"
RIGHT="B"
DO_KILL=0
GAP=0
TOP=60           # leave the status bar alone; freeform windows below it
TIMEOUT=25
NO_STAGE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --left)    LEFT="$2";    shift 2 ;;
        --right)   RIGHT="$2";   shift 2 ;;
        --kill)    DO_KILL=1;    shift ;;
        --gap)     GAP="$2";     shift 2 ;;
        --top)     TOP="$2";     shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --no-stage) NO_STAGE=1;  shift ;;
        -h|--help) sed -n '2,66p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

pkg_of()  { [[ "$1" == "A" ]] && echo "$PKG_A" || echo "$PKG_B"; }
act_of()  { [[ "$1" == "A" ]] && echo "$ACT_A" || echo "$ACT_B"; }

# ---------------------------------------------------------------- device probe

if ! $ADB get-state >/dev/null 2>&1; then
    echo "ERROR: no adb device" >&2; exit 3
fi

# Landscape display extent (the override display info carries the rotated size).
read -r DISP_W DISP_H < <(
    $ADB shell dumpsys window displays 2>/dev/null |
    sed -n 's/.*cur=\([0-9]*\)x\([0-9]*\).*/\1 \2/p' | head -1
)
if [[ -z "${DISP_W:-}" ]]; then DISP_W=2560; DISP_H=1600; fi
echo "display: ${DISP_W}x${DISP_H}"

HAVE_ROOT=0
if [[ "$($ADB shell id -u 2>/dev/null | tr -d '\r')" == "0" ]]; then
    HAVE_ROOT=1
else
    $ADB root >/dev/null 2>&1
    sleep 1
    [[ "$($ADB shell id -u 2>/dev/null | tr -d '\r')" == "0" ]] && HAVE_ROOT=1
fi
if [[ $HAVE_ROOT -eq 1 ]]; then
    echo "mode:    freeze (SIGSTOP) — resize provably precedes surface creation"
else
    echo "mode:    poll-resize (no root) — best effort, may race"
fi

HALF=$(( (DISP_W - GAP) / 2 ))
L_BOUNDS=(0 "$TOP" "$HALF" "$DISP_H")
R_BOUNDS=($((HALF + GAP)) "$TOP" "$DISP_W" "$DISP_H")

# ------------------------------------------------------------------- utilities

# Newest visible freeform/standard task id for a package (exact package match).
task_id_for() {
    local pkg="$1"
    $ADB shell dumpsys activity activities 2>/dev/null |
    tr -d '\r' |
    grep -oE "Task\{[0-9a-f]+ #[0-9]+ type=standard A=[0-9]+:${pkg} U=0 visible=true[^}]*" |
    grep -oE '#[0-9]+' | tr -d '#' | sort -n | tail -1
}

task_bounds_for() {
    local tid="$1"
    $ADB shell dumpsys activity activities 2>/dev/null | tr -d '\r' |
    awk -v tid="#${tid} " '
        index($0, "Task{") && index($0, tid) { want=1; next }
        want && /mBounds=Rect\(/ {
            match($0, /mBounds=Rect\([^)]*\)/)
            print substr($0, RSTART, RLENGTH); exit
        }'
}

# Frame of the package's top window, as "[l,t][r,b]".
window_frame_for() {
    local pkg="$1"
    $ADB shell dumpsys window windows 2>/dev/null | tr -d '\r' |
    awk -v pkg="$pkg/" '
        /^  Window #/ { cur = (index($0, "Window{") && index($0, pkg)) ? 1 : 0 }
        cur && /Frames:/ {
            match($0, /frame=\[[0-9-]+,[0-9-]+\]\[[0-9-]+,[0-9-]+\]/)
            if (RSTART) { f = substr($0, RSTART+6, RLENGTH-6) }
        }
        END { print f }'
}

app_pid_for() { $ADB shell pidof "$1" 2>/dev/null | tr -d '\r' | awk '{print $1}'; }

wait_for() {   # wait_for <timeout_sec> <cmd...>  — cmd exits 0 when satisfied
    local deadline=$(( $(date +%s) + $1 )); shift
    while (( $(date +%s) < deadline )); do
        "$@" && return 0
        sleep 0.25
    done
    return 1
}

# --------------------------------------------------------------- launch an app

# stage_app: launch, freeze, and size the task — but do NOT thaw. Both apps are
# staged before either is thawed, so the SECOND window's arrival never re-lays-out
# a window that already owns a live OpenXR session.
#
# Why that matters (the "one app is flat 2D" bug): when a second freeform window
# appears, Android bounces the first app through onActivityPaused/Resumed with a
# NativeWindowResized. The app answers with xrEndSession + xrBeginSession — and
# oxr_session_end() resets this session's active_rendering_mode_index to 0, so the
# following oxr_session_begin() pushes "CONTENT rendering mode 0 (grid 1x1)". The
# first app is then stuck in flat 2D forever (never weaves; the panel's 3D lens is
# still on, so it also looks dimmer than the 3D one). Staging both apps first
# means neither has a session to end when the other's window shows up.
stage_app() {
    local which="$1"; shift
    local l="$1" t="$2" r="$3" b="$4"
    local pkg act tid pid
    pkg="$(pkg_of "$which")"; act="$(act_of "$which")"

    echo "--- staging $which ($pkg) -> [$l,$t][$r,$b]"

    # Always cold-start the target: the freeze technique needs a process that did
    # not exist a moment ago, and a warm process would be SIGSTOPped mid-frame.
    $ADB shell am force-stop "$pkg" >/dev/null 2>&1
    sleep 1

    if [[ $HAVE_ROOT -eq 1 ]]; then
        # One round trip: launch twice (fullscreen -> freeform), then freeze the
        # app the moment its pid exists. The tight loop runs ON the device, so
        # host<->adb latency never enters the window.
        $ADB shell "
            am start -n $act --windowingMode 5 >/dev/null 2>&1
            am start -n $act --windowingMode 5 >/dev/null 2>&1
            i=0
            while [ \$i -lt 4000 ]; do
                p=\$(pidof $pkg)
                if [ -n \"\$p\" ]; then kill -STOP \$p; echo \"froze \$p\"; exit 0; fi
                i=\$((i+1))
            done
            echo 'freeze-failed'
        " 2>/dev/null | tr -d '\r' | sed 's/^/    /'
    else
        $ADB shell "am start -n $act --windowingMode 5 >/dev/null 2>&1; am start -n $act --windowingMode 5 >/dev/null 2>&1" >/dev/null 2>&1
    fi

    # Task id — exists as soon as the activity record is created.
    local deadline=$(( $(date +%s) + 8 ))
    while (( $(date +%s) < deadline )); do
        tid="$(task_id_for "$pkg")"
        [[ -n "$tid" ]] && break
        sleep 0.2
    done
    if [[ -z "${tid:-}" ]]; then
        echo "    ERROR: no visible task for $pkg" >&2
        return 1
    fi
    echo "    task #$tid"

    # Resize while the app is frozen: system_server applies it alone, so there is
    # no surface to replace and nothing for the satellite to trip over.
    $ADB shell am task resize "$tid" "$l" "$t" "$r" "$b" >/dev/null 2>&1

    local want="mBounds=Rect($l, $t - $r, $b)"
    deadline=$(( $(date +%s) + 6 ))
    while (( $(date +%s) < deadline )); do
        [[ "$(task_bounds_for "$tid")" == "$want" ]] && break
        $ADB shell am task resize "$tid" "$l" "$t" "$r" "$b" >/dev/null 2>&1
        sleep 0.3
    done
    local got_bounds; got_bounds="$(task_bounds_for "$tid")"
    if [[ "$got_bounds" != "$want" ]]; then
        echo "    WARN: task bounds '$got_bounds' != '$want'"
    else
        echo "    task bounds set pre-surface: $got_bounds"
    fi

    LAST_TID="$tid"
    LAST_PID="$(app_pid_for "$pkg")"
    return 0
}

# thaw_app: resume the frozen process. The window is created ONCE, already at the
# target size — the OpenXR session and the satellite's swapchain are built against
# the final surface, so there is no post-session surface replacement (#1040).
thaw_app() {
    local which="$1"; shift
    local l="$1" t="$2" r="$3" b="$4"
    local pkg pid frame target
    pkg="$(pkg_of "$which")"
    target="[$l,$t][$r,$b]"

    if [[ $HAVE_ROOT -eq 1 ]]; then
        pid="${R_APPPID[$which]}"
        [[ -n "$pid" ]] && $ADB shell "kill -CONT $pid" >/dev/null 2>&1
    fi

    local deadline=$(( $(date +%s) + TIMEOUT ))
    frame=""
    while (( $(date +%s) < deadline )); do
        frame="$(window_frame_for "$pkg")"
        [[ "$frame" == "$target" ]] && break
        sleep 0.3
    done
    [[ "$frame" != "$target" ]] && echo "    WARN: $which window frame '$frame' != '$target'"
    R_FRAME["$which"]="$frame"
}

# launch_app_live: the DELIBERATELY racy path (--no-stage). No freeze — the app
# is launched, resized and allowed to bring its OpenXR session all the way up
# BEFORE the other app is launched. The second window's arrival then really does
# bounce this one through onActivityPaused + NativeWindowResized -> xrEndSession
# -> xrBeginSession, and replaces its Surface after the session is live. That is
# the regression test for #1041 (content mode must survive the bounce) and for
# #1040 (the satellite must survive the surface replacement).
launch_app_live() {
    local which="$1"; shift
    local l="$1" t="$2" r="$3" b="$4"
    local pkg act tid frame target
    pkg="$(pkg_of "$which")"; act="$(act_of "$which")"
    target="[$l,$t][$r,$b]"

    echo "--- launching $which ($pkg) LIVE (no staging) -> $target"
    $ADB shell am force-stop "$pkg" >/dev/null 2>&1
    sleep 1
    $ADB shell "am start -n $act --windowingMode 5 >/dev/null 2>&1; am start -n $act --windowingMode 5 >/dev/null 2>&1" >/dev/null 2>&1

    local deadline=$(( $(date +%s) + 8 ))
    while (( $(date +%s) < deadline )); do
        tid="$(task_id_for "$pkg")"
        [[ -n "$tid" ]] && break
        sleep 0.2
    done
    if [[ -z "${tid:-}" ]]; then
        echo "    ERROR: no visible task for $pkg" >&2
        return 1
    fi
    echo "    task #$tid"

    deadline=$(( $(date +%s) + 8 ))
    while (( $(date +%s) < deadline )); do
        $ADB shell am task resize "$tid" "$l" "$t" "$r" "$b" >/dev/null 2>&1
        frame="$(window_frame_for "$pkg")"
        [[ "$frame" == "$target" ]] && break
        sleep 0.4
    done
    [[ "$frame" != "$target" ]] && echo "    WARN: $which window frame '$frame' != '$target'"

    LAST_TID="$tid"
    LAST_PID="$(app_pid_for "$pkg")"
    LAST_FRAME="$frame"

    # Wait until this app's satellite is actually presenting, so the NEXT
    # launch lands on a live session rather than one still starting up.
    deadline=$(( $(date +%s) + TIMEOUT ))
    while (( $(date +%s) < deadline )); do
        read -r sp _sl < <(satellite_for "$LAST_PID")
        if [[ -n "${sp:-}" && "$(presenting_for "$sp")" == "yes" ]]; then
            echo "    session live on satellite $sp"
            break
        fi
        sleep 1
    done
    return 0
}

# --------------------------------------------------------- satellite reporting

# Map app pid -> satellite pid + slot. The runtime's [HEALTH] line is emitted BY
# the satellite and names the client app's pid, so the logcat pid column gives us
# the satellite; the satellite's :dxrN process name gives us the slot. (The
# slot= field inside [HEALTH] is the CLIENT slot within that satellite, always 0
# always 0 because a satellite hosts exactly one client — not the satellite slot.)
satellite_for() {
    local apppid="$1" satpid slot
    satpid="$($ADB logcat -d -t 4000 2>/dev/null | tr -d '\r' |
              grep -F "[HEALTH]" | grep -F "pid=$apppid " |
              awk '{print $3}' | tail -1)"
    [[ -z "$satpid" ]] && return 1
    slot="$($ADB shell ps -A -o PID,NAME 2>/dev/null | tr -d '\r' |
            awk -v p="$satpid" '$1==p {print $2}' |
            grep -oE ':dxr[0-9]+')"
    echo "$satpid" "${slot:-?}"
}

# What the slot broker decided for this package, verbatim from its own log.
broker_for() {
    local pkg="$1"
    $ADB logcat -d 2>/dev/null | tr -d '\r' |
    grep -F "acquireSlot: $pkg " | tail -1 |
    sed 's/.*acquireSlot: //'
}

presenting_for() {   # satellite pid -> yes/no
    local satpid="$1"
    if $ADB logcat -d -t 4000 2>/dev/null | tr -d '\r' |
       awk -v p="$satpid" '$3==p && /OOP_PRESENT_TS/ {found=1} END{exit !found}'; then
        echo yes
    else
        echo no
    fi
}

# Last CONTENT rendering mode this satellite was asked for. 1 (grid 2x1) = the
# LeiaSR 3D mode; 0 (grid 1x1) = flat 2D. A satellite that ends up on 0 will
# never weave — see the note on stage_app.
mode_for() {
    local satpid="$1"
    $ADB logcat -d 2>/dev/null | tr -d '\r' |
    awk -v p="$satpid" '$3==p && /CONTENT rendering mode/ {
        match($0, /mode [0-9]+ .*grid [0-9]+x[0-9]+/)
        if (RSTART) m = substr($0, RSTART, RLENGTH)
    } END { print (m == "" ? "?" : m) }'
}

# Per-satellite present-surface description: swapchain format + colorspace +
# compositeAlpha + the transparent/overlay flags. Emitted once per swapchain
# (re)build by comp_target_swapchain as "SURFACE_FMT: ...". Two satellites that
# differ here will differ in brightness/gamma on screen.
surface_fmt_for() {
    local satpid="$1"
    $ADB logcat -d 2>/dev/null | tr -d '\r' |
    awk -v p="$satpid" '$3==p && /SURFACE_FMT:/ {
        match($0, /SURFACE_FMT:.*/)
        if (RSTART) m = substr($0, RSTART + 13, RLENGTH - 13)
    } END { print (m == "" ? "?" : m) }'
}

# Did this satellite's DP actually weave (CNSDK interlace ran)?
woven_for() {
    local satpid="$1"
    if $ADB logcat -d 2>/dev/null | tr -d '\r' |
       awk -v p="$satpid" '$3==p && /backlight -> 3D ON \(weave\)/ {f=1} END{exit !f}'; then
        echo yes
    else
        echo no
    fi
}

# --------------------------------------------------------------------- main

if [[ $DO_KILL -eq 1 ]]; then
    echo "killing apps + satellites"
    $ADB shell am force-stop "$PKG_A" >/dev/null 2>&1
    $ADB shell am force-stop "$PKG_B" >/dev/null 2>&1
    $ADB shell "for p in \$(ps -A -o PID,NAME | grep 'out_of_process:dxr' | awk '{print \$1}'); do kill -9 \$p; done" >/dev/null 2>&1
    sleep 2
fi

$ADB logcat -c >/dev/null 2>&1

declare -A R_TID R_FRAME R_APPPID

if [[ $NO_STAGE -eq 1 ]]; then
    # Sequential, unstaged: each app comes fully up before the next is launched,
    # so the first one takes the pause/resize/end-begin bounce for real.
    echo "--- no-stage mode: sequential launch, first app WILL be bounced"
    for spec in "left:$LEFT" "right:$RIGHT"; do
        side="${spec%%:*}"; which="${spec##*:}"
        if [[ "$side" == "left" ]]; then bnds=("${L_BOUNDS[@]}"); else bnds=("${R_BOUNDS[@]}"); fi
        launch_app_live "$which" "${bnds[@]}" || exit 1
        R_TID["$which"]="$LAST_TID"; R_APPPID["$which"]="$LAST_PID"; R_FRAME["$which"]="$LAST_FRAME"
    done
    # The first app's window was re-laid-out by the second's arrival; re-assert
    # both task bounds and re-read the frames.
    for spec in "left:$LEFT" "right:$RIGHT"; do
        side="${spec%%:*}"; which="${spec##*:}"
        if [[ "$side" == "left" ]]; then bnds=("${L_BOUNDS[@]}"); else bnds=("${R_BOUNDS[@]}"); fi
        $ADB shell am task resize "${R_TID[$which]}" "${bnds[0]}" "${bnds[1]}" "${bnds[2]}" "${bnds[3]}" >/dev/null 2>&1
        sleep 1
        R_FRAME["$which"]="$(window_frame_for "$(pkg_of "$which")")"
        R_APPPID["$which"]="$(app_pid_for "$(pkg_of "$which")")"
    done
else
    # Phase 1: stage BOTH apps frozen, with both task bounds already final.
    for spec in "left:$LEFT" "right:$RIGHT"; do
        side="${spec%%:*}"; which="${spec##*:}"
        if [[ "$side" == "left" ]]; then bnds=("${L_BOUNDS[@]}"); else bnds=("${R_BOUNDS[@]}"); fi
        stage_app "$which" "${bnds[@]}" || exit 1
        R_TID["$which"]="$LAST_TID"; R_APPPID["$which"]="$LAST_PID"
    done

    # Phase 2: thaw both. Neither app owns a session while the other's window lands,
    # so neither is bounced into CONTENT rendering mode 0.
    echo "--- thawing both"
    for spec in "left:$LEFT" "right:$RIGHT"; do
        side="${spec%%:*}"; which="${spec##*:}"
        if [[ "$side" == "left" ]]; then bnds=("${L_BOUNDS[@]}"); else bnds=("${R_BOUNDS[@]}"); fi
        thaw_app "$which" "${bnds[@]}"
    done
fi

echo "waiting for both satellites to present..."
sleep 12

echo
echo "=== status ==="
for spec in "LEFT:$LEFT" "RIGHT:$RIGHT"; do
    side="${spec%%:*}"; which="${spec##*:}"
    pkg="$(pkg_of "$which")"
    apppid="${R_APPPID[$which]}"
    read -r satpid slot < <(satellite_for "$apppid")
    pres="no"; mode="?"; woven="no"; fmt="?"
    if [[ -n "${satpid:-}" ]]; then
        pres="$(presenting_for "$satpid")"
        mode="$(mode_for "$satpid")"
        woven="$(woven_for "$satpid")"
        fmt="$(surface_fmt_for "$satpid")"
    fi
    printf '%-5s %s  task=#%s  frame=%s  app_pid=%s  satellite=%s(%s)  presenting=%s  %s  woven=%s\n' \
        "$side" "$which" "${R_TID[$which]}" "${R_FRAME[$which]}" "$apppid" \
        "${satpid:-?}" "${slot:-?}" "$pres" "$mode" "$woven"
    printf '      broker:  %s\n' "$(broker_for "$pkg")"
    printf '      surface: %s\n' "$fmt"
done
