---
name: sync-website
description: Editorial sync pass for the displayxr-website — driven from the displayxr-runtime hub. The mechanical facts (versions, demo cards, repo list, extension names) are auto-synced by the website's sync-org.yml; THIS skill handles the class-B narrative that needs judgment — surfacing new ADRs, new repos, new extensions, new demos, and closed milestones, deciding which shipped releases are news-worthy and writing the homepage "What's New" feed, then authoring the matching roadmap / architecture / ecosystem prose into the website's hand-written TSX and opening a PR. Use when a release or milestone has landed and the site's prose has fallen behind.
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
  of themselves. Do not duplicate that here. It also writes
  `generated/news-candidates.json` — the raw "a release happened" list that
  feeds §2.6 below — but it never decides what is news.

- **Editorial layer (THIS skill).** The narrative that needs a human's judgment:
  roadmap phrasing, architecture prose, ecosystem blurbs, extension
  titles/descriptions, device copy, **and the homepage "What's New" feed**. The
  generator can *detect* that these inputs changed (new ADR, new repo, new
  extension header, new demo, closed milestone, new release) but must never
  auto-write them. This skill reads the new sources and authors tasteful prose
  into the **hand-written** TSX, then opens a **PR** — because prose warrants a
  glance, unlike the mechanical facts.

**Mechanical commit = facts (direct to main). Skill PR = narrative.** They never
touch the same fields: the skill edits authored TSX (`lib/data/roadmap.ts`,
`lib/data/ecosystem.ts`, `lib/data/news.ts`, `lib/constants.ts`,
`app/architecture/page.tsx`, `app/extensions/page.tsx`, `lib/data/devices.ts`,
`app/contribute/page.tsx`) and **never** the `lib/data/generated/*.json` files.

### Site IA note (persona-led, since the 2026-06 overhaul)
The site is organized around three audiences. Two facts matter for editorial
sync: (1) the **repo map and a headline-ADR list live on `app/contribute/page.tsx`**
— the repo map renders `ecosystemRepos` from `lib/data/ecosystem.ts`, so adding a
repo there surfaces it on both the homepage `EcosystemMap` *and* `/contribute`
automatically; the ADR list is a short hand-curated array. (2) The device /
compatibility tables and the version dashboard now render on **one merged
`/platform-support` page** (old `/compatibility` + `/status` 308-redirect there) —
but you still edit the **data** in `lib/data/devices.ts` / `lib/data/compatibility.ts`,
never the page, so this doesn't change the authoring targets.

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
              ecosystem | extensions | devices | news. Omit to scan all.
```

Examples:
```
/sync-website                  # full scan → author prose → open PR
/sync-website --dry-run        # just tell me what's drifted
/sync-website architecture     # only reconcile new ADRs into /architecture
/sync-website news             # only triage new releases into the What's New feed
```

`news` is the highest-cadence focus — run it after any release batch, even when
nothing else has drifted.

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
[ -f "$BASE" ] || echo '{"reviewedAdrs":[],"reviewedNewsCandidates":[]}' > "$BASE"
jq -r '.reviewedAdrs[]'            "$BASE" 2>/dev/null | sort > /tmp/sw_adr_base.txt
jq -r '.reviewedNewsCandidates[]?' "$BASE" 2>/dev/null | sort > /tmp/sw_news_base.txt
```

First run: the baseline is empty, so the whole ADR backlog is "new" — that's the
intended one-time reconciliation. After PHASE 3 you write every triaged ADR back
into the baseline (whether you surfaced it or skipped it), so subsequent runs see
only the delta.

**Release candidates converge the same way** — for the same reason. Most releases
are *not* news, so "is this release in `news.ts`" is false for the overwhelming
majority and would re-flag the whole history every run. `reviewedNewsCandidates`
records every `<repo>@<tag>` you triaged, surfaced or skipped.

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

### 2.6 New releases → the "What's New" feed

The homepage ticker and `/news` both render `lib/data/news.ts` (authored). The
generator writes every recent release to `generated/news-candidates.json` with
the bullet lines it found under a *Highlights / Features / What's New / Added*
heading — so a candidate carrying `featureLines` is one that *claims* new
capability. `looksMechanical: true` means the notes had no feature section at
all, which is the common case.

