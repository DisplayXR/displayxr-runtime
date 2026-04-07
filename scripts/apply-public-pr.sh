#!/bin/bash
# apply-public-pr.sh — Fetch a PR from the public displayxr-runtime repo
# and create a local branch for review/merge into the private dev repo.
#
# Usage: ./scripts/apply-public-pr.sh <PR_NUMBER>
# Example: ./scripts/apply-public-pr.sh 42

set -euo pipefail

PR_NUM="${1:?Usage: $0 <PR_NUMBER>}"
PUBLIC_REPO="DisplayXR/displayxr-runtime"
BRANCH="community/pr-${PR_NUM}"

echo "=== Fetching PR #${PR_NUM} from ${PUBLIC_REPO} ==="

# Get PR info
PR_INFO=$(gh pr view "$PR_NUM" --repo "$PUBLIC_REPO" --json title,headRefName,author,body)
PR_TITLE=$(echo "$PR_INFO" | jq -r '.title')
PR_AUTHOR=$(echo "$PR_INFO" | jq -r '.author.login')
PR_HEAD=$(echo "$PR_INFO" | jq -r '.headRefName')

echo "Title:  $PR_TITLE"
echo "Author: $PR_AUTHOR"
echo "Branch: $PR_HEAD"

# Add public repo as a remote (if not already)
if ! git remote get-url public &>/dev/null; then
    git remote add public "https://github.com/${PUBLIC_REPO}.git"
fi

# Fetch the PR ref
git fetch public "pull/${PR_NUM}/head:${BRANCH}"

echo ""
echo "=== Created local branch: ${BRANCH} ==="
echo ""
echo "Next steps:"
echo "  git checkout ${BRANCH}"
echo "  # Review the changes"
echo "  git log main..${BRANCH} --oneline"
echo "  git diff main..${BRANCH}"
echo "  # Merge into main when ready"
echo "  git checkout main"
echo "  git merge ${BRANCH} --no-ff -m \"Merge community PR #${PR_NUM}: ${PR_TITLE} (by @${PR_AUTHOR})\""
echo ""
echo "After merging, close the public PR:"
echo "  gh pr close ${PR_NUM} --repo ${PUBLIC_REPO} -c \"Merged internally. Thanks @${PR_AUTHOR}!\""
