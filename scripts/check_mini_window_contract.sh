#!/usr/bin/env bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Assert that a built runtime APK still exposes the MiniWindowLayout cross-APK
# contract (#1403 Option B), unobfuscated, with the exact descriptors a
# reflecting caller in ANOTHER APK looks up by name.
#
#   scripts/check_mini_window_contract.sh [--require-dexdump] <runtime.apk>
#
# --require-dexdump turns the degraded name-only arm into a hard failure. CI sets
# it: without it a runner that lost its build-tools would go GREEN on a grep for
# method names, which is not the check anyone thinks is running (#1401 review).
#
# WHY THIS EXISTS. The DisplayXR Browser's XrSession runs in Chromium's GPU
# process, which is an isolated Service with no Activity — so it cannot be told
# the mini-window answer over OpenXR, and its BROWSER process measures for
# itself by reflecting into the runtime APK's
# org.freedesktop.monado.auxiliary.MiniWindowLayout. That turns a Java class into
# a cross-APK ABI, which rots differently from a C one: a rename, a signature
# change, or a minification config change here compiles clean, ships, and breaks
# a shipped browser at run time with no signal on either side. This is the
# signal.
#
# Run against the RELEASE APK — that is the artifact that ships.
#
# SCOPE: build.gradle currently sets `minifyEnabled false` on release, so today
# this detects a RENAME, a signature change or an access-flag change. It is not
# yet evidence that the contract survives R8, because R8 does not run. It
# becomes that automatically if minification is ever enabled, which is the point
# of checking the release artifact rather than the debug one.
set -uo pipefail

# dexdump emits raw string-pool bytes; a UTF-8 locale makes awk abort on them.
export LC_ALL=C

REQUIRE_DEXDUMP=0
APK=""
for arg in "$@"; do
    case "$arg" in
    --require-dexdump) REQUIRE_DEXDUMP=1 ;;
    *) APK="$arg" ;;
    esac
done
if [ -z "$APK" ] || [ ! -f "$APK" ]; then
    echo "usage: $0 [--require-dexdump] <runtime.apk>" >&2
    exit 2
fi

CLASS_DESC='Lorg/freedesktop/monado/auxiliary/MiniWindowLayout;'