```bash
CAND=lib/data/generated/news-candidates.json
# Untriaged candidates, richest first. looksMechanical ones are listed after,
# so you can skim them — occasionally a real feature ships with lazy notes.
jq -r --slurpfile b <(jq '[.reviewedNewsCandidates[]?]' lib/data/editorial-baseline.json) '
  [.[] | select(.id as $i | ($b[0] | index($i)) | not)]
  | sort_by(.looksMechanical, (.date | explode | map(-.)))
  | .[] | "\(if .looksMechanical then "·" else "★" end) \(.id)  \(.date)\n    \(.title)\n\(.featureLines | map("      - " + .) | join("\n"))"
' "$CAND"

# What the feed already says, so you never double-post a story.
grep -nE '^\s+(id|date|headline|href):' lib/data/news.ts
```

**The news-worthiness rubric.** Apply it to every untriaged candidate. The test:
*can you state it as a capability that did not exist last month, in ten words or
fewer, without a version number?* If the sentence collapses to "we shipped a
build", it is not news.

| Verdict | What qualifies |
|---|---|
| `tier: "banner"` | **First-of-kind.** A new OS or platform · a new graphics API · a new engine · a new vendor (display *or* input) · a new product surface (browser, gallery, SDK) · a new input class · a standards/Khronos milestone · a headline capability users can name. |
| `tier: "list"` | Real but incremental — a named feature inside an existing surface · a new demo or sample · a distribution milestone that reaches a new audience (a package manager, a new installer target) · a broad compatibility unlock. |
| **skip** | Version bumps with no named feature · bundle/meta-installer releases · ABI bumps · patch releases · CI, refactors, docs, test infrastructure · bug fixes (unless the fix *is* the unlock, e.g. "every graphics API now works on iGPU laptops"). |

Judgment notes that keep the feed credible:

- **One story, one entry.** A release that lands three separate first-of-kind
  things gets three entries (each with its own `id`, all pointing at that
  release); a story that dribbles across four releases gets **one** entry, dated
  when it became usable. Never post the same story twice under new versions.
- **Vendor names are allowed when the vendor is the news** — a new display or
  input vendor onboarding, a new tracking source. A vendor's own version bump is
  not news. The homepage's *own prose* stays vendor-neutral (website CLAUDE.md);
  a factual news item naming a vendor does not violate that.
- **Prereleases and preview builds can be news** (the browser preview is), but
  say so in the blurb — "preview", "ahead of GA" — rather than implying GA.
- **Don't date it "today".** `date` is when it became true for a user: the
  release's `date` field, or the upstream merge date for a standards item.
- **When the pool is thin, resist promoting filler to `banner`.** The ticker
  renders nothing when nothing qualifies, and that is the correct outcome.

### 2.7 Build the gap report
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
| User-facing new ADR | `app/architecture/page.tsx` (product-level) and/or the headline-ADR array in `app/contribute/page.tsx` (contributor-facing) | A sentence/paragraph summarizing the decision in product terms (NOT the ADR's internal rationale). Most ADRs are internal — skip them; a genuinely notable one belongs on /architecture, a contributor-relevant one in the /contribute headline list. |
| New featured repo | `lib/data/ecosystem.ts` (+ `lib/constants.ts` `REPO_URLS`) | An `EcosystemRepo` entry: name, repo, one-sentence description, `category`, optional `status`. Surfaces automatically on the homepage `EcosystemMap` **and** `/contribute`'s repo map — no page edit needed. |
| New extension | `app/extensions/page.tsx` | An `Extension` entry: `name`, human `title`, 1–2 sentence `description` derived from the header comment, `status`, `group`. |
| New device | `lib/data/devices.ts` | A `Device` entry — only when the user supplies the hardware (not auto-discoverable). |
| Closed milestone | `lib/data/roadmap.ts` | Move the matching item to the `done` phase, or add a `done` entry; trim the corresponding `now`/`next` item. |
| News-worthy release (§2.6) | `lib/data/news.ts` | A `NewsItem` **prepended** to `NEWS` (the array is newest-first). See the writing rules below. |

**Writing a `NewsItem`.** The file's header comment is the contract; the craft is
in the two strings:

- `headline` — **≤ 60 characters**, benefit-first, no version number, no repo
  name, sentence case. It is read in a pill on the hero, so it must survive
  being the only thing someone reads. Write the *outcome*, not the mechanism:
  "Eye-position latency down to one display refresh", not "Late-weave
  presentation pacing on every present path". "Motion controllers, from any
  tracking source", not "Input-provider plug-in channel (ADR-034)".
- `blurb` — one sentence, `/news` only. This is where the mechanism, the numbers,
  and the caveat go. Concrete beats grand: cite the measured figure, name the
  APIs, say "preview" if it is one.
- `id` — a stable slug describing the *story*, not the release
  (`input-provider-plugins`, not `runtime-v2-3-0`). Never recycle one; it is the
  `/news` anchor.
- `href` — the most durable destination: a spec or ADR for a capability, the
  release for a feature set, the upstream PR for a standards item.
- Do **not** set `pinnedUntil` unless the user asks — expiry is meant to be
  automatic.
- **`priority` is for landmarks, and almost never yours to set.** The ticker
  sorts by `priority` desc then date, so `priority: 1` lets an item outrank
  fresher news. Reserve it for something whose importance outlives the week it
  shipped in — a standards-body registration, a first vendor, a license change.
  The bar: *would this still belong on the homepage a month from now, when four
  newer things have shipped?* If the honest answer is "no", leave it unset.
  Default to unset and say in the PR body that you considered it; a field every
  item claims is a field that ranks nothing. It reorders the **ticker only** —
  it does not extend the freshness window (`pinnedUntil` does that) and does not
  touch `/news`, which stays strictly chronological.

Rules:
- **Never** edit `lib/data/generated/*.json` or anything under `public/demos|engines/`.
- If the generator wrote changes to `generated/*`/`public/` in PHASE 1, `git
  checkout -- lib/data/generated public` before committing so the PR is
  prose-only (the mechanical workflow owns those files).
- **Update the baseline.** Append every ADR *and* every release candidate you
  triaged this run — both the ones you surfaced AND the ones you deliberately
  skipped — to `lib/data/editorial-baseline.json`'s `reviewedAdrs` /
  `reviewedNewsCandidates`, so they don't re-flag next run. (Repos/extensions
  need no baseline — their grep checks self-correct.)
