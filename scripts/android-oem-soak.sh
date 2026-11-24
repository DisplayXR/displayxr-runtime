#!/usr/bin/env bash
# android-oem-soak.sh -- keep-alive soak for the DisplayXR Android runtime: the
# time- and load-dependent half of ask R8 in
# docs/specs/vendor/oem-android-platform-requirements.md, which the one-shot
# scripts/android-oem-probe.sh cannot see (R8.5 kill ranking, R8.6 wake + bind,
# freezer behaviour under use).
#
# Run it on a bench unit over adb. No root. Three modes:
#
#   soak       (default) A browser page draws inline 3D for --minutes (60), on
#              battery, with screen off/on cycles, app switches and induced
#              memory pressure. Sampled every 30 s.
#   idle       Screen off, no client, on battery for --idle-minutes (10); then a
#              3D app is launched cold and must come up with no manual step.
#   reinstall  The runtime APK is reinstalled in place (--runtime-apk), then a 3D
#              app must work on its FIRST launch, without opening the runtime.
#
#   ./scripts/android-oem-soak.sh --url https://example.org/inline3d-page \
#        --pressure com.example.heavy1,com.example.heavy2
#   ./scripts/android-oem-soak.sh --mode idle
#   ./scripts/android-oem-soak.sh --mode reinstall --runtime-apk DisplayXR-Runtime-….apk
#
# What it changes on the device, all restored on exit: it unplugs the battery
# gauge in software (`dumpsys battery unplug` -> `reset`; adb stays up), turns
# the screen off and on, launches and force-stops the apps it started, and in
# soak mode touches a capture trigger file in the runtime's own app directory.
# reinstall mode installs the APK you pass. It writes no setting and no system
# property, and never touches a whitelist: an exemption it added itself would
# hide exactly the failure it is looking for.
#
# Each KPI prints its raw evidence and one of:
#   PASS          measured, satisfied
#   FAIL          measured, not satisfied                       -> exit 1
#   INCONCLUSIVE  the run did not create the condition under test (for example
#                 memory pressure never built up) -- NOT a pass  -> exit 3
#   INFO          reported, not gated
#   ERROR         could not be measured at all                  -> exit 2
#
# Everything sampled is kept under --out (default: a new temp directory):
# samples.tsv, events.log, logcat-hits.txt, exit-info-*.txt, whitelist-*.txt.
#
# Requirements: adb, awk. The inline-3D page must be reachable from the device
# (for a page served from this machine: `adb reverse tcp:8000 tcp:8000` first).

set -u

RUNTIME_PKG=org.freedesktop.monado.openxr_runtime.out_of_process
BROWSER_PKG=org.chromium.chrome
BROWSER_ACT=com.google.android.apps.chrome.Main
APP=com.displayxr.model_viewer_vk_android
# Vendor display services, sampled when installed. Override with --services.
SERVICES="com.leia.headtrackingservice com.leialoft.display.config"

MODE=soak
URL=
MINUTES=60
IDLE_MINUTES=10
SAMPLE_S=30
SCREEN_CYCLES=10
APP_SWITCHES=20
PRESSURE=
MEM_FRAC=15            # pressure counts as reached below this % of MemTotal
RESUME_LIMIT_S=5       # weave must answer the liveness probe within this
RUNTIME_APK=
OUTDIR=
SERIAL=${ANDROID_SERIAL:-}
UNPLUG=1

usage() { sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --url) URL="$2"; shift 2 ;;
        --minutes) MINUTES="$2"; shift 2 ;;
        --idle-minutes) IDLE_MINUTES="$2"; shift 2 ;;
        --sample) SAMPLE_S="$2"; shift 2 ;;
        --screen-cycles) SCREEN_CYCLES="$2"; shift 2 ;;
        --app-switches) APP_SWITCHES="$2"; shift 2 ;;
        --pressure) PRESSURE="$2"; shift 2 ;;
        --mem-frac) MEM_FRAC="$2"; shift 2 ;;
        --resume-limit) RESUME_LIMIT_S="$2"; shift 2 ;;
        --browser) BROWSER_PKG="$2"; shift 2 ;;
        --browser-activity) BROWSER_ACT="$2"; shift 2 ;;
        --app) APP="$2"; shift 2 ;;
        --services) SERVICES=$(printf '%s' "$2" | tr ',' ' '); shift 2 ;;
        --runtime-apk) RUNTIME_APK="$2"; shift 2 ;;
        --no-unplug) UNPLUG=0; shift ;;
        --out) OUTDIR="$2"; shift 2 ;;
        --serial|-s) SERIAL="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1  (--help)" >&2; exit 2 ;;
    esac
