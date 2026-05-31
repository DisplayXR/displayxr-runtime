#!/usr/bin/env bash
# DisplayXR dev orchestrator (#283) — macOS.
#
# One command takes a fresh contributor from `git clone` to a working
# DisplayXR dev box: downloads each component's release asset from the
# canonical GitHub Releases page (tags pinned in versions.json), runs
# `sudo installer -pkg` on each, verifies the install location.
#
# Windows is a follow-up PR after the Leia plug-in cuts its first
# user-facing installer release.
#
# Usage:
#   ./scripts/setup-displayxr.sh                # runtime (+ bundled sim-display)
#   ./scripts/setup-displayxr.sh --with mcp     # also DisplayXR MCP Tools
#   ./scripts/setup-displayxr.sh --with-demos   # also clone each demo into demos/
#   ./scripts/setup-displayxr.sh --dry-run      # print plan, install nothing
#   ./scripts/setup-displayxr.sh --uninstall    # chain known uninstallers
#   ./scripts/setup-displayxr.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSIONS_JSON="$REPO_ROOT/versions.json"

# shellcheck source=lib/components.sh
. "$SCRIPT_DIR/lib/components.sh"

# --- ANSI helpers (no-color when stdout is not a TTY) ---
if [ -t 1 ]; then
    C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RESET=$'\033[0m'
else
    C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_RESET=""
fi
log()  { printf '%s\n' "$*"; }
info() { printf '%s==>%s %s\n' "$C_BOLD" "$C_RESET" "$*"; }
warn() { printf '%sWARN%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
err()  { printf '%sERROR%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
ok()   { printf '%s OK %s %s\n' "$C_GREEN" "$C_RESET" "$*"; }

# --- arg parsing ---
WITH_MCP=0
WITH_DEMOS=0
DRY_RUN=0
ACTION=install   # install | uninstall | help

usage() {
    cat <<EOF
${C_BOLD}DisplayXR dev orchestrator${C_RESET} (#283, macOS)

Usage: $0 [flags]

  --with mcp        Also install DisplayXR MCP Tools (when a macOS
                    asset is available; warn+skip otherwise).
  --with-demos      Clone each DisplayXR/displayxr-demo-* repo into
                    ./demos/<name>/ (does not build them; see each
                    demo's README for build steps).
  --dry-run         Print everything that would be downloaded and
                    installed; perform no privileged operations.
  --uninstall       Chain known uninstallers (runtime + sim-display
                    plug-in via the .pkg's bundled uninstall.sh).
  -h, --help        Show this message.

Pins live in ${C_DIM}versions.json${C_RESET}. Bump that file to upgrade
the dev box to a newer release matrix.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --with)
            shift
            case "${1:-}" in
                mcp) WITH_MCP=1 ;;
                "") err "--with requires an argument (currently: mcp)"; exit 2 ;;
                *)  err "Unknown --with target: $1 (supported: mcp)"; exit 2 ;;
            esac
            ;;
        --with-demos) WITH_DEMOS=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        --uninstall)  ACTION=uninstall ;;
        -h|--help)    ACTION=help ;;
        *) err "Unknown flag: $1"; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$ACTION" = "help" ]; then
    usage
    exit 0
fi

# --- preflight ---

if [[ "$OSTYPE" != darwin* ]]; then
    err "This orchestrator is macOS-only. The Windows path is tracked as a"
    err "follow-up to issue #283 (Leia plug-in must cut its first user-facing"
    err "release first)."
    exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
    err "GitHub CLI (gh) not found."
    err "Install with: brew install gh"
    exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
    err "GitHub CLI is not authenticated."
    err "Run: gh auth login"
    exit 1
fi

# Read versions.json via python3 (guaranteed on macOS 13+; avoids a
# dependency on system jq, which is not pre-installed).
if [ ! -f "$VERSIONS_JSON" ]; then
    err "versions.json not found at $VERSIONS_JSON"
    exit 1
fi
pinned_tag() {
    /usr/bin/env python3 -c "import json,sys; d=json.load(open(sys.argv[1])); k=sys.argv[2].split('.'); v=d
for p in k: v=v[p]
print(v)" "$VERSIONS_JSON" "$1"
}

