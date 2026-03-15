---
status: Accepted
date: 2026-03-10
source: "#46"
---
# ADR-008: Display as Spatial Entity

## Context
Multi-display and spatial shell scenarios require knowing where displays are in physical space. The runtime needs a spatial model for displays.

## Decision
Model displays as spatial entities in the `xrt_space_overseer` DAG. Each display has a pose in the spatial graph, enabling multi-display rendering and spatial relationships between displays.

## Consequences
Foundation for multi-display (#69, #70) and spatial shell (#43, #44). Display poses can be calibrated, tracked, or configured.