done

command -v adb >/dev/null 2>&1 || { echo "adb not found." >&2; exit 2; }
ADB="adb"; [ -n "$SERIAL" ] && ADB="adb -s $SERIAL"
$ADB get-state >/dev/null 2>&1 || { echo "no device (serial='${SERIAL:-<any>}')." >&2; exit 2; }
case "$MODE" in soak|idle|reinstall) ;; *) echo "--mode must be soak, idle or reinstall" >&2; exit 2 ;; esac
[ "$MODE" = soak ] && [ -z "$URL" ] && { echo "soak mode needs --url <page that draws inline 3D>" >&2; exit 2; }
[ "$MODE" = reinstall ] && [ ! -f "$RUNTIME_APK" ] && { echo "reinstall mode needs --runtime-apk <file>" >&2; exit 2; }

[ -n "$OUTDIR" ] || OUTDIR=$(mktemp -d /tmp/dxr-oem-soak.XXXXXX)
mkdir -p "$OUTDIR"

# --- result table -----------------------------------------------------------
RESULTS=""; FAILS=0; ERRORS=0; INCONCL=0
record() {  # record <id> <verdict> <one-line summary>
    RESULTS="${RESULTS}$1|$2|$3
"
    case "$2" in
        FAIL) FAILS=$((FAILS + 1)) ;;
        ERROR) ERRORS=$((ERRORS + 1)) ;;
        INCONCLUSIVE) INCONCL=$((INCONCL + 1)) ;;
    esac
}
hdr() { printf '\n=== %s ===\n' "$1"; }
ev()  { printf '  %s\n' "$*"; }
kv()  { printf '  %-34s %s\n' "$1" "$2"; }
# </dev/null: adb shell reads stdin, and would swallow the rest of any
# `while read` loop it is called from.
sh_() { $ADB shell "$@" 2>/dev/null </dev/null | tr -d '\r'; }
note() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" >> "$OUTDIR/events.log"; }
dev_now() { sh_ date "+'%m-%d %H:%M:%S.000'" | tr -d "'"; }

LAUNCHED=
UNPLUGGED=0
TRIGGER=
cleanup() {
    local p
    for p in $LAUNCHED; do $ADB shell am force-stop "$p" >/dev/null 2>&1; done
    [ "$UNPLUGGED" = 1 ] && $ADB shell dumpsys battery reset >/dev/null 2>&1
    [ -n "$TRIGGER" ] && $ADB shell rm -f "$TRIGGER" >/dev/null 2>&1
    $ADB shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
    $ADB shell input keyevent KEYCODE_HOME >/dev/null 2>&1
}
trap cleanup EXIT
trap 'exit 130' INT TERM

