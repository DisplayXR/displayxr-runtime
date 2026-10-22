#!/usr/bin/env bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# CI gate for #1790: the runtime a job just built reports the version of the
# checkout it was built from, and its package is named after the same thing.
#
# cmake/DisplayXRVersion.cmake derives the version; this script re-derives the
# expectation independently (plain `git describe`, and on a tag push the tag
# GitHub says it is building) and checks the ARTIFACTS against it -- the stamp
# CMake wrote next to the build, the strings inside the shipped binaries, what
# `displayxr-cli info` prints, and the package file names. Before #1790 every
# non-Windows build and every untagged build reported 1.6.0, and nothing
# noticed, because nothing looked at the artifacts.
#
# usage: check_runtime_version.sh --build-dir DIR
#            [--binary FILE]...   runtime library / executable that must embed the describe
#            [--cli FILE]         displayxr-cli whose `info` must print the version + git tag
#            [--package FILE]...  installer / package whose name must carry the version
#
# Needs the checkout's tags (CI: actions/checkout with fetch-depth: 0).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR=""
CLI=""
BINARIES=()
PACKAGES=()
while [ "$#" -gt 0 ]; do
	case "$1" in
	--build-dir) BUILD_DIR="$2"; shift 2 ;;
	--binary) BINARIES+=("$2"); shift 2 ;;
	--cli) CLI="$2"; shift 2 ;;
	--package) PACKAGES+=("$2"); shift 2 ;;
	*) echo "usage: $0 --build-dir DIR [--binary FILE]... [--cli FILE] [--package FILE]..." >&2; exit 2 ;;
	esac
done
[ -n "$BUILD_DIR" ] || { echo "error: --build-dir is required" >&2; exit 2; }

fail=0
err() { echo "::error::$*"; fail=1; }

# --- Expectation, from git alone ------------------------------------------------
want_desc="$(git -C "$ROOT" describe --tags --match 'v[0-9]*' --exclude 'v*[!0-9.]*' 2>/dev/null || true)"
if [ -z "$want_desc" ]; then
	echo "::error::git describe finds no vX.Y.Z tag -- the checkout needs its history and tags (fetch-depth: 0)."
	exit 1
fi
if [ "${GITHUB_REF_TYPE:-}" = "tag" ] && [ "$want_desc" != "${GITHUB_REF_NAME:-}" ]; then
	err "tag build of '${GITHUB_REF_NAME:-}' describes as '$want_desc' -- the artifacts would name another release."
fi
want_ver="$(sed -E 's/^v([0-9]+\.[0-9]+\.[0-9]+).*/\1/' <<<"$want_desc")"
case "$want_desc" in
v*-*-g*) want_pkg="$(sed -E 's/^v([0-9.]+)-([0-9]+)-g([0-9a-f]+)$/\1+\2.g\3/' <<<"$want_desc")" ;;
*) want_pkg="$want_ver" ;;
esac
echo "expected: version $want_ver, git tag '$want_desc', package '$want_pkg'"

# --- The stamp CMake wrote next to the build ------------------------------------
stamp="$BUILD_DIR/displayxr-version.txt"
if [ ! -f "$stamp" ]; then
	err "$stamp missing -- the build did not go through cmake/DisplayXRVersion.cmake."
else
	field() { sed -n "s/^$1=//p" "$stamp" | tr -d '\r'; }
	[ "$(field DXR_RUNTIME_VERSION)" = "$want_ver" ] || err "stamp version '$(field DXR_RUNTIME_VERSION)' != '$want_ver'"
	[ "$(field DXR_RUNTIME_VERSION_DESCRIBE)" = "$want_desc" ] || err "stamp git tag '$(field DXR_RUNTIME_VERSION_DESCRIBE)' != '$want_desc'"
	[ "$(field DXR_RUNTIME_VERSION_PACKAGE)" = "$want_pkg" ] || err "stamp package '$(field DXR_RUNTIME_VERSION_PACKAGE)' != '$want_pkg'"
	[ "$(field DXR_RUNTIME_VERSION_SOURCE)" = "git" ] || err "stamp source '$(field DXR_RUNTIME_VERSION_SOURCE)' != 'git' (a CI build must version from its own tags)"
	echo "  stamp:   $(tr '\n' ' ' <"$stamp")"
fi

# --- Binaries: u_git_tag is a literal string in every one that links aux_util --
for bin in ${BINARIES[@]+"${BINARIES[@]}"}; do
	if [ ! -f "$bin" ]; then
		err "$bin missing"
	elif command -v strings >/dev/null 2>&1; then
		# Into a variable, then a here-string: `strings | grep -q` under pipefail
		# fails whenever grep exits before strings is done writing (SIGPIPE).
		bin_strings="$(strings -a "$bin")"
		if grep -qxF -- "$want_desc" <<<"$bin_strings"; then
			echo "  ok:      $bin embeds '$want_desc'"
		else
			err "$bin does not embed '$want_desc'"
		fi
	elif LC_ALL=C grep -qaF -- "$want_desc" "$bin"; then
		echo "  ok:      $bin embeds '$want_desc' (grep -a)"
	else
		err "$bin does not embed '$want_desc'"
	fi
done

# --- displayxr-cli info: the number xrGetInstanceProperties reports -------------
if [ -n "$CLI" ]; then
	info="$("$CLI" info 2>/dev/null | tr -d '\r' || true)"
	# The " :: Runtime" block comes first: "\tversion:     X.Y.Z", "\tgit-tag:     'vX.Y.Z...'".
	got_ver="$(sed -n 's/^[[:space:]]*version:[[:space:]]*//p' <<<"$info")"
	got_tag="$(sed -n "s/^[[:space:]]*git-tag:[[:space:]]*'\(.*\)'.*/\1/p" <<<"$info")"
	got_ver="${got_ver%%$'\n'*}"
	got_tag="${got_tag%%$'\n'*}"
	[ "$got_ver" = "$want_ver" ] || err "displayxr-cli info: version '$got_ver' != '$want_ver'"
	[ "$got_tag" = "$want_desc" ] || err "displayxr-cli info: git-tag '$got_tag' != '$want_desc'"
	if [ "$got_ver" = "$want_ver" ] && [ "$got_tag" = "$want_desc" ]; then
		echo "  ok:      displayxr-cli info reports $got_ver '$got_tag'"
	fi
fi

# --- Package names ----------------------------------------------------------------
for pkg in ${PACKAGES[@]+"${PACKAGES[@]}"}; do
	name="$(basename "$pkg")"
	if [ ! -f "$pkg" ]; then
		err "$pkg missing"
	elif [[ "$name" == *[-_]"$want_pkg"[._]* ]]; then
		echo "  ok:      $name carries '$want_pkg'"
	else
		err "$name does not carry '$want_pkg'"
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "Runtime version check FAILED (#1790)."
	exit 1
fi
echo "Runtime version check passed (#1790)."
