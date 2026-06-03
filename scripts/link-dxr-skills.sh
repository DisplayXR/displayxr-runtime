#!/usr/bin/env bash
# Symlink the DisplayXR release skills into the user-level skills dir so
# they're invocable from ANY working directory (not just inside this
# repo). The canonical, git-tracked copies live in this repo's
# .claude/skills/; the symlinks make them globally reachable on this
# machine without copying (no drift).
#
# Why: Claude Code resolves skills from (a) the launch repo's
# .claude/skills/ and (b) ~/.claude/skills/. Release work for sibling
# components (mcp, leia-plugin, shell, gauss) + the meta-installer is
# driven FROM the displayxr-runtime hub, so the skills must be reachable
# from the runtime repo AND ideally from anywhere. Committing them here
# satisfies "shared via git"; symlinking into ~/.claude/skills/ satisfies
# "reachable from any cwd". One source of truth (this repo), one
# per-machine setup command (this script).
#
# Idempotent — safe to re-run. Run once per machine (or let
# setup-displayxr.{sh,bat} call it). On Windows, see the .bat sibling
# note in CLAUDE.md (mklink /D), since this script is bash-only.
#
# Usage:
#   ./scripts/link-dxr-skills.sh            # create/refresh the symlinks
#   ./scripts/link-dxr-skills.sh --check    # report state, change nothing
#   ./scripts/link-dxr-skills.sh --unlink   # remove the symlinks

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SKILLS_SRC="$REPO_ROOT/.claude/skills"
USER_SKILLS="$HOME/.claude/skills"

# Skills to expose globally. Add a name here when a new hub-driven skill
# lands in .claude/skills/. (The runtime's own /release is NOT listed —
# it's only meaningful inside this repo, so it stays repo-scoped.)
SKILLS=(dxr-release installer-release sync-website)

MODE="${1:-link}"

C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
[ -t 1 ] || { C_GREEN=; C_YELLOW=; C_RED=; C_RESET=; }
ok()   { printf '%s OK %s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%sWARN%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
err()  { printf '%sERR %s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }

mkdir -p "$USER_SKILLS"

for name in "${SKILLS[@]}"; do
    src="$SKILLS_SRC/$name"
    dst="$USER_SKILLS/$name"

    if [ ! -d "$src" ]; then
        err "$name: canonical source missing at $src — skipping."
        continue
    fi

    case "$MODE" in
        --check)
            if [ -L "$dst" ]; then
                target="$(readlink "$dst")"
                if [ "$target" = "$src" ]; then
                    ok "$name → $src"
                else
                    warn "$name → $target (expected $src)"
                fi
            elif [ -e "$dst" ]; then
                warn "$name: $dst exists and is NOT a symlink (a real copy?). Leaving it."
            else
                warn "$name: not linked."
            fi
            ;;

        --unlink)
            if [ -L "$dst" ]; then
                rm "$dst"; ok "unlinked $name"
            elif [ -e "$dst" ]; then
                warn "$name: $dst is a real file/dir, not a symlink — NOT removing."
            else
                ok "$name: already absent"
            fi
            ;;

        link)
            if [ -L "$dst" ] && [ "$(readlink "$dst")" = "$src" ]; then
                ok "$name already linked → $src"
            elif [ -e "$dst" ] && [ ! -L "$dst" ]; then
                # A real (non-symlink) copy is in the way — the user had a
                # manual mirror. Don't clobber silently; tell them.
                err "$name: $dst is a real file/dir (legacy manual copy). Remove it, then re-run, to switch to the git-tracked symlink."
            else
                rm -f "$dst"
                ln -s "$src" "$dst"
                ok "linked $name → $src"
            fi
            ;;

        *)
            err "unknown mode '$MODE' (use: link | --check | --unlink)"; exit 1 ;;
    esac
done

if [ "$MODE" = "link" ]; then
    printf '\nDone. /dxr-release, /installer-release, and /sync-website are now invocable from any directory.\n'
    printf 'Canonical copies stay git-tracked in %s — edit there, never the symlink target.\n' "$SKILLS_SRC"
fi