screen_on()  { $ADB shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1; $ADB shell wm dismiss-keyguard >/dev/null 2>&1; }
screen_off() { $ADB shell input keyevent KEYCODE_SLEEP >/dev/null 2>&1; }
is_awake()   { sh_ dumpsys power | grep -q 'mWakefulness=Awake'; }
top_pkg()    { sh_ dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity' | grep -oE ' u[0-9]+ [^/ ]+/' | awk '{print $2}' | tr -d '/'; }
mem_kb()     { sh_ cat /proc/meminfo | awk -v k="$1:" '$1==k{print $2}'; }

# Every process of a package: "pid name" per line.
procs_of() { sh_ ps -A -o PID,NAME | awk -v p="$1" '$2==p || index($2, p ":")==1 {print $1, $2}'; }

# Freezer state of one pid: 1 frozen, 0 not, ? unreadable. The cgroup file is
# the direct reading; where an OEM build hides it from the shell user, fall
# back to ActivityManager's own flag. Which one answered is kept in a file,
# because this function is always called in a command substitution.
freeze_src() { cat "$OUTDIR/.freeze-src" 2>/dev/null; }
frozen_of() {
    local pid=$1 uid v
    uid=$(sh_ stat -c %u "/proc/$pid" 2>/dev/null)
    v=$(sh_ cat "/sys/fs/cgroup/uid_${uid}/pid_${pid}/cgroup.freeze" 2>/dev/null)
    case "$v" in 0|1) echo cgroup.freeze > "$OUTDIR/.freeze-src"; printf '%s' "$v"; return ;; esac
    v=$(sh_ dumpsys activity processes | awk -v pid="$pid" '$0 ~ ("ProcessRecord\\{[0-9a-f]+ " pid ":") {f=1} f && /isFrozen=/ {m=$0; sub(/.*isFrozen=/,"",m); sub(/[^a-z].*/,"",m); print m; exit}')
    case "$v" in
        true) echo "dumpsys activity (isFrozen)" > "$OUTDIR/.freeze-src"; printf 1 ;;
        false) echo "dumpsys activity (isFrozen)" > "$OUTDIR/.freeze-src"; printf 0 ;;
        *) printf '?' ;;
    esac
}

# Log lines that mean a bind or a start of the runtime was refused (R8.6).
BIND_RE='Bind failed immediately|RelatedStart BlockResult = true|skip unfrozen when bringup|could not be found to bind|XR_ERROR_RUNTIME_UNAVAILABLE'
LOG_SINCE=
collect_log_hits() {
    local now; now=$(dev_now)
    $ADB logcat -d -t "$LOG_SINCE" 2>/dev/null | tr -d '\r' | grep -E "$BIND_RE" | grep -E "monado|displayxr|DisplayXR|XR_ERROR" >> "$OUTDIR/logcat-hits.txt"
    LOG_SINCE=$now
}

exit_info() {  # exit_info <tag>: SIGNALED status=9 records per package
    local p
    for p in $RUNTIME_PKG $BROWSER_PKG $SERVICES; do
        sh_ dumpsys activity exit-info "$p" > "$OUTDIR/exit-info-$1-$p.txt"
    done
}
count_sigkills() { grep -hE 'reason=2 \(SIGNALED\)' "$OUTDIR"/exit-info-"$1"-*.txt 2>/dev/null | grep -c 'status=9'; }

env_header() {
    hdr "Environment"
    kv "device" "$(sh_ getprop ro.product.model), Android $(sh_ getprop ro.build.version.release) (SDK $(sh_ getprop ro.build.version.sdk))"
    kv "runtime" "$(sh_ dumpsys package $RUNTIME_PKG | grep -m1 -oE 'versionName=[^ ]+' || echo 'NOT INSTALLED')"
    kv "MemTotal / MemAvailable" "$(mem_kb MemTotal) kB / $(mem_kb MemAvailable) kB"
    kv "output directory" "$OUTDIR"
    sh_ dumpsys package $RUNTIME_PKG | grep -q versionName || { record ENV ERROR "runtime package not installed"; return 1; }
}

go_battery() {
    [ "$UNPLUG" = 1 ] || { ev "battery: left as is (--no-unplug) -- a freezer that only acts on battery is NOT exercised"; return; }
    $ADB shell dumpsys battery unplug >/dev/null 2>&1 && UNPLUGGED=1
    ev "battery: reported unplugged for the run (restored on exit)"
}

whitelist_snapshot() { sh_ dumpsys deviceidle whitelist | grep -E 'monado|displayxr' | sort > "$OUTDIR/whitelist-$1.txt"; }

