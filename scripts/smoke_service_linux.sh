#!/usr/bin/env bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Headless smoke test for displayxr-service on Linux (#1744): no GPU, no
# window, no display, no systemd, no OpenXR loader — it runs in a pristine
# ubuntu container (CI's DebInstall matrix, via verify_deb_install_linux.sh) as
# well as on a dev box against an extracted .deb or a build tree:
#
#   ./scripts/smoke_service_linux.sh                       # the installed .deb
#   ./scripts/smoke_service_linux.sh --bin-dir <dir>       # <dir>/displayxr-{service,cli}
#
# The service's display processor comes from the normal discovery path
# (/usr/lib/displayxr/plugins on an installed box); set XRT_PLUGIN_SEARCH_PATH
# to point it elsewhere. Every run uses a private XDG_RUNTIME_DIR, so it never
# touches a service the user already has running.
#
# Checks, each against a fresh service process:
#   1. DETACHED start — stdin </dev/null, the way systemd / an autostart entry /
#      nohup start it. That used to die at once with
#      "epoll_ctl(stdin) failed" (Linux service only ran as `sleep | service`).
#      Then an IPC handshake: `displayxr-cli clients` connects, maps the shared
#      memory, passes the client<->service git-tag check and lists itself.
#   2. SIGTERM stops it cleanly (exit 0) and it unlinks its socket.
#   3. A socket left behind by a SIGKILLed service does not block the next
#      start (stale-socket recovery).
#   4. SOCKET ACTIVATION — the packaged systemd user unit's path — emulated
#      with the sd_listen_fds(3) protocol (a listening socket on fd 3 +
#      LISTEN_FDS/LISTEN_PID): the service must adopt it, serve the handshake,
#      and exit by itself once idle (IPC_EXIT_WHEN_IDLE, as the unit sets).
set -euo pipefail

BIN_DIR=/usr/lib/displayxr/bin
while [ "$#" -gt 0 ]; do
    case "$1" in
    --bin-dir) BIN_DIR="$2"; shift 2 ;;
    *) echo "usage: $0 [--bin-dir DIR]" >&2; exit 2 ;;
    esac
done
SERVICE="$BIN_DIR/displayxr-service"
CLI="$BIN_DIR/displayxr-cli"
[ -x "$SERVICE" ] || { echo "error: $SERVICE missing or not executable" >&2; exit 1; }
[ -x "$CLI" ] || { echo "error: $CLI missing or not executable" >&2; exit 1; }

WORK="$(mktemp -d)"
chmod 0700 "$WORK"
export XDG_RUNTIME_DIR="$WORK/run"
mkdir -m 0700 "$XDG_RUNTIME_DIR"
SOCK="$XDG_RUNTIME_DIR/displayxr_comp_ipc"
SVC_PID=""

cleanup() {
    # Only ever the pid this script started.
    if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
        kill -KILL "$SVC_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "error: $*" >&2
    [ -f "$WORK/service.log" ] && { echo "--- service log (tail)" >&2; tail -40 "$WORK/service.log" >&2; }
    exit 1
}

# Ready = the socket exists AND the service printed its start banner, which
# comes after the listening socket is set up (ipc_server_mainloop_init). The
# socket alone is not enough: in check 3 it is the dead service's stale one.
wait_until_ready() {
    for _ in $(seq 1 300); do
        if [ -S "$SOCK" ] && grep -q 'service has started' "$WORK/service.log" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SVC_PID" 2>/dev/null || fail "displayxr-service exited during startup"
        sleep 0.1
    done
    fail "service not ready (socket $SOCK + start banner) after 30 s"
}

# Wait up to $1 seconds for the service to exit; sets SVC_RC.
wait_for_exit() {
    local limit=$(($1 * 10))
    for _ in $(seq 1 "$limit"); do
        if ! kill -0 "$SVC_PID" 2>/dev/null; then
            SVC_RC=0
            wait "$SVC_PID" || SVC_RC=$?
            SVC_PID=""
            return 0
        fi
        sleep 0.1
    done
    return 1
}

handshake() {
    local out
    out="$("$CLI" clients 2>&1)" || { echo "$out" >&2; fail "displayxr-cli clients failed (IPC handshake)"; }
    grep -q 'service: connected' <<<"$out" || { echo "$out" >&2; fail "IPC handshake did not report a connected service"; }
    grep -q 'displayxr-cli' <<<"$out" || { echo "$out" >&2; fail "the handshake client is missing from the service's client list"; }
    echo "    IPC handshake OK ($(head -1 <<<"$out"))"
}

start_detached() {
    "$SERVICE" </dev/null >"$WORK/service.log" 2>&1 &
    SVC_PID=$!
}

echo "=== 1. detached start (stdin </dev/null) + IPC handshake"
start_detached
wait_until_ready
handshake
kill -0 "$SVC_PID" 2>/dev/null || fail "service died after the handshake"

echo "=== 2. SIGTERM: clean exit, socket unlinked"
kill -TERM "$SVC_PID"
wait_for_exit 15 || fail "service still running 15 s after SIGTERM"
[ "$SVC_RC" = 0 ] || fail "service exited $SVC_RC on SIGTERM (want 0)"
[ ! -e "$SOCK" ] || fail "socket $SOCK left behind after SIGTERM"
echo "    exit 0, socket removed"

echo "=== 3. stale socket from a SIGKILLed service"
start_detached
wait_until_ready
kill -KILL "$SVC_PID"
wait_for_exit 10 || fail "SIGKILLed service did not exit"
[ -S "$SOCK" ] || fail "expected the killed service's socket to be left behind"
start_detached
wait_until_ready
handshake
grep -q 'Removing stale socket file' "$WORK/service.log" || fail "restart did not report removing the stale socket"
kill -TERM "$SVC_PID"
wait_for_exit 15 || fail "service still running 15 s after SIGTERM"

echo "=== 4. socket activation (sd_listen_fds protocol) + exit when idle"
# perl-base only (Socket, POSIX): present in every Ubuntu image, no python.
cat >"$WORK/activate.pl" <<'PERL'
use strict; use warnings; use Socket; use POSIX ();
my $path = shift @ARGV;
$^F = 10;    # keep the socket inheritable across exec (no FD_CLOEXEC)
socket(my $s, PF_UNIX, SOCK_STREAM, 0) or die "socket: $!";
bind($s, pack_sockaddr_un($path)) or die "bind $path: $!";
listen($s, 16) or die "listen: $!";
if (fileno($s) != 3) { POSIX::dup2(fileno($s), 3) or die "dup2: $!"; }
$ENV{LISTEN_FDS} = 1;
$ENV{LISTEN_PID} = $$;    # exec keeps the pid
exec { $ARGV[0] } @ARGV or die "exec: $!";
PERL
IPC_EXIT_WHEN_IDLE=1 IPC_EXIT_WHEN_IDLE_DELAY_MS=1000 \
    perl "$WORK/activate.pl" "$SOCK" "$SERVICE" </dev/null >"$WORK/service.log" 2>&1 &
SVC_PID=$!
wait_until_ready
handshake
grep -q 'socket-activated fd 3' "$WORK/service.log" || fail "service did not adopt the socket-activation fd"
wait_for_exit 20 || fail "socket-activated service did not exit when idle (IPC_EXIT_WHEN_IDLE)"
[ "$SVC_RC" = 0 ] || fail "idle exit returned $SVC_RC (want 0)"
echo "    adopted fd 3, served the handshake, exited 0 when idle"

echo "==> displayxr-service smoke PASS"
