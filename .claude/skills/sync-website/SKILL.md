---
name: sync-website
description: Editorial sync pass for the displayxr-website — driven from the displayxr-runtime hub. The mechanical facts (versions, demo cards, repo list, extension names) are auto-synced by the website's sync-org.yml; THIS skill handles the class-B narrative that needs judgment — surfacing new ADRs, new repos, extension-overlay gaps, component-model and Play changes, SDK-pin drift, new demos, and closed milestones, deciding which shipped releases are news-worthy and writing the homepage "What's New" feed, then authoring the matching prose into the website's hand-written data/TSX under its positioning guardrails, and opening a PR. Use when a release or milestone has landed and the site's prose has fallen behind.
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
touch the same fields: the skill edits authored data/TSX (`lib/data/*.ts` —
`roadmap`, `ecosystem`, `news`, `components`, `devices`, `compatibility`, `web`,
`browser`, `native`, `play`; `lib/constants.ts`; `app/architecture/page.tsx`,
`app/extensions/page.tsx`, `app/contribute/page.tsx`) and **never**
`lib/data/generated/*.json` or `public/{demos,engines}/`.

### Site IA (hub-led, since the 2026-10 overhaul) — verify against `NAV` first
The website's own `CLAUDE.md` IA section has lagged the code before; **`NAV` in
`lib/constants.ts` and the redirects in `next.config.ts` on `origin/main` are the
truth.** As of 2026-11 the header is leaves only: About · Browser · (Play, once
website PR #54 merges) · Developers · Contribute · Components, plus Download and
Docs ↗. Routes that matter for editorial sync:

| Route | Authored source | Notes |
|---|---|---|
| `/components` | `lib/data/components.ts` | ADR-054 classes + implementers. `/vendors`, `/display-vendors/*` 308 here. |
| `/browser` | `lib/data/browser.ts` + `lib/data/web.ts` | Browser product page + SDK; absorbed `/web`, `/webxr`. |
| `/developers`, `/developers/native` | `lib/data/web.ts`, `lib/data/native.ts` | Native quickstart replaced `/getting-started` (308). |
| `/platform-support` | `lib/data/devices.ts`, `lib/data/compatibility.ts` (incl. `desktopApps`) | Merged page; `/status`, `/compatibility` 308 here. Edit data, never the page. |
| `/contribute` | `ecosystemRepos` (`lib/data/ecosystem.ts`) + hand-curated `adrs` array in the page | A repo added to `ecosystem.ts` shows on `/about`'s `EcosystemMap` *and* here. |
| `/extensions` | `editorial` overlay in `app/extensions/page.tsx` | List itself is generated. |
| `/news`, `/news/rss.xml`, homepage | `lib/data/news.ts` | Hero ticker (`getBannerNews()`, ≤4) + `ProofSection` (first 3). |
| `/play` (PR #54) | `lib/data/play.ts` + generated `play.json` | Android lane version/assets are generated from `displayxr-play-android` releases. |
| `/apps` (planned) | not on `main` yet | Hub for Gallery, Ride, Chat, Media Player Pro. If `app/apps/` exists, read its data file before writing app news; until then app stories link to the app's own URL. |

A new route also needs an entry in `app/sitemap.ts`; never revive a redirected route.

### Positioning guardrails (apply to every string you write)
These come from the website `CLAUDE.md` plus the 2026-11 strategy session. A
string that breaks one is a bug, not a style choice:

- **"Spatial display(s)"** is the canonical term. "Glasses-free" only where it is
  literally true of a specific product, never as DisplayXR's scope.
- DisplayXR **extends** OpenXR — never "replaces"/"alternative to", and never
  "conformant" or "certified" (it *runs* the Khronos conformance suite).
- DisplayXR was **"founded and incubated at Leia"** — never "spin-off", never
  "foundation". Leia mentions stay off the homepage's own prose.
- **No OEM, customer or partner names** in any pipeline/roadmap/"coming" context
  (a vendor may be named only when its *shipped* onboarding is the news). The
  **Media Player Pro customer is never named**. Never "Immersity". Show is
  excluded from the site.
- **Private-source products (Shell, Browser, Chat) are never called open source**
  (and need not be called closed). Their release repos stay `releasesOnly`.
- **Play**: no game title is named until a public release carries it (the PC
  titles are *described* until their release); no company or OEM names on `/play`.
- Browser/SDK claims follow the guardrails in `lib/data/web.ts` (preview-tier
  modules are labelled preview; platform "soon" cells stay "soon").
- **US spelling** ("color", "organization", "-ize").

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
              ecosystem | extensions | components | sdk | devices | play |
              news. Omit to scan all.
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
Read them from a temp clone. Read the clone's `CLAUDE.md` too, but where it
disagrees with the code (`NAV`, `next.config.ts`), the code wins — say so in
the report.

```bash
WORK=$(mktemp -d)
gh repo clone DisplayXR/displayxr-website "$WORK/web" -- --quiet
cd "$WORK/web"

# Belt-and-braces: make sure the snapshot is current before diffing prose
# against it (in case a release landed since the last cron and no dispatch
# fired). Needs Node (dependency-free) and a token (GITHUB_TOKEN, else
# `gh auth token`). This may write to
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
Filter out repos that intentionally aren't featured: `.github`,
`displayxr-website` itself, third-party forks (`3DGameBridgeProjects`,
`glTF-Sample-Assets`), test-only forks, archived mirrors. Repos that *are*
candidates as of 2026-11: `displayxr-tracking-open` (viewer-tracking provider,
`tools`), `displayxr-slicer` / `displayxr-paraview` (preview app integrations —
also candidates for `compatibility.ts` `desktopApps`), `displayxr-play-android`
(Play's Android lane — `/play` already links it; an ecosystem card is optional).
A new repo also needs a `REPO_URLS` key in `lib/constants.ts` when a page links
it. Use the repo description:
```bash
gh repo view "DisplayXR/$r" --json description,repositoryTopics \
  --jq '{desc:.description, topics:[.repositoryTopics[]?.name]}'
```

### 2.3 Extensions overlay drift → `/extensions`
Every generated extension renders automatically; the gap is a missing (or
stale) key in the page's `editorial` overlay. Diff the **keys**, not a loose
grep (a name mentioned in a comment would hide a gap):
```bash
grep -oE '^  "XR_DXR_[a-z0-9_]+": \{' app/extensions/page.tsx \
  | grep -oE 'XR_DXR_[a-z0-9_]+' | sort > /tmp/sw_ext_overlay.txt
jq -r '.[].name' lib/data/generated/extensions.json | sort > /tmp/sw_ext_gen.txt
comm -13 /tmp/sw_ext_overlay.txt /tmp/sw_ext_gen.txt | sed 's/^/MISSING-EXT /'
comm -23 /tmp/sw_ext_overlay.txt /tmp/sw_ext_gen.txt | sed 's/^/STALE-EXT /'   # renamed/removed
```
(`next build` prints the same drift as `[extensions] …` warnings.) As of
2026-11 five had no overlay: `app_share`, `desktop_control`, `input_providers`,
`stereo_comfort`, `swapchain_color_encoding`.
For each, read the runtime's catalog note and spec before writing the overlay:
```bash
gh api repos/DisplayXR/displayxr-runtime/contents/docs/specs/extensions/index.json \
  --jq '.content' | base64 -d | jq --arg e "$ext" '.extensions[$e]'   # {group,title,summary}
gh api "repos/DisplayXR/displayxr-runtime/contents/docs/specs/extensions/$ext.md" \
  --jq '.content' | base64 -d | sed -n '1,60p'
gh api "repos/DisplayXR/displayxr-extensions/contents/include/openxr/$ext.h" \
  --jq '.content' | base64 -d | sed -n '1,40p'   # fallback if no spec
```
If the catalog summary says "not implemented yet" / "designed", the overlay's
`status` must say so too — never describe a header-only extension as shipping.

### 2.3b SDK pin + module list → `/developers`, `/browser`
`INLINE3D_VERSION` in `lib/constants.ts` is the version every snippet installs
(`npm install @displayxr/inline3d@…`, the jsDelivr `call.js` tag in
`lib/data/browser.ts`). It is hand-bumped and drifts (2026-11: `1.30.0` vs npm
`1.51.1`). Compare it, and `SDK_MODULES` in `lib/data/web.ts`, against npm:
```bash
PIN=$(grep -oE 'INLINE3D_VERSION = "[^"]+"' lib/constants.ts | cut -d'"' -f2)
NPM=$(npm view @displayxr/inline3d version)
[ "$PIN" = "$NPM" ] || echo "SDK-PIN-DRIFT site=$PIN npm=$NPM"
npm view @displayxr/inline3d exports --json | jq -r 'keys[]' \
  | grep -vE '^\.$|package\.json' | sed 's#^\.##' | while read -r m; do
    grep -q "\"$m\"" lib/data/web.ts || echo "SDK-MODULE-UNLISTED $m"
  done
```
Bump `INLINE3D_VERSION` only after checking the SDK changelog that `HELLO_WORLD`
and the `browser.ts` `CODE_TABS` still run unchanged on the new version. A new
subpath (e.g. `/hands`, `/depth`) gets an `SDK_MODULES` row with `preview: true`
unless the SDK's stability doc says it is frozen; sub-entries like
`/call/full` or `/model/three` usually fold into their parent's row.
`package.json` also pins `@displayxr/inline3d` because `HeroSplat` imports it at
runtime — **that is a code change, not prose**: report it, leave it out of the
editorial PR.

### 2.3c Component model → `/components`
```bash
grep -nE "shippedIn|status:" lib/data/components.ts
```
Compare each class against the runtime: a new ADR-054 class, a core landing on
`main` (`core-merged`), its first tagged release (set `shippedIn`), or a new
implementer repo (e.g. `displayxr-tracking-open`, `displayxr-conversion-open`).
Status values are `shipped | core-merged | designed | planned`.

### 2.3d Play → `/play` (once `app/play/` exists on `main`)
The Android lane's version, companion APK and per-game mod zips are generated
(`generated/play.json` from `displayxr-play-android` releases, prereleases
included). The editorial gaps are in `lib/data/play.ts`: a system moving
`preview` → `available`, a new game whose release now carries it (only then may
it be **named**), or the Windows/Steam lanes getting a real download. Never name
a PC title before its public release, and never name a company/OEM on `/play`.

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

`lib/data/news.ts` (authored) feeds the homepage Hero ticker (`getBannerNews()`:
banner tier, ≤90 days, ≤4 items), the homepage `ProofSection` (first 3 of the
same), `/news` and `/news/rss.xml` (all items, chronological). The
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

**Tag shapes that are not `v*`.** Candidate ids are `<repo>@<tag>`; read the tag
before the title:

| Tag | What it is | Default |
|---|---|---|
| `displayxr-installer@android-bundle-latest` | moving alias of the newest dated bundle | **always skip** |
| `displayxr-installer@android-bundle-YYYY-MM-DD` | dated Android tablet bundle (re-pins runtime/plug-in/CNSDK) | skip, unless it is the first bundle to carry a new capability — then the story is the capability |
| `displayxr-demo-mediaplayer@pro-v*` | **Media Player Pro**, a separate product line from the base player's `v*` tags | skip increments; a first-of-kind Pro capability is `list` tier. Never name the customer or the production. |
| `displayxr-web@wordpress-v*` | the "DisplayXR 3D Photos" WordPress plug-in | its first release is a distribution milestone (`list`); later ones skip |
| `displayxr-leia-plugin@sr-sdk-*`, `displayxr-models@blobs` | asset/storage tags, not releases | always skip |
| `displayxr-play-android@v*` (prerelease) | Play's Android lane; title names the game | a new **game** or a lane leaving preview can be news; naming the game is fine because the release carries it |
| first `v0.1.0` of a new repo (`displayxr-slicer`, `displayxr-paraview`, `displayxr-tracking-open`, …) | a new integration or provider | usually `list` (or `banner` if it opens a new class, e.g. an open camera-tracking provider); say "preview" when the release is one |

Judgment notes that keep the feed credible:

- **Apply the positioning guardrails** (top of this file) to every headline and
  blurb — especially: no OEM/customer/partner name in anything forward-looking,
  never "conformant"/"certified", never call Shell/Browser/Chat open source.
- **Apps (Gallery, Ride, Chat, Media Player Pro) ship from private repos or
  hosted sites**, so they rarely appear in `news-candidates.json` at all. When
  the user tells you one shipped, it is a `product` item; link the app's public
  URL (or the `/apps` hub once it exists), never a private repo.

- **One story, one entry.** A release that lands three separate first-of-kind
  things gets three entries (each with its own `id`, all pointing at that
  release); a story that dribbles across four releases gets **one** entry, dated
  when it became usable. Never post the same story twice under new versions.
- **Vendor names are allowed when the vendor is the news** — a new display or
  input vendor onboarding, a new tracking source. A vendor's own version bump is
  not news. The homepage's *own prose* stays vendor-neutral (website CLAUDE.md);
  a factual news item naming a vendor does not violate that.
- **Prereleases and preview builds can be news**, but say so in the blurb —
  "preview", "ahead of GA" — rather than implying GA. Note the browser is **no
  longer** an example of this: browser-pvt#120 retired its Developer-Preview
  channel at v1.0.0, so a browser release is a normal release and must not be
  blurbed as a preview.
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
| New featured repo | `lib/data/ecosystem.ts` (+ `lib/constants.ts` `REPO_URLS`) | An `EcosystemRepo` entry: name, repo, one-sentence description, `category`, optional `status` (`shipping` once it has a public release). Surfaces automatically on `/about`'s `EcosystemMap` **and** `/contribute`'s repo map — no page edit needed. Set `releasesOnly: true` on a repo that only publishes installers and takes issues while its source is private (today: `displayxr-shell-releases`, `displayxr-browser`); that keeps it off `/contribute`'s "Where the code lives" map. Never describe such a product as open source (nor as closed — make no claim). |
| Missing/stale extension overlay (§2.3) | `app/extensions/page.tsx` (+ `lib/data/openxr-gaps.ts` for grouping) | An entry in the `editorial` overlay keyed by the generated name: required `status` + `tier` (`'essential'` \| `'advanced'`), optional `title`/`href`, and the three sentences the card renders — `vanillaGap` ("Vanilla OpenXR assumes …"), `adds` ("This adds …"), `needIf` ("You need it if …"). Write from the spec, not the name; when the spec is ambiguous, write less. To group it under one of the four gaps, add it to that gap's `extensions` in `openxr-gaps.ts`, else it lands in "Advanced / other". Rename a `STALE-EXT` key rather than deleting its copy. |
| SDK drift (§2.3b) | `lib/constants.ts` `INLINE3D_VERSION`, `lib/data/web.ts` `SDK_MODULES` | Bump the pin (after the snippet check); add a row per new subpath, `preview: true` unless frozen. Never touch `package.json` in this PR. |
| New component class or implementer (§2.3c, ADR-054) | `lib/data/components.ts` | The class cards on `/components` are data: one entry per class (`status` = `shipped` \| `core-merged` \| `designed` \| `planned`; `shippedIn` = the first runtime tag carrying the core, compared against the generated runtime version so the chip flips itself; `impls` = implementers with an honest chip, e.g. `build-verified` for one never run on hardware). Never edit the page TSX. |
| New desktop-app integration | `lib/data/compatibility.ts` `desktopApps` | Rendered on `/platform-support`. Verification is data (`seated-verified` \| `sim-verified` \| `proposed`) — never upgrade it in prose. |
| Play lane change (§2.3d) | `lib/data/play.ts` | Status, steps, note. Generated fields (versions, asset URLs) are not yours. |
| New platform / API / Android capability | `lib/data/native.ts` (quickstart), `lib/data/browser.ts` `MATRIX` (browser per-OS cells), `lib/data/compatibility.ts` | Flip a `soon` cell only when a release ships it; Android claims cite the released APK/bundle, not a dev build. |
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

Scanned: ADRs · repos · extensions · SDK pin · components · play · demos · milestones · releases
Gaps found:
  • architecture — ADR-021 (displayxr-runtime) → summarized in §Compositors
  • ecosystem    — displayxr-foo → added card (category: tools)
  • extensions   — XR_DXR_stereo_comfort → overlay added (Advanced / other, tier: advanced)
  • sdk          — INLINE3D_VERSION 1.30.0 → 1.51.1; +/hands, /depth rows (preview)
  • news         — runtime@v2.3.0 → 2 banner items (input providers, CTS in CI)
                   browser@v1.0.0 → 1 banner item
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