# Launch a 3D app cold and decide whether it reached a session. Evidence is the
# runtime's own log lines, which do not depend on the app.
cold_launch_check() {  # cold_launch_check <kpi-id> <what>
    local id=$1 what=$2 t0 log="$OUTDIR/cold-launch-$1.txt"
    screen_on; sleep 1
    $ADB shell am force-stop "$APP" >/dev/null 2>&1
    t0=$(dev_now)
    local comp; comp=$(sh_ cmd package resolve-activity --brief "$APP" | tail -1)
    $ADB shell am start -n "$comp" >/dev/null 2>&1; LAUNCHED="$LAUNCHED $APP"
    sleep 15
    $ADB logcat -d -t "$t0" 2>/dev/null | tr -d '\r' > "$log"
    local refused err alive
    refused=$(grep -cE "$BIND_RE" "$log")
    err=$(grep -E 'XR_ERROR_[A-Z_]+ in xr(CreateInstance|CreateSession|GetSystem)' "$log" | head -3)
    alive=$(sh_ pidof "$APP")
    kv "app" "$APP (pid ${alive:-none})"
    kv "refused bind/start lines" "$refused"
    [ -n "$err" ] && printf '%s\n' "$err" | while IFS= read -r l; do ev "$l"; done
    if [ "$refused" -gt 0 ] || [ -n "$err" ] || [ -z "$alive" ]; then
        record "$id" FAIL "$what: app did not come up (refused=$refused, pid=${alive:-none}$( [ -n "$err" ] && echo ', OpenXR error'))"
    else
        record "$id" PASS "$what: app up on its first launch, no refused bind, no OpenXR error"
    fi
    $ADB shell am force-stop "$APP" >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# soak
# ---------------------------------------------------------------------------
weave_alive() {  # 0 if the runtime's weave engine consumes a capture trigger in time
    local i
    sh_ touch "$TRIGGER"
    i=0
    while [ $i -lt $((RESUME_LIMIT_S * 2)) ]; do
        sleep 0.5
        [ -z "$(sh_ ls "$TRIGGER" 2>/dev/null)" ] && return 0
        i=$((i + 1))
    done
    sh_ rm -f "$TRIGGER"
    return 1
}

sample() {  # one row in samples.tsv; updates the soak counters
    local t awake top avail row pid name fr frozen_now=0
    t=$(date -u +%H:%M:%S); awake=0; is_awake && awake=1
    top=$(top_pkg); avail=$(mem_kb MemAvailable)
    [ -n "$avail" ] && [ "$avail" -lt "$MIN_AVAIL" ] && MIN_AVAIL=$avail
    row="$t\t$awake\t$top\t$avail"
    local p watched=""
    for p in $RUNTIME_PKG $SERVICES; do watched="$watched
$(procs_of "$p")"; done
    # The browser process that binds the runtime and weaves is its GPU process.
    watched="$watched
$(procs_of "$BROWSER_PKG" | grep -E ':privileged_process|:gpu')"
    local seen=""
    while read -r pid name; do
        [ -n "$pid" ] || continue
        case "$name" in *:diag) continue ;; esac
        fr=$(frozen_of "$pid")
        row="$row\t$name=$pid:$fr"
        seen="$seen $name=$pid"
        # A frozen process only counts against the device while it should be working.
        if [ "$fr" = 1 ] && [ "$awake" = 1 ] && [ "$top" = "$BROWSER_PKG" ]; then
            frozen_now=1; note "FROZEN while weaving: $name pid $pid"
        fi
    done <<EOT
$watched
EOT
    [ "$frozen_now" = 1 ] && FROZEN_SAMPLES=$((FROZEN_SAMPLES + 1))
    printf '%b\n' "$row" >> "$OUTDIR/samples.tsv"
    # pid stability. Strict: the runtime, its :dxrN satellites and each display
    # service's main process must never be replaced. A service's own helper
    # sub-process (name with a ':') is reported, not gated: some restart by design.
    local e n v old strict
    for e in $seen; do
        n=${e%%=*}; v=${e##*=}
        case "$n" in
            "$RUNTIME_PKG"|"$RUNTIME_PKG":dxr*) strict=1 ;;
            "$BROWSER_PKG"*) continue ;;
            *:*) strict=0 ;;
            *) strict=1 ;;
        esac
        old=$(printf '%s\n' "$PIDMAP" | awk -v n="$n" '$1==n{print $2}')
        if [ -z "$old" ]; then PIDMAP="$PIDMAP
