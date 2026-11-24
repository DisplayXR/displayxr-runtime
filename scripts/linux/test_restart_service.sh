#!/bin/bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Tests for scripts/linux/displayxr-restart-service (the helper a package
# upgrade runs so a RUNNING user displayxr.service picks up the new binaries).
#
#   scripts/linux/test_restart_service.sh [path/to/displayxr-restart-service]
#
# Part 1 (always): fake systemctl / loginctl / id on PATH, so the root path
# (every user manager, --machine=USER@) and the user path are checked without
# systemd and without touching anything. Asserts: only RUNNING services are
# acted on, try-restart (never start/restart), stop for the tarball,
# DXR_NO_SERVICE_RESTART, and exit 0 whatever happens.
#
# Part 2 (--real-user-manager, opt in): a throwaway probe unit in THIS user's
# real systemd --user manager (never displayxr.service): running -> restarted
# with a new PID; stopped -> stays stopped. Add --real-root to also run the
# root path through sudo (systemctl --user --machine=$USER@).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HELPER="$HERE/displayxr-restart-service"
REAL=0
REAL_ROOT=0
for a in "$@"; do
    case "$a" in
    --real-user-manager) REAL=1 ;;
    --real-root) REAL=1 REAL_ROOT=1 ;;
    *) HELPER="$(readlink -f "$a")" ;;
    esac
done
[ -x "$HELPER" ] || { echo "error: $HELPER not executable" >&2; exit 2; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }

# --- fakes ---------------------------------------------------------------
mkdir -p "$T/bin" "$T/run"
cat >"$T/bin/systemctl" <<'EOF'
#!/bin/sh
# Records every call; is-active answers from $T/state/<user> ("active" or not).
echo "$*" >>"$FAKE_LOG"
user="$FAKE_SELF"
for a in "$@"; do case "$a" in --machine=*) user="${a#--machine=}"; user="${user%@}" ;; esac; done
for a in "$@"; do
    case "$a" in
    is-active) [ "$(cat "$FAKE_STATE/$user" 2>/dev/null)" = active ]; exit $? ;;
    try-restart) exit "${FAKE_RESTART_RC:-0}" ;;
    esac
done
exit 0
EOF
cat >"$T/bin/loginctl" <<'EOF'
#!/bin/sh
printf '%s\n' "1001 alice no active" "1002 bob no active" "1003 carol no closing"
EOF
cat >"$T/bin/id" <<'EOF'
#!/bin/sh
case "$1" in
-u) case "${2:-}" in alice) echo 1001 ;; bob) echo 1002 ;; carol) echo 1003 ;; "") echo "$FAKE_UID" ;; *) exit 1 ;; esac ;;
-un) echo "$FAKE_SELF" ;;
esac
EOF
chmod +x "$T/bin/"*
# alice and bob have a user manager; carol's session is closing (no manager dir).
mkdir -p "$T/run/1001/systemd" "$T/run/1002/systemd"
export FAKE_LOG="$T/log" FAKE_STATE="$T/state" DXR_RUN_USER_DIR="$T/run"
mkdir -p "$FAKE_STATE"

run() { # run <uid> <self> [action] -> output in $T/out, calls in $T/log
    : >"$FAKE_LOG"
    FAKE_UID="$1" FAKE_SELF="$2" PATH="$T/bin:$PATH" "$HELPER" ${3:+"$3"} >"$T/out" 2>&1
    echo $? >"$T/rc"
}
has() { grep -qxF -- "$1" "$FAKE_LOG"; }
none() { ! grep -q -- "$1" "$FAKE_LOG"; }

echo "== part 1: fake systemd"

echo active >"$FAKE_STATE/alice"
echo inactive >"$FAKE_STATE/bob"
echo active >"$FAKE_STATE/carol"
run 0 root
[ "$(cat "$T/rc")" = 0 ] && ok "root: exit 0" || bad "root: exit $(cat "$T/rc")"
has "--user --machine=alice@ try-restart displayxr.service" && ok "root: running service (alice) try-restarted" ||
    bad "root: alice not try-restarted: $(cat "$FAKE_LOG")"
none "machine=bob@ try-restart" && ok "root: stopped service (bob) left alone" || bad "root: bob was acted on"
none "machine=carol@" && ok "root: user without a manager (carol) skipped" || bad "root: carol was reached"
grep -qE '(^| )(start|restart) ' "$FAKE_LOG" && bad "root: a plain start/restart was issued" || ok "root: never start / plain restart"
grep -q "restarted displayxr.service for user alice" "$T/out" && ok "root: says what it restarted" || bad "root: no message: $(cat "$T/out")"