# --- uninstall path ---

if [ "$ACTION" = "uninstall" ]; then
    UNINSTALL_SH="/Library/Application Support/DisplayXR/uninstall.sh"
    if [ -f "$UNINSTALL_SH" ]; then
        info "Chaining $UNINSTALL_SH"
        if [ "$DRY_RUN" -eq 1 ]; then
            log "  (dry-run) would run: sudo bash \"$UNINSTALL_SH\""
        else
            sudo bash "$UNINSTALL_SH"
            ok "Runtime uninstalled."
        fi
    else
        warn "No runtime uninstaller found at $UNINSTALL_SH (already removed?)."
    fi

    if [ -d "$REPO_ROOT/demos" ]; then
        warn "$REPO_ROOT/demos/ is present. Leaving it in place — remove"
        warn "manually if intended (it may contain local changes)."
    fi
    exit 0
fi

# --- install path ---

# Keep sudo timestamp alive so the per-component `sudo installer` calls
# don't each re-prompt. -v primes; the background keep-alive refreshes.
if [ "$DRY_RUN" -eq 0 ]; then
    info "This script will run 'sudo installer' for each component. You may be prompted for your password."
    sudo -v
    ( while true; do sudo -n true 2>/dev/null; sleep 50; kill -0 $$ 2>/dev/null || exit; done ) &
    SUDO_KEEPALIVE_PID=$!
    trap 'kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true' EXIT
fi

STAGING="$(mktemp -d -t dxr-setup)"
# Append to the EXIT trap rather than overwriting the sudo keepalive cleanup.
if [ "$DRY_RUN" -eq 0 ]; then
    trap 'kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true; rm -rf "$STAGING"' EXIT
else
    trap 'rm -rf "$STAGING"' EXIT
fi

# Build the install list. Runtime is always in. Opt-ins gated by flags.
COMPONENTS=(runtime)
[ "$WITH_MCP" -eq 1 ] && COMPONENTS+=(mcp_tools)

