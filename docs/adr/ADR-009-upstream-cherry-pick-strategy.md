---
status: Accepted
date: 2026-03-10
source: "#47"
---
# ADR-009: Upstream Cherry-Pick Strategy

## Context
This repo forked from Monado. Upstream continues active development. Need a strategy for incorporating upstream improvements without destabilizing the lightweight runtime.

## Decision
Tier-based cherry-pick approach. Tier 1 (always take): bug fixes, build fixes, CI improvements. Tier 2 (evaluate): API compliance, utility improvements. Tier 3 (skip): large refactors, new VR drivers, compositor architecture changes. Skip anything that conflicts with native compositor architecture.

## Consequences
Selective upstream sync. Avoids large merge conflicts. May diverge from upstream over time. Reference branch `merge/upstream-sync-with-ci-build` tracks full upstream state.