# name<TAB>descriptor. APPEND-ONLY — see the class javadoc. A signature that
# changes rather than being added is the failure this file exists to catch, so
# fixing a break by editing the line below is almost always the wrong move.
read -r -d '' WANT <<'EOF'
contractVersion	()I
computeHintForActivity	(Ljava/lang/Object;IIIIII)[I
isInScalableContainer	(Ljava/lang/Object;)Z
resetForActivity	()V
isTell	(IIIIII)Z
isBindingTell	(IIIIII)Z
isEnabled	()Z
EOF

find_tool() {
    local name=$1 p
    if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return 0; fi
    for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Library/Android/sdk" /usr/local/lib/android/sdk; do
        [ -n "$root" ] || continue
        # Newest build-tools first.
        p=$(ls -1d "$root"/build-tools/*/"$name" 2>/dev/null | sort -V | tail -1)
        [ -n "$p" ] && { echo "$p"; return 0; }
        p=$(ls -1d "$root"/cmdline-tools/*/bin/"$name" 2>/dev/null | sort -V | tail -1)
        [ -n "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

DEXDUMP=$(find_tool dexdump || true)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$APK" 'classes*.dex' -d "$TMP" || {
    echo "::error::no classes*.dex in $APK" >&2
    exit 1
}

if [ -z "$DEXDUMP" ] && [ "$REQUIRE_DEXDUMP" = "1" ]; then
    echo "::error::dexdump not found, and --require-dexdump was given." >&2
    echo "         Looked on PATH and under ANDROID_HOME/ANDROID_SDK_ROOT build-tools." >&2
    echo "         Refusing to fall back to the name-only check: it cannot see an access-flag" >&2
    echo "         or signature change, so passing it would be a false green." >&2
    exit 1
fi

if [ -z "$DEXDUMP" ]; then
    # DEGRADED, and it says so. A DEX string pool holds every method NAME and
    # the class descriptor, so their absence is still conclusive; the parameter
    # descriptors are built from type ids rather than stored whole, so this arm
    # cannot see a signature CHANGE. Never silently skip — a contract check that
    # quietly does nothing is worse than not having one.
    echo "::warning::dexdump not found; falling back to a NAME-ONLY check (signatures unverified)." >&2
    rc=0
    if ! grep -qa 'Lorg/freedesktop/monado/auxiliary/MiniWindowLayout;' "$TMP"/classes*.dex; then
        echo "::error::$CLASS_DESC is NOT in the APK's DEX." >&2
        exit 1
    fi
    while IFS=$'\t' read -r name _; do
        [ -n "$name" ] || continue
        if grep -qa -- "$name" "$TMP"/classes*.dex; then
            printf '  ok?  %s (name only)\n' "$name"
        else
            printf '::error::MISSING from the release APK: %s\n' "$name"
            rc=1
        fi
    done <<< "$WANT"
    [ $rc -eq 0 ] || exit 1
    echo "MiniWindowLayout cross-APK contract present by NAME in $(basename "$APK") (install build-tools for the full check)"
    exit 0
fi

# One dexdump pass over every dex, sliced to the class we care about. The class
# can live in any of them (dex splitting is not stable across builds), so scan
# them all and concatenate.
DUMP="$TMP/dump.txt"
: > "$DUMP"
for d in "$TMP"/classes*.dex; do
    "$DEXDUMP" -d "$d" 2>/dev/null | awk -v cls="$CLASS_DESC" '
        $0 ~ /^Class #/ { inclass = 0 }
        $0 ~ /Class descriptor  : / {
            desc = $0
            sub(/.*: */, "", desc)
            gsub(/'"'"'/, "", desc)
            inclass = (desc == cls)
        }
        inclass { print }
    ' >> "$DUMP"
done

if [ ! -s "$DUMP" ]; then
    echo "::error::$CLASS_DESC is NOT in the APK's DEX." >&2
    echo "         Either the class was renamed/removed, or minification stripped it." >&2
    echo "         The -keep rule lives in src/xrt/targets/openxr_android/proguard-rules.pro." >&2
    exit 1
fi

# dexdump prints, per method:
#     name          : 'computeHintForActivity'
#     type          : '(Ljava/lang/Object;IIIIII)[I'
#     access        : 0x0009 (PUBLIC STATIC)
#
# The ACCESS LINE IS PART OF THE CONTRACT and dropping it was a real hole
# (#1401 review): with only name+type checked, demoting isBindingTell to PRIVATE
# or isEnabled to an instance method both still reported "ok", and either one
# breaks a reflecting caller at run time — getMethod() throws on the private
# one, invoke(null, ...) throws on the instance one. Capture all three.
PAIRS="$TMP/pairs.txt"
awk "
    /^ *name *: / { n = \$0; sub(/.*: *'/, \"\", n); sub(/'.*/, \"\", n); a = \"\"; t = \"\"; next }
    /^ *type *: / { t = \$0; sub(/.*: *'/, \"\", t); sub(/'.*/, \"\", t); next }
    /^ *access *: / { a = \$0; sub(/.*\(/, \"\", a); sub(/\).*/, \"\", a);
                      if (n != \"\" && t != \"\") { print n \"\t\" t \"\t\" a }
                      n = \"\"; t = \"\"; a = \"\" }
" "$DUMP" | sort -u > "$PAIRS"

rc=0
while IFS=$'\t' read -r name desc; do
    [ -n "$name" ] || continue
    line=$(grep -F "$name	$desc	" "$PAIRS" | head -1)
    if [ -z "$line" ]; then
        printf '::error::MISSING from the release APK: %s %s\n' "$name" "$desc"
        got=$(grep -F "$name	" "$PAIRS" | sed 's/^/           found instead: /')
        [ -n "$got" ] && printf '%s\n' "$got"
        rc=1
        continue
    fi
    access=${line##*$'\t'}
    # A cross-APK caller does getMethod() + invoke(null, ...). PRIVATE fails the
    # first, non-STATIC fails the second.
    case " $access " in
    *" PUBLIC "*) ;;
    *)
        printf '::error::%s %s is NOT PUBLIC (access: %s)\n' "$name" "$desc" "$access"
        rc=1
        continue
        ;;
    esac
    case " $access " in
    *" STATIC "*) ;;
    *)
        printf '::error::%s %s is NOT STATIC (access: %s)\n' "$name" "$desc" "$access"
        rc=1
        continue
        ;;
    esac
    printf '  ok   %s %s [%s]\n' "$name" "$desc" "$access"
done <<< "$WANT"

if [ $rc -ne 0 ]; then
    echo "::error::The MiniWindowLayout cross-APK contract (#1403) is broken in $APK." >&2
    echo "         It is APPEND-ONLY: add a method and bump contractVersion(); never change one." >&2
    exit 1
fi
echo "MiniWindowLayout cross-APK contract OK in $(basename "$APK")"
