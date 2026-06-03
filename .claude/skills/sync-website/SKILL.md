---
name: sync-website
description: Editorial sync pass for the displayxr-website — driven from the displayxr-runtime hub. The mechanical facts (versions, demo cards, repo list, extension names) are auto-synced by the website's sync-org.yml; THIS skill handles the class-B narrative that needs judgment — surfacing new ADRs, new repos, new extensions, new demos, and closed milestones, then authoring the matching roadmap / architecture / ecosystem prose into the website's hand-written TSX and opening a PR. Use when a release or milestone has landed and the site's prose has fallen behind.
---

# sync-website — editorial drift pass for displayxr.org

## What this is (and is NOT)

The DisplayXR website is kept in sync with the org by a **two-layer** system
(design: `displayxr-website/docs/org-sync.md`):

- **Mechanical layer (NOT this skill).** `displayxr-website/scripts/sync-org.mjs`
  + `.github/workflows/sync-org.yml` regenerate `lib/data/generated/*.json` (+ 2D
  icons) from `versions.json` and the GitHub API, and **direct-commit to the
  website's `main`** on a daily cron / on `org-changed` dispatch after any
  release. Versions, demo cards, the repo list, and extension *names* take care
  of themselves. Do not duplicate that here.

- **Editorial layer (THIS skill).** The narrative that needs a human's judgment:
  roadmap phrasing, architecture prose, ecosystem blurbs, extension
  titles/descriptions, device copy. The generator can *detect* that these inputs
  changed (new ADR, new repo, new extension header, new demo, closed milestone)
  but must never auto-write them. This skill reads the new sources and authors
  tasteful prose into the **hand-written** TSX, then opens a **PR** — because
  prose warrants a glance, unlike the mechanical facts.

**Mechanical commit = facts (direct to main). Skill PR = narrative.** They never
touch the same fields: the skill edits authored TSX (`lib/data/roadmap.ts`,
`lib/data/ecosystem.ts`, `lib/constants.ts`, `app/architecture/page.tsx`,
`app/extensions/page.tsx`, `lib/data/devices.ts`) and **never** the
`lib/data/generated/*.json` files.

## Why hub-homed + parameterized

Like `/dxr-release`, this lives in `displayxr-runtime/.claude/skills/` and is
symlinked into `~/.claude/skills/` by `scripts/link-dxr-skills.sh`, so it's
invocable from anywhere. It operates on a **fresh temp clone** of
`displayxr-website` — it does NOT assume cwd is the website repo.

## Syntax

```
/sync-website [--dry-run] [focus]

  --dry-run   detect + report the editorial gaps, change nothing, open no PR.
  focus       optional: narrow to one surface — roadmap | architecture |
              ecosystem | extensions | devices. Omit to scan all.
```

Examples:
```
/sync-website                  # full scan → author prose → open PR
/sync-website --dry-run        # just tell me what's drifted
/sync-website architecture     # only reconcile new ADRs into /architecture
```

This skill makes **outward-facing changes** (opens a PR, pushes a branch). The
`--dry-run` form is safe and read-only — prefer it first if you're unsure what
will change.

## CRITICAL: run the detection inline, author the prose deliberately