run 0 root stop
has "--user --machine=alice@ stop displayxr.service" && ok "root stop: running service stopped" || bad "root stop: $(cat "$FAKE_LOG")"
none "machine=bob@ stop" && ok "root stop: stopped service left alone" || bad "root stop: bob acted on"

echo active >"$FAKE_STATE/dave"
run 1000 dave
has "--user try-restart displayxr.service" && ok "user: own running service try-restarted" || bad "user: $(cat "$FAKE_LOG")"
none "machine=" && ok "user: never reaches other managers" || bad "user: used --machine"

echo inactive >"$FAKE_STATE/dave"
run 1000 dave
none "try-restart" && ok "user: not running -> nothing (socket activation starts the new one)" || bad "user: acted on a stopped service"

echo active >"$FAKE_STATE/dave"
DXR_NO_SERVICE_RESTART=1 run 1000 dave
none "try-restart" && grep -q "NOT restarted" "$T/out" && ok "DXR_NO_SERVICE_RESTART=1: skipped, and says so" ||
    bad "DXR_NO_SERVICE_RESTART: $(cat "$FAKE_LOG") / $(cat "$T/out")"

FAKE_RESTART_RC=1 run 1000 dave
[ "$(cat "$T/rc")" = 0 ] && grep -q "could not try-restart" "$T/out" && ok "failed restart: exit 0 + recovery hint" ||
    bad "failed restart: rc=$(cat "$T/rc") $(cat "$T/out")"

run 1000 dave bogus
[ "$(cat "$T/rc")" = 0 ] && [ ! -s "$FAKE_LOG" ] && ok "unknown action: exit 0, no systemctl call" || bad "unknown action"

# No systemctl at all (a container): silent no-op.
mkdir -p "$T/nosysd"
ln -sf "$(command -v sh)" "$T/nosysd/sh"
for t in awk timeout id loginctl sed; do p="$(command -v "$t")" && ln -sf "$p" "$T/nosysd/$t"; done
PATH="$T/nosysd" "$HELPER" >"$T/out" 2>&1
rc=$?
[ "$rc" = 0 ] && [ ! -s "$T/out" ] && ok "no systemctl: silent exit 0" || bad "no systemctl: rc=$rc $(cat "$T/out")"

# --- part 2: a real user manager, a probe unit ---------------------------
if [ "$REAL" = 1 ]; then
    echo "== part 2: real systemd --user manager (probe unit, never displayxr.service)"
    PROBE=dxr-restart-probe-$$.service
    UDIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    mkdir -p "$UDIR"
    printf '[Service]\nExecStart=/bin/sleep infinity\n' >"$UDIR/$PROBE"
    cleanup_probe() {
        systemctl --user stop "$PROBE" >/dev/null 2>&1
        rm -f "$UDIR/$PROBE"
        systemctl --user daemon-reload
    }
    systemctl --user daemon-reload
    pid() { systemctl --user show -p MainPID --value "$PROBE"; }

    systemctl --user start "$PROBE"
    p1="$(pid)"
    DXR_SERVICE_UNIT="$PROBE" "$HELPER" >/dev/null
    p2="$(pid)"
    [ "$p1" != 0 ] && [ "$p2" != 0 ] && [ "$p1" != "$p2" ] && ok "user: running probe restarted (PID $p1 -> $p2)" ||
        bad "user: probe PID $p1 -> $p2"

    systemctl --user stop "$PROBE"
    DXR_SERVICE_UNIT="$PROBE" "$HELPER" >/dev/null
    systemctl --user is-active --quiet "$PROBE" && bad "user: a stopped probe was started" ||
        ok "user: stopped probe stays stopped"

    if [ "$REAL_ROOT" = 1 ]; then
        systemctl --user start "$PROBE"
        p1="$(pid)"
        timeout 60 sudo -n env DXR_SERVICE_UNIT="$PROBE" "$HELPER" >"$T/out" 2>&1
        p2="$(pid)"
        [ "$p1" != "$p2" ] && [ "$p2" != 0 ] && ok "root (--machine=$USER@): running probe restarted (PID $p1 -> $p2)" ||
            bad "root: probe PID $p1 -> $p2 ($(cat "$T/out"))"
        systemctl --user stop "$PROBE"
        timeout 60 sudo -n env DXR_SERVICE_UNIT="$PROBE" "$HELPER" >/dev/null 2>&1
        systemctl --user is-active --quiet "$PROBE" && bad "root: a stopped probe was started" ||
            ok "root: stopped probe stays stopped"
    fi
    cleanup_probe
fi

if [ "$fails" = 0 ]; then
    echo "PASS"
else
    echo "FAIL ($fails)"
    exit 1
fi
