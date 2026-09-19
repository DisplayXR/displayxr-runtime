#!/bin/sh
# Copyright 2019-2024, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0
# Author: Rylie Pavlik <rylie.pavlik@collabora.com>

# Formats all the source files in this project
#
# Usage: scripts/format-project.sh [--force]
#
# THIS REFORMATS THE WHOLE TREE, so it is pinned to one clang-format major
# version. src/xrt/.clang-format is a 2020-2022 config (BasedOnStyle: LLVM)
# authored for clang-format 14 — the newest name this script searched for, and
# the version Debian bookworm's `clang-format` package supplied to upstream
# Monado's CI. clang-format's LLVM base style is NOT stable across majors:
# running 21 over this tree rewrites trailing-comment alignment and macro
# spacing in files nobody touched, producing a diff that is pure noise and that
# the next person on a correct version reverts.
#
# Before the version check existed, the search below fell through to a bare
# `clang-format` whenever no versioned name matched (Ubuntu 26.04 ships only
# clang-format-19 / -21), so the script silently did exactly that.
#
# Escape hatches, in order of preference:
#   git clang-format                 — formats ONLY your changed lines; this is
#                                      the path CLAUDE.md and CONTRIBUTING.md
#                                      recommend and it needs no pinned major.
#   pipx install clang-format==14.*  — then re-run this script.
#   CLANGFORMAT=/path/to/clang-format-14 scripts/format-project.sh
#   scripts/format-project.sh --force  — run whatever was found anyway.
#
# An explicit CLANGFORMAT= is trusted (you picked it) and skips the check.

set -e

EXPECTED_MAJOR=14

FORCE=0
for arg in "$@"; do
        case "$arg" in
        --force) FORCE=1 ;;
        *) echo "Unknown option: $arg (usage: $0 [--force])" 1>&2; exit 2 ;;
        esac
done

# An explicitly-provided CLANGFORMAT is the user's own choice of binary, so it
# is honored as-is — same effect as --force.
CLANGFORMAT_WAS_SET=0
if [ "${CLANGFORMAT}" ]; then
        CLANGFORMAT_WAS_SET=1
fi

if [ ! "${CLANGFORMAT}" ]; then
        for fn in clang-format-14 clang-format-13 clang-format-12 clang-format-11 clang-format-10 clang-format-9 clang-format-8 clang-format-7 clang-format-6.0 clang-format; do
                if command -v $fn > /dev/null; then
                        CLANGFORMAT=$fn
                        break
                fi
        done
fi

if [ ! "${CLANGFORMAT}" ]; then
        echo "We need some version of clang-format, please install one!" 1>&2
        echo "This project is pinned to clang-format ${EXPECTED_MAJOR}: pipx install 'clang-format==${EXPECTED_MAJOR}.*'" 1>&2
        exit 1
fi

# Version gate: never silently reformat the tree with the wrong major.
if [ "${CLANGFORMAT_WAS_SET}" = 0 ] && [ "${FORCE}" = 0 ]; then
        FOUND_VERSION=$(${CLANGFORMAT} --version 2>/dev/null || echo "")
        # "Ubuntu clang-format version 21.1.8 (6ubuntu1)" -> 21
        FOUND_MAJOR=$(echo "${FOUND_VERSION}" | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p')
        if [ -z "${FOUND_MAJOR}" ]; then
                echo "ERROR: could not parse a major version out of '${CLANGFORMAT} --version':" 1>&2
                echo "         ${FOUND_VERSION}" 1>&2
                echo "       Refusing to reformat the tree with an unidentified clang-format." 1>&2
                echo "       Re-run with --force, or set CLANGFORMAT=<path>." 1>&2
                exit 1
        fi
        if [ "${FOUND_MAJOR}" != "${EXPECTED_MAJOR}" ]; then
                echo "ERROR: found ${CLANGFORMAT} = major ${FOUND_MAJOR}, but this project's" 1>&2
                echo "       src/xrt/.clang-format is pinned to clang-format ${EXPECTED_MAJOR}." 1>&2
                echo "       (${FOUND_VERSION})" 1>&2
                echo "" 1>&2
                echo "       Reformatting the whole tree with a different major rewrites files" 1>&2
                echo "       nobody touched — comment alignment and macro spacing drift between" 1>&2
                echo "       majors — so this is refused rather than done silently." 1>&2
                echo "" 1>&2
                echo "       Do one of:" 1>&2
                echo "         git clang-format                  # format ONLY your changes (preferred)" 1>&2
                echo "         pipx install 'clang-format==${EXPECTED_MAJOR}.*' && $0" 1>&2
                echo "         CLANGFORMAT=/path/to/clang-format-${EXPECTED_MAJOR} $0" 1>&2
                echo "         $0 --force        # reformat with ${FOUND_MAJOR} anyway" 1>&2
                exit 1
        fi
fi

(
        ${CLANGFORMAT} --version

        cd "$(dirname "$0")/.."

        find \
                src/xrt/auxiliary \
                src/xrt/compositor \
                src/xrt/drivers \
                src/xrt/include \
                src/xrt/ipc \
                src/xrt/state_trackers \
                src/xrt/targets \
                src/xrt/tracking \
                tests \
                \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
                -and -not \( -ipath \*/.cxx/\* \) \
                -exec "${CLANGFORMAT}" -i -style=file \{\} +
)