$n $v"
        elif [ "$old" != "$v" ]; then
            if [ "$strict" = 1 ]; then PID_CHANGES=$((PID_CHANGES + 1)); note "PID CHANGED: $n $old -> $v"
            else SUB_CHANGES=$((SUB_CHANGES + 1)); note "helper process restarted (not gated): $n $old -> $v"; fi
            PIDMAP=$(printf '%s\n' "$PIDMAP" | awk -v n="$n" -v v="$v" '$1==n{$2=v} {print}')
        fi
    done
    SAMPLES=$((SAMPLES + 1))
    collect_log_hits
}

to_browser() { $ADB shell am start -n "$BROWSER_PKG/$BROWSER_ACT" >/dev/null 2>&1; }

run_soak() {
    hdr "Soak -- browser inline 3D for $MINUTES min, $SCREEN_CYCLES screen cycles, $APP_SWITCHES app switches"
    go_battery
    whitelist_snapshot start
    exit_info start
    local kills0; kills0=$(count_sigkills start)
    : > "$OUTDIR/samples.tsv"; : > "$OUTDIR/logcat-hits.txt"
    SAMPLES=0; FROZEN_SAMPLES=0; PID_CHANGES=0; SUB_CHANGES=0; PIDMAP=""
    MIN_AVAIL=$(mem_kb MemAvailable)
    local total; total=$(mem_kb MemTotal)
    TRIGGER="/sdcard/Android/data/$RUNTIME_PKG/files/displayxr_atlas_trigger.$BROWSER_PKG"

    screen_on; sleep 1
    LOG_SINCE=$(dev_now)
    $ADB shell am start -n "$BROWSER_PKG/$BROWSER_ACT" -a android.intent.action.VIEW -d "$URL" >/dev/null 2>&1
    LAUNCHED="$LAUNCHED $BROWSER_PKG"
    sleep 20
    local probes=0 probe_fail=0
    if weave_alive; then
        ev "weave liveness probe: answered at start (the page is weaving)"
    else
        ev "weave liveness probe: NO answer at start. Either the page draws no inline 3D, the 3D"
        ev "element is not on screen, or this runtime has no capture trigger. The soak continues,"
        ev "but 'weave resumed' cannot be judged."
        probe_fail=-1
    fi

    # Memory pressure: start the operator's heavy apps, then come back.
    local p
    for p in $(printf '%s' "$PRESSURE" | tr ',' ' '); do
        $ADB shell am start -n "$(sh_ cmd package resolve-activity --brief "$p" | tail -1)" >/dev/null 2>&1
        LAUNCHED="$LAUNCHED $p"; note "pressure app started: $p"; sleep 6
    done
    [ -n "$PRESSURE" ] && { to_browser; sleep 5; }

    local end=$(( $(date +%s) + MINUTES * 60 )) dur=$((MINUTES * 60))
    local start; start=$(date +%s)
    local next_sample=$start cyc=0 sw=0 now el
    while :; do
        now=$(date +%s); [ "$now" -ge "$end" ] && break
        el=$((now - start))
        if [ "$now" -ge "$next_sample" ]; then sample; next_sample=$((now + SAMPLE_S)); fi
        # Screen off/on cycles, evenly spread.
        if [ "$SCREEN_CYCLES" -gt 0 ] && [ "$cyc" -lt "$SCREEN_CYCLES" ] && [ "$el" -ge $(( (cyc + 1) * dur / (SCREEN_CYCLES + 1) )) ]; then
            cyc=$((cyc + 1)); note "screen off (cycle $cyc)"
            screen_off; sleep 20; sample; screen_on; sleep 2; to_browser
            if [ "$probe_fail" -ge 0 ]; then
                probes=$((probes + 1)); weave_alive || { probe_fail=$((probe_fail + 1)); note "weave did NOT resume after screen-on (cycle $cyc)"; }
            fi
            continue
        fi
        # App switches, evenly spread.
        if [ "$APP_SWITCHES" -gt 0 ] && [ "$sw" -lt "$APP_SWITCHES" ] && [ "$el" -ge $(( (sw + 1) * dur / (APP_SWITCHES + 1) )) ]; then
            sw=$((sw + 1)); note "app switch $sw (home and back)"
            $ADB shell input keyevent KEYCODE_HOME >/dev/null 2>&1; sleep 4; to_browser; sleep 1
            if [ "$probe_fail" -ge 0 ]; then
                probes=$((probes + 1)); weave_alive || { probe_fail=$((probe_fail + 1)); note "weave did NOT resume after app switch $sw"; }
            fi
            continue
        fi
        sleep 2
    done
    sample
    whitelist_snapshot end
    exit_info end
    local kills1; kills1=$(count_sigkills end)

    # ---- verdicts ----
    local pct=$(( MIN_AVAIL * 100 / total ))
    kv "samples" "$SAMPLES (every ${SAMPLE_S} s; freezer read from: $(freeze_src || echo unreadable))"
    kv "lowest MemAvailable" "$MIN_AVAIL kB = ${pct}% of MemTotal (pressure threshold ${MEM_FRAC}%)"
    kv "screen cycles / app switches" "$cyc / $sw"
    kv "service helper restarts" "$SUB_CHANGES (reported, not gated; events.log)"
    local pressure=1; [ "$pct" -ge "$MEM_FRAC" ] && pressure=0

    if [ -z "$(freeze_src)" ]; then
        record K1-FREEZE ERROR "freezer state unreadable on this build (no cgroup.freeze, no isFrozen in dumpsys)"
    elif [ "$FROZEN_SAMPLES" -gt 0 ]; then
        record K1-FREEZE FAIL "$FROZEN_SAMPLES of $SAMPLES samples had a runtime, service or browser-GPU process frozen while the browser was in front"
    else
        record K1-FREEZE PASS "0 of $SAMPLES samples frozen while the browser was in front"
    fi

    if [ "$PID_CHANGES" -gt 0 ]; then
        record K2-PIDS FAIL "$PID_CHANGES process replacement(s) of the runtime or a display service (events.log)"
    elif [ "$pressure" = 0 ]; then
        record K2-PIDS INCONCLUSIVE "no process replaced, but MemAvailable never fell below ${MEM_FRAC}% -- the killer had no reason to run (pass --pressure)"
    else
        record K2-PIDS PASS "runtime and display-service pids unchanged under memory pressure"
    fi

    local hits; hits=$(wc -l < "$OUTDIR/logcat-hits.txt" | tr -d ' ')
    if [ "$hits" -gt 0 ]; then
        head -3 "$OUTDIR/logcat-hits.txt" | cut -c1-200 | while IFS= read -r l; do ev "$l"; done
        record K3-BINDS FAIL "$hits refused bind/start line(s) against the runtime (logcat-hits.txt)"
    else
        record K3-BINDS PASS "0 refused binds or starts"
    fi

    local newkills=$((kills1 - kills0))
    if [ "$newkills" -gt 0 ]; then
        record K4-KILLS FAIL "$newkills new 'reason=2 SIGNALED status=9' exit record(s) (exit-info-end-*.txt)"
    elif [ "$pressure" = 0 ]; then
        record K4-KILLS INCONCLUSIVE "no new SIGKILL exit records, but memory pressure was never reached"
    else
        record K4-KILLS PASS "no new SIGKILL exit records under memory pressure"
    fi

    if [ ! -s "$OUTDIR/whitelist-start.txt" ]; then
        record K5-WHITELIST INFO "no DisplayXR package on the device-idle whitelist at start; nothing to lose"
    elif cmp -s "$OUTDIR/whitelist-start.txt" "$OUTDIR/whitelist-end.txt"; then
        record K5-WHITELIST PASS "device-idle whitelist entries identical at start and end"
    else
        diff "$OUTDIR/whitelist-start.txt" "$OUTDIR/whitelist-end.txt" | while IFS= read -r l; do ev "$l"; done
        record K5-WHITELIST FAIL "device-idle whitelist entries changed during the run (pruned exemption)"
    fi

    if [ "$probe_fail" -lt 0 ]; then
        record K6-RESUME ERROR "weave liveness probe never answered, even at start (see above)"
    elif [ "$probe_fail" -gt 0 ]; then
        record K6-RESUME FAIL "weave did not resume within ${RESUME_LIMIT_S} s after $probe_fail of $probes screen-on / app-switch events"
    else
        record K6-RESUME PASS "weave resumed within ${RESUME_LIMIT_S} s after all $probes screen-on / app-switch events"
    fi
}