install_component() {
    local name="$1"
    local repo tag glob marker
    repo="$(component_field "$name" REPO)"
    glob="$(component_field "$name" PKG_MACOS)"
    marker="$(component_field "$name" INSTALL_MARKER_MACOS)"
    tag="$(pinned_tag "$name" 2>/dev/null || true)"

    if [ -z "$tag" ]; then
        err "versions.json has no pin for component '$name'."
        return 1
    fi

    info "$name @ $tag  (repo: $repo)"

    if [ -z "$glob" ]; then
        warn "$name: no macOS asset is published for this component today."
        warn "       Skipping. (This is expected for shell / leia / mcp on this PR;"
        warn "       see docs/getting-started/full-stack-install.md.)"
        return 0
    fi

    # Validate the pin exists in the source repo's releases before downloading.
    if ! gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
        err "versions.json pins $name to '$tag', but"
        err "  gh release view $tag --repo $repo"
        err "failed. Bump the pin in versions.json, or verify the repo is accessible."
        return 1
    fi

    # Confirm the macOS asset exists on this release — gh download will
    # fail with a noisy stack otherwise, and we want a clean warn-and-skip
    # when a pre-#279 runtime tag is selected.
    local asset_count
    asset_count="$(gh release view "$tag" --repo "$repo" --json assets \
        --jq "[.assets[].name | select(test(\"$(printf '%s' "$glob" | sed 's/\*/.*/g')\"))] | length")"
    if [ "$asset_count" -eq 0 ]; then
        warn "$name @ $tag: no asset matching '$glob' is attached to this release."
        warn "       Pin a newer tag in versions.json once one is available."
        warn "       (Runtime macOS .pkg first appears in the release immediately after PR #279.)"
        return 0
    fi

    local subdir="$STAGING/$name"
    mkdir -p "$subdir"
    gh release download "$tag" --repo "$repo" --pattern "$glob" --dir "$subdir"

    local pkg
    pkg="$(find "$subdir" -maxdepth 1 -name "*.pkg" -type f | head -1)"
    if [ -z "$pkg" ]; then
        err "$name: download succeeded but no .pkg landed in $subdir"
        return 1
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        log "  (dry-run) would run: sudo installer -pkg \"$pkg\" -target /"
    else
        info "Installing $(basename "$pkg")"
        sudo installer -pkg "$pkg" -target /
        if [ -n "$marker" ] && [ ! -e "$marker" ]; then
            err "$name: post-install marker missing: $marker"
            err "       The .pkg ran but didn't lay down the expected file. Check the"
            err "       installer log: /var/log/install.log"
            return 1
        fi
        ok "$name installed."
    fi
}

# --- run the install loop ---

FAIL=0
for c in "${COMPONENTS[@]}"; do
    install_component "$c" || FAIL=1
done

# --- --with-demos (always after binary installs) ---

if [ "$WITH_DEMOS" -eq 1 ]; then
    info "Discovering DisplayXR demo repos…"
    DEMOS_DIR="$REPO_ROOT/demos"
    [ "$DRY_RUN" -eq 0 ] && mkdir -p "$DEMOS_DIR"
    # gh has jq embedded — no system jq needed.
    DEMO_REPOS="$(gh repo list DisplayXR --limit 50 --json name --jq '.[].name | select(startswith("displayxr-demo-"))' || true)"
    if [ -z "$DEMO_REPOS" ]; then
        warn "No displayxr-demo-* repos visible to this gh account."
    fi
    while IFS= read -r repo_name; do
        [ -z "$repo_name" ] && continue
        target="$DEMOS_DIR/$repo_name"
        if [ -d "$target/.git" ]; then
            log "  $repo_name already cloned at $target — skipping."
            continue
        fi
        if [ "$DRY_RUN" -eq 1 ]; then
            log "  (dry-run) would run: gh repo clone DisplayXR/$repo_name $target"
        else
            info "Cloning DisplayXR/$repo_name"
            gh repo clone "DisplayXR/$repo_name" "$target"
            log "    See $target/README.md for build instructions."
        fi
    done <<< "$DEMO_REPOS"
fi

# --- link release skills into ~/.claude/skills (global invocation) ---
# The /dxr-release + /installer-release skills live git-tracked in this
# repo's .claude/skills/; symlink them into the user-level skills dir so
# they're invocable from any working directory (Claude Code reads skills
# from the launch repo + ~/.claude/skills). Idempotent; harmless to
# re-run. Non-privileged, so it runs regardless of component-install
# outcome — but is skipped on --dry-run.
if [ "$DRY_RUN" -eq 0 ]; then
    if [ -x "$SCRIPT_DIR/link-dxr-skills.sh" ]; then
        info "Linking DisplayXR release skills into ~/.claude/skills"
        "$SCRIPT_DIR/link-dxr-skills.sh" || warn "skill linking failed (non-fatal)."
    fi
else
    log "  (dry-run) would link release skills via scripts/link-dxr-skills.sh"
fi

# --- smoke verification (skip on dry-run; nothing was installed) ---

if [ "$DRY_RUN" -eq 0 ] && [ "$FAIL" -eq 0 ]; then
    info "Smoke verification"
    SMOKE_OK=1
    MANIFEST="/Library/Application Support/DisplayXR/DisplayProcessors/200-sim-display.json"
    if [ -f "$MANIFEST" ]; then
        ok "sim-display plug-in manifest present — runtime is discoverable."
    else
        warn "Manifest missing: $MANIFEST"
        warn "  (Only meaningful if the runtime install actually ran. If runtime was"
        warn "   skipped above due to missing macOS asset, this warning is expected.)"
        SMOKE_OK=0
    fi
    ACTIVE_RUNTIME=/etc/xdg/openxr/1/active_runtime.json
    if [ -L "$ACTIVE_RUNTIME" ] || [ -f "$ACTIVE_RUNTIME" ]; then
        ok "Active OpenXR runtime registered at $ACTIVE_RUNTIME"
    else
        warn "Active runtime symlink missing: $ACTIVE_RUNTIME"
        SMOKE_OK=0
    fi
    if [ "$SMOKE_OK" -eq 1 ]; then
        echo
        ok "DisplayXR dev box is ready."
    fi
fi

exit "$FAIL"
