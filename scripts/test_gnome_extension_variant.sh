#!/bin/bash
# Unit test for part 1 of scripts/linux/displayxr-gnome-extension-enable:
# picking the extension entry point that the running GNOME Shell can parse
# (#1663). GNOME 45+ loads an ES module, GNOME 40-44 the legacy importer, and
# one UUID holds one extension.js — so the installer has to choose.
#
# Hermetic and hardware-free: every run builds a throwaway HOME plus a fake
# system data root, drives the script with DISPLAYXR_GNOME_SHELL_VERSION
# instead of a real shell, and pre-writes the per-user stamp so part 2 (the
# gsettings enable) exits before touching the real session's dconf.
#
#   ./scripts/test_gnome_extension_variant.sh
#
# There is no GNOME 42 machine in CI; this asserts the SELECTION, not that the
# legacy entry point runs. Loading it is validated by hand on a 22.04 box —
# see contrib/gnome-shell/window-geometry@displayxr.org/README.md.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT/scripts/linux/displayxr-gnome-extension-enable"
SRC="$ROOT/contrib/gnome-shell/window-geometry@displayxr.org"
UUID="window-geometry@displayxr.org"

FAILED=0
CASE="(none)"

fail() {
	echo "  FAIL [$CASE] $*"
	FAILED=1
}
ok() { echo "  ok   [$CASE] $*"; }

assert_file() { [ -f "$1" ] && ok "exists: ${1#"$SANDBOX"/}" || fail "missing: ${1#"$SANDBOX"/}"; }
assert_no_file() { [ ! -e "$1" ] && ok "absent: ${1#"$SANDBOX"/}" || fail "should not exist: ${1#"$SANDBOX"/}"; }
assert_no_dir() { [ ! -d "$1" ] && ok "absent: ${1#"$SANDBOX"/}" || fail "should not exist: ${1#"$SANDBOX"/}"; }
assert_same() {
	if cmp -s "$1" "$2"; then ok "$3"; else fail "$3 (${1#"$SANDBOX"/} != ${2#"$SANDBOX"/})"; fi
}

# A fresh sandbox: $SANDBOX/home (HOME), $SANDBOX/sys (a system data root).
new_sandbox() {
	CASE="$1"
	echo "== $CASE"
	SANDBOX="$(mktemp -d)"
	TRASH="$TRASH $SANDBOX"
	mkdir -p "$SANDBOX/home" "$SANDBOX/sys"
	# Pre-stamp so the gsettings half is skipped: this test is about part 1,
	# and it must never write to the developer's own session settings.
	mkdir -p "$SANDBOX/home/.local/state/displayxr"
	echo "$UUID" >"$SANDBOX/home/.local/state/displayxr/gnome-extension-autoenabled"
	USERDIR="$SANDBOX/home/.local/share/gnome-shell/extensions/$UUID"
	SYSDIR="$SANDBOX/sys/gnome-shell/extensions/$UUID"
}
TRASH=""
cleanup() { for d in $TRASH; do rm -rf "$d"; done; }
trap cleanup EXIT

install_into() { # install_into <dir> [--no-legacy]
	mkdir -p "$1"
	cp "$SRC/metadata.json" "$SRC/extension.js" "$SRC/lib.js" "$1/"
	[ "${2:-}" = --no-legacy ] || cp "$SRC/extension-gnome42.js" "$1/"
}

run_enable() { # run_enable <shell-version>
	HOME="$SANDBOX/home" \
		XDG_DATA_HOME="$SANDBOX/home/.local/share" \
		XDG_STATE_HOME="$SANDBOX/home/.local/state" \
		XDG_DATA_DIRS="$SANDBOX/sys" \
		XDG_CURRENT_DESKTOP="GNOME" \
		DISPLAYXR_GNOME_SHELL_VERSION="$1" \
		"$SCRIPT" 2>&1
}

# --- 1. .deb-shaped install (system dir, not the user's) on GNOME 42 --------
new_sandbox "system install + GNOME 42 -> per-user legacy shadow"
install_into "$SYSDIR"
run_enable 42.9 >/dev/null
assert_file "$USERDIR/.displayxr-shadow"
assert_same "$USERDIR/extension.js" "$SYSDIR/extension-gnome42.js" "slot holds the legacy entry point"
assert_same "$USERDIR/lib.js" "$SYSDIR/lib.js" "shared lib.js copied"
assert_same "$USERDIR/metadata.json" "$SYSDIR/metadata.json" "metadata.json copied"