# ---------------------------------------------------------------------------
# idle
# ---------------------------------------------------------------------------
run_idle() {
    hdr "Idle -- screen off, no client, $IDLE_MINUTES min on battery, then a cold 3D launch"
    go_battery
    whitelist_snapshot start
    $ADB shell am force-stop "$APP" >/dev/null 2>&1
    $ADB shell input keyevent KEYCODE_HOME >/dev/null 2>&1
    screen_off
    ev "waiting $IDLE_MINUTES min ..."
    sleep $((IDLE_MINUTES * 60))
    local pid fr
    pid=$(sh_ pidof "$RUNTIME_PKG")
    if [ -z "$pid" ]; then
        kv "runtime after idle" "not running"
        record I1-IDLE-FREEZE INFO "runtime process not resident after the idle period"
    else
        fr=$(frozen_of "$pid")
        kv "runtime after idle" "pid $pid, frozen=$fr (read from: $(freeze_src || echo unreadable))"
        case "$fr" in
            0) record I1-IDLE-FREEZE PASS "runtime not frozen after $IDLE_MINUTES min idle" ;;
            1) record I1-IDLE-FREEZE INFO "runtime frozen after idle -- acceptable ONLY if I2 passes (a launch must thaw it)" ;;
            *) record I1-IDLE-FREEZE ERROR "freezer state unreadable on this build" ;;
        esac
    fi
    cold_launch_check I2-COLD-LAUNCH "after idle"
    whitelist_snapshot end
    if [ -s "$OUTDIR/whitelist-start.txt" ] && ! cmp -s "$OUTDIR/whitelist-start.txt" "$OUTDIR/whitelist-end.txt"; then
        record I3-WHITELIST FAIL "device-idle whitelist entries changed during the idle period"
    fi
}