The detection (PHASE 1–2) is mechanical bash — run it directly. The authoring
(PHASE 3) is the judgment part: read each new source and write copy that matches
the surrounding voice. Do NOT batch-generate boilerplate; the whole point of the
editorial layer is that a human-quality sentence beats a templated one. If a gap
is ambiguous (e.g. an ADR that doesn't obviously map to any page), surface it in
the report and leave it for the user rather than inventing a section.

---

## PHASE 1: CLONE + LOAD THE SIGNAL SNAPSHOT

The website's `_meta.json` + `generated/*.json` are the generator's current view
of the org. They're already on `main` (the mechanical workflow keeps them fresh).
Read them from a temp clone.

```bash
WORK=$(mktemp -d)
gh repo clone DisplayXR/displayxr-website "$WORK/web" -- --quiet
cd "$WORK/web"

# Belt-and-braces: make sure the snapshot is current before diffing prose
# against it (in case a release landed since the last cron and no dispatch
# fired). Needs Node; the generator is dependency-free. This may write to
# generated/* + public/ in the WORKING TREE only — we never commit it here
# (that's the mechanical workflow's job); we just want fresh signals.
node scripts/sync-org.mjs || echo "WARN: generator failed; using committed snapshot"

META=lib/data/generated/_meta.json
jq -r '.signals.adrs[]'      "$META" | sort > /tmp/sw_adrs.txt
jq -r '.signals.repos[]'     "$META" | sort > /tmp/sw_repos.txt
jq -r '.signals.demoRepos[]' "$META" | sort > /tmp/sw_demos.txt
```

### The editorial baseline (so ADR detection converges)

Repos and extensions self-correct: once you add a repo to `ecosystem.ts` or an
extension to the page, the grep checks below stop flagging them. **ADRs don't** —
the architecture page summarizes decisions in product terms and never cites ADR
numbers, so "is this ADR on the page" is always false and would re-flag every ADR
on every run. To make ADR detection converge, this skill keeps a hand-owned
**baseline** of ADRs it has already *triaged* (surfaced OR deliberately skipped):

```bash
BASE=lib/data/editorial-baseline.json   # NOT generator-owned; this skill owns it
[ -f "$BASE" ] || echo '{"reviewedAdrs":[]}' > "$BASE"
jq -r '.reviewedAdrs[]' "$BASE" 2>/dev/null | sort > /tmp/sw_adr_base.txt
```

First run: the baseline is empty, so the whole ADR backlog is "new" — that's the
intended one-time reconciliation. After PHASE 3 you write every triaged ADR back
into the baseline (whether you surfaced it or skipped it), so subsequent runs see
only the delta.

---

## PHASE 2: DETECT THE EDITORIAL GAPS

Each check compares a generator signal against what the **authored** TSX already
says. A gap = "the fact exists but the prose hasn't caught up." Skip any check
not in `focus` when `focus` is given.

### 2.1 New ADRs → `/architecture`
```bash
# ADRs present in the org but not yet in this skill's reviewed baseline.
comm -23 /tmp/sw_adrs.txt /tmp/sw_adr_base.txt | sed 's/^/NEW-ADR /'
```
For each `NEW-ADR`, fetch the ADR body to judge whether it's user-facing enough
to surface (many ADRs are internal and should NOT go on the marketing site —
that's a judgment call, not an auto-add):
```bash
# adr path is "<repo>/<path>" — split and fetch
repo=${adr%%/*}; path=${adr#*/}
gh api "repos/DisplayXR/$repo/contents/$path" --jq '.content' | base64 -d | head -40
```

### 2.2 New repos → ecosystem grid + REPO_URLS
```bash
# Public, non-archived repos absent from the ecosystem data / constants.
while read -r r; do
  grep -q "\"DisplayXR/$r\"\|/$r\"" lib/data/ecosystem.ts lib/constants.ts 2>/dev/null \
    || echo "MISSING-REPO $r"
done < /tmp/sw_repos.txt
```
Filter out repos that intentionally aren't featured (test-only forks, archived
mirrors, `.github`, `displayxr-website` itself). Use the repo description:
```bash
gh repo view "DisplayXR/$r" --json description,repositoryTopics \
  --jq '{desc:.description, topics:[.repositoryTopics[]?.name]}'
```

### 2.3 New extensions → `/extensions`
```bash
# Extension headers the extensions page doesn't list yet.
jq -r '.[].name' lib/data/generated/extensions.json | while read -r ext; do
  grep -q "$ext" app/extensions/page.tsx || echo "MISSING-EXT $ext"
done
```
For each, fetch the header's top comment to write an accurate title/description:
```bash
gh api "repos/DisplayXR/displayxr-extensions/contents/include/openxr/$ext.h" \
  --jq '.content' | base64 -d | sed -n '1,40p'
```

### 2.4 Demos → roadmap mention
Demo *cards* are mechanical (already rendered from `generated/demos.json`). The
editorial gap is only the roadmap's "expand demos" narrative — check whether a
newly-added demo deserves a roadmap line. Usually low-priority; report, don't
force.
```bash
comm -13 <(grep -oE 'displayxr-demo-[a-z]+' lib/data/roadmap.ts | sort -u) /tmp/sw_demos.txt
```

### 2.5 Closed milestones → roadmap phases
```bash
gh api 'repos/DisplayXR/displayxr-runtime/milestones?state=closed&per_page=20' \
  --jq '.[] | {title, closed_at, url:.html_url}'
```
A milestone closed since the last roadmap edit may mean a "Now"/"Next" item
should move to "Done". Judgment call — surface the candidates.

### 2.6 Build the gap report
Collect all `MISSING*` lines + milestone/ADR candidates into a structured list.
If `--dry-run`, print this report (PHASE 5 format) and **STOP** — clone can be
deleted, no branch, no PR.

---

## PHASE 3: AUTHOR THE PROSE  (skip if --dry-run)

For each confirmed gap, edit the matching authored file. **Match the existing
voice** — read 2–3 neighboring entries first and mirror their length, tone, and
structure. Concrete mapping:

| Gap | File | What to write |
|---|---|---|
| User-facing new ADR | `app/architecture/page.tsx` | A sentence/paragraph in the relevant section summarizing the decision in product terms (NOT the ADR's internal rationale). Skip internal-only ADRs. |
| New featured repo | `lib/data/ecosystem.ts` (+ `lib/constants.ts` `REPO_URLS`) | An `EcosystemRepo` entry: name, repo, one-sentence description, `category`, optional `status`. |
| New extension | `app/extensions/page.tsx` | An `Extension` entry: `name`, human `title`, 1–2 sentence `description` derived from the header comment, `status`, `group`. |
| New device | `lib/data/devices.ts` | A `Device` entry — only when the user supplies the hardware (not auto-discoverable). |
| Closed milestone | `lib/data/roadmap.ts` | Move the matching item to the `done` phase, or add a `done` entry; trim the corresponding `now`/`next` item. |

Rules:
- **Never** edit `lib/data/generated/*.json` or anything under `public/demos|engines/`.
- If the generator wrote changes to `generated/*`/`public/` in PHASE 1, `git
  checkout -- lib/data/generated public` before committing so the PR is
  prose-only (the mechanical workflow owns those files).
- **Update the baseline.** Append every ADR you triaged this run — both the ones
  you surfaced AND the ones you deliberately skipped — to
  `lib/data/editorial-baseline.json`'s `reviewedAdrs`, so they don't re-flag next
  run. (Repos/extensions need no baseline — their grep checks self-correct.)
- One coherent commit; conventional-commit message.

```bash
# Write every ADR you triaged this run (surfaced + skipped), one path per line,
# to /tmp/sw_triaged.txt — then fold into the reviewed baseline. (File-based, so
# no bash-4 `mapfile` dependency.)
jq --argjson new "$(jq -R . /tmp/sw_triaged.txt | jq -s .)" \
   '.reviewedAdrs = (.reviewedAdrs + $new | unique)' \
   lib/data/editorial-baseline.json > /tmp/base.json && mv /tmp/base.json lib/data/editorial-baseline.json

git checkout -- lib/data/generated public 2>/dev/null || true   # keep PR prose-only
git checkout -b chore/editorial-sync
git add -A
git commit -m "docs(site): editorial sync — <short summary of what landed>"
```

### Validate before pushing
```bash
npm ci --silent && npx tsc --noEmit && npm run lint && npm run build
```
If any fail, fix the authored TSX (usually a missing `Status` value or an unbalanced
JSX tag) until green. Do NOT push a red build.

---

## PHASE 4: OPEN THE PR  (skip if --dry-run)

```bash
git push -u origin chore/editorial-sync
gh pr create --repo DisplayXR/displayxr-website --base main \
  --head chore/editorial-sync \
  --title "docs(site): editorial sync" \
  --body "$(cat <<'EOF'
Editorial (class-B) follow-up to the auto-synced mechanical data — authored by
`/sync-website` from new org signals. Mechanical facts (versions, demo cards,
repo list, extension names) are handled separately by `sync-org.yml`; this PR is
prose only.

## What changed
<bullet list: which ADR/repo/extension/milestone drove each edit, with links>

## Detected but intentionally skipped
<internal-only ADRs, test-fork repos, etc. — so the reviewer sees the judgment calls>

🤖 Authored by /sync-website (displayxr-runtime hub). Review the prose and merge to publish.
EOF
)"
```

The website auto-deploys on merge (Vercel). Editorial PRs are **not** auto-merged
— a human reviews the copy.

---

## PHASE 5: REPORT

```
/sync-website  [DRY RUN | PR #NNN]

Scanned: ADRs · repos · extensions · demos · milestones
Gaps found:
  • architecture — ADR-021 (displayxr-runtime) → summarized in §Compositors
  • ecosystem    — displayxr-foo → added card (category: tools)
  • extensions   — XR_EXT_bar → added card (group: display)
Skipped (judgment):
  • ADR-019 — internal vendor-isolation rule, not user-facing
  • displayxr-unity-test-2d-ui — test fork, not featured
Result: PR https://github.com/DisplayXR/displayxr-website/pull/NNN  (or "no gaps — site current")
```

Clean up the temp clone (`rm -rf "$WORK"`) when done.

---

## Notes

- **Cadence.** Run after a milestone closes or a batch of releases lands — not on
  every patch. The mechanical layer already keeps versions/demos live; prose
  drifts on a slower clock.
- **Detection source of truth.** `_meta.json.signals` is the generator's
  deterministic snapshot (no timestamp). Repos/extensions/demos self-correct
  against the authored TSX (grep), so they need no state; ADRs converge against
  this skill's own `lib/data/editorial-baseline.json` reviewed-set. The skill
  re-runs the generator first only as a safety net to freshen the snapshot.
- **Don't over-add.** Most internal ADRs and many repos should NOT appear on a
  marketing site. The skill's value is in the *judgment* of what to surface; when
  in doubt, list it under "Skipped" and let the user decide.