- One coherent commit; conventional-commit message.

```bash
# Write every ADR you triaged this run (surfaced + skipped), one path per line,
# to /tmp/sw_triaged.txt, and every release candidate id (<repo>@<tag>) to
# /tmp/sw_news_triaged.txt — then fold both into the reviewed baseline.
# (File-based, so no bash-4 `mapfile` dependency.)
touch /tmp/sw_triaged.txt /tmp/sw_news_triaged.txt
jq --argjson adrs "$(jq -R . /tmp/sw_triaged.txt      | jq -s 'map(select(. != ""))')" \
   --argjson news "$(jq -R . /tmp/sw_news_triaged.txt | jq -s 'map(select(. != ""))')" \
   '.reviewedAdrs            = ((.reviewedAdrs // [])            + $adrs | unique)
  | .reviewedNewsCandidates  = ((.reviewedNewsCandidates // [])  + $news | unique)' \
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

Scanned: ADRs · repos · extensions · demos · milestones · releases
Gaps found:
  • architecture — ADR-021 (displayxr-runtime) → summarized in §Compositors
  • ecosystem    — displayxr-foo → added card (category: tools)
  • extensions   — XR_EXT_bar → added card (group: display)
  • news         — runtime@v2.3.0 → 2 banner items (input providers, CTS in CI)
                   browser@preview-0.1.7 → 1 banner item
Skipped (judgment):
  • ADR-019 — internal vendor-isolation rule, not user-facing
  • displayxr-unity-test-2d-ui — test fork, not featured
  • runtime@v2.4.1, installer@v2.0.14 (+9 more) — patch/bundle, no new capability
Result: PR https://github.com/DisplayXR/displayxr-website/pull/NNN  (or "no gaps — site current")
```

Clean up the temp clone (`rm -rf "$WORK"`) when done.

---

## Notes

- **Cadence.** Run after a milestone closes or a batch of releases lands — not on
  every patch. The mechanical layer already keeps versions/demos live; prose
  drifts on a slower clock. **`/sync-website news` is the exception** — it is
  cheap and worth running after every release batch, because the feed's value is
  its freshness. Skipping everything is a valid outcome.
- **Detection source of truth.** `_meta.json.signals` + `news-candidates.json`
  are the generator's deterministic snapshots (no timestamps). Repos, extensions
  and demos self-correct against the authored TSX (grep), so they need no state;
  ADRs and release candidates converge against this skill's own
  `lib/data/editorial-baseline.json` reviewed-sets. The skill re-runs the
  generator first only as a safety net to freshen the snapshot.
- **The feed ages itself out.** `getBannerNews()` drops banner items older than
  `BANNER_MAX_AGE_DAYS` (90) and caps the ticker at `BANNER_MAX_ITEMS` (4), so
  nothing has to be pruned by hand and an empty pool hides the ticker entirely.
  The homepage is statically generated, so the *rendered* pool only refreshes on
  a rebuild — which the mechanical sync's daily direct-commit to `main` already
  triggers.
- **Don't over-add.** Most internal ADRs and many repos should NOT appear on a
  marketing site. The skill's value is in the *judgment* of what to surface; when
  in doubt, list it under "Skipped" and let the user decide.