# ---------------------------------------------------------------------------
# reinstall
# ---------------------------------------------------------------------------
run_reinstall() {
    hdr "Reinstall -- runtime APK reinstalled in place, then a 3D app on its first launch"
    local saw0 saw1 out
    saw0=$(sh_ cmd appops get "$RUNTIME_PKG" SYSTEM_ALERT_WINDOW | head -1)
    $ADB shell am force-stop "$APP" >/dev/null 2>&1
    out=$($ADB install -r "$RUNTIME_APK" 2>&1 | tail -1)
    kv "adb install -r" "$out"
    case "$out" in
        *Success*) ;;
        *) record R1-REINSTALL ERROR "install failed: $out"; return ;;
    esac
    ev "the runtime app is deliberately NOT opened before the 3D app is launched"
    cold_launch_check R1-FIRST-LAUNCH "after a runtime reinstall"
    saw1=$(sh_ cmd appops get "$RUNTIME_PKG" SYSTEM_ALERT_WINDOW | head -1)
    kv "SYSTEM_ALERT_WINDOW before" "$saw0"
    kv "SYSTEM_ALERT_WINDOW after" "$saw1"
    if [ "$saw0" = "$saw1" ]; then
        record R2-APPOP PASS "overlay app-op unchanged by the reinstall"
    else
        record R2-APPOP FAIL "overlay app-op changed by the reinstall ('$saw0' -> '$saw1')"
    fi
}

# ---------------------------------------------------------------------------
env_header && case "$MODE" in
    soak) run_soak ;;
    idle) run_idle ;;
    reinstall) run_reinstall ;;
esac

hdr "Result ($MODE)"
printf '%s' "$RESULTS" | while IFS='|' read -r id verdict summary; do
    [ -n "$id" ] && printf '  %-16s %-13s %s\n' "$id" "$verdict" "$summary"
done
echo
echo "  evidence: $OUTDIR"
[ "$ERRORS" -gt 0 ] && exit 2
[ "$FAILS" -gt 0 ] && exit 1
[ "$INCONCL" -gt 0 ] && exit 3
exit 0
