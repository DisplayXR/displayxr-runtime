#!/usr/bin/env bash
#
# Mirror versions.json byte-for-byte into DisplayXR/displayxr-installer.
#
# This is the ONE copy of the mirror logic. Three workflows call it:
#
#   .github/workflows/versions-bump.yml    sibling component release (dispatch)
#   .github/workflows/build-windows.yml    runtime self-bump on a v* tag
#   .github/workflows/versions-mirror.yml  any push to main touching the file
#
# The third exists because the first two only fire on a *release*: a
# versions.json field edited by hand and merged as an ordinary PR used to
# mirror nothing, and displayxr-installer's publish-bundle.yml
# `assert-versions-in-sync` job hard-fails the whole bundle on any diff
# against runtime/main. So one hand-edited pin silently blocked every bundle
# release until the next component bump happened to carry it across.
#
# IDEMPOTENT by design: if the installer's copy already matches, this commits
# nothing and exits 0. That is what makes it safe to call from more than one
# trigger — the second caller is always a no-op.
#
# Usage:
#   scripts/mirror_versions_json.sh <commit-subject> <commit-body>
#
# Environment:
#   GH_TOKEN          required — displayxr-publish-bot installation token with
#                     write access to DisplayXR/displayxr-installer. Same
#                     credential path in all three callers; no new secret.
#   MIRROR_REPO_URL   optional — override the clone URL. For local dry-runs
#                     against a throwaway clone; CI never sets it.
#   MIRROR_WORKDIR    optional — clone destination (default: a fresh mktemp -d).
#   MIRROR_NO_PUSH    optional — "1" does everything but the push (dry-run).
#
set -euo pipefail

SUBJECT=${1:?usage: mirror_versions_json.sh <commit-subject> <commit-body>}
BODY=${2:-}

SRC=versions.json
if [ ! -f "$SRC" ]; then
  echo "::error::$SRC not found — run this from the repo root"
  exit 1
fi

if [ -n "${MIRROR_REPO_URL:-}" ]; then
  URL=$MIRROR_REPO_URL
else
  : "${GH_TOKEN:?GH_TOKEN is required (publish-bot installation token)}"
  URL="https://x-access-token:${GH_TOKEN}@github.com/DisplayXR/displayxr-installer.git"
fi

DEST=${MIRROR_WORKDIR:-$(mktemp -d)}

# Clone quietly: the URL carries the token, and --quiet keeps it out of any
# future `set -x`-style diagnostics as well as the normal progress output.
git clone --quiet "$URL" "$DEST"
cp "$SRC" "$DEST/versions.json"
cd "$DEST"

if git diff --quiet versions.json; then
  echo "displayxr-installer/versions.json already matches; no mirror commit needed"
  exit 0
fi

git config user.name  "displayxr-publish-bot[bot]"
git config user.email "displayxr-publish-bot@users.noreply.github.com"
git add versions.json
git commit -m "$SUBJECT" -m "$BODY"

if [ "${MIRROR_NO_PUSH:-}" = "1" ]; then
  echo "MIRROR_NO_PUSH=1 — committed locally, not pushing:"
  git --no-pager log -1 --stat
  exit 0
fi

# Retry once if the installer's main moved underneath us (unchanged from the
# inline version this replaced).
git push origin HEAD:main || (git pull --rebase origin main && git push origin HEAD:main)