# --- 2. the same login again changes nothing -------------------------------
CASE="system install + GNOME 42, second login -> idempotent"
echo "== $CASE"
BEFORE="$(find "$USERDIR" -type f -newermt "@0" -printf '%p %s\n' | sort; md5sum "$USERDIR"/* | sort)"
OUT="$(run_enable 42.9)"
AFTER="$(find "$USERDIR" -type f -newermt "@0" -printf '%p %s\n' | sort; md5sum "$USERDIR"/* | sort)"
[ "$BEFORE" = "$AFTER" ] && ok "no files rewritten" || fail "files changed on a no-op run"
[ -z "$OUT" ] && ok "silent" || fail "printed on a no-op run: $OUT"

# --- 3. ...and a distribution upgrade to GNOME 50 removes it ---------------
CASE="system install + upgrade to GNOME 50 -> shadow removed"
echo "== $CASE"
run_enable 50.1 >/dev/null
assert_no_dir "$USERDIR"
assert_file "$SYSDIR/extension.js"

# --- 4. tarball-shaped user install on GNOME 42: switched in place ---------
new_sandbox "user install + GNOME 42 -> slot switched in place"
install_into "$USERDIR"
cp "$USERDIR/extension.js" "$SANDBOX/modern-original.js"
run_enable 42.9 >/dev/null
assert_same "$USERDIR/extension.js" "$SRC/extension-gnome42.js" "slot holds the legacy entry point"
assert_same "$USERDIR/extension-gnome45.js" "$SANDBOX/modern-original.js" "modern entry kept aside"
assert_no_file "$USERDIR/.displayxr-shadow"

# --- 5. ...and back on GNOME 50 -------------------------------------------
CASE="user install + upgrade to GNOME 50 -> modern entry restored"
echo "== $CASE"
run_enable 50.1 >/dev/null
assert_same "$USERDIR/extension.js" "$SANDBOX/modern-original.js" "slot holds the modern entry point"
assert_file "$USERDIR/extension-gnome42.js"

# --- 6. a modern shell with a modern install: nothing happens --------------
new_sandbox "user install + GNOME 50 -> untouched"
install_into "$USERDIR"
OUT="$(run_enable 50.1)"
assert_same "$USERDIR/extension.js" "$SRC/extension.js" "slot untouched"
assert_no_file "$USERDIR/extension-gnome45.js"
[ -z "$OUT" ] && ok "silent" || fail "printed: $OUT"

# --- 7. GNOME 42 against a package that predates the legacy entry point ----
new_sandbox "old package (no legacy entry) + GNOME 42 -> refuses, says why"
install_into "$SYSDIR" --no-legacy
OUT="$(run_enable 42.9)"
assert_no_dir "$USERDIR"
case "$OUT" in *extension-gnome42.js*) ok "explains what is missing" ;; *) fail "silent about the missing entry point: $OUT" ;; esac

# --- 8. version cannot be determined: leave everything alone ---------------
new_sandbox "unknown shell version -> nothing touched"
install_into "$USERDIR"
PATH="/nonexistent" run_enable "" >/dev/null
assert_same "$USERDIR/extension.js" "$SRC/extension.js" "slot untouched"

# --- 9. a package update reaches an existing shadow ------------------------
new_sandbox "package update + GNOME 42 -> shadow refreshed"
install_into "$SYSDIR"
run_enable 42.9 >/dev/null
printf '\n// updated by the package\n' >>"$SYSDIR/lib.js"
run_enable 42.9 >/dev/null
assert_same "$USERDIR/lib.js" "$SYSDIR/lib.js" "shadow picked up the new lib.js"

# --- 10. nothing installed anywhere ---------------------------------------
new_sandbox "no install at all -> no-op"
run_enable 42.9 >/dev/null
assert_no_dir "$USERDIR"

echo
if [ "$FAILED" = 0 ]; then
	echo "PASS: entry-point selection"
else
	echo "FAIL: entry-point selection"
fi
exit "$FAILED"
