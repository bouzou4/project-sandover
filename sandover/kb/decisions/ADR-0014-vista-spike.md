# ADR-0014: Vista (far-level background) as an early spike, late implementation
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
Distant levels are visible from several diorama viewpoints but are not drawn once their
own level slot swaps out. Whether that is feasible at all, and how it interacts with
level streaming, needs answering before Stage 2 design can commit to it, but it does not
sit on the lighting critical path (ADR-0013) and stereo is more urgent.

## Options (with cost/risk)
**Skip vista entirely.** Cost: none. Risk: forecloses a visually significant feature with
no data on whether it is even cheap to try.
**Full implementation now, ahead of stereo.** Cost: competes with stereo for the build lock
and for design attention before the facts are known. Risk: highest — could be built on
wrong assumptions about coordinate consistency across levels.
**Early spike, late implementation.** Cost: one exploration slot now for
facts only; the render hack itself waits until after stereo. Risk: low — defers the
expensive part until the facts justify it.

## Decision
Run the vista spike now as a facts-only spike (issue "Vista spike"): coordinate consistency
between level slots, distance-based tricks in the existing renderer, memory/draw-call
budgets, and engine seams that a far pass would need to hook. Implementation is phase 2,
scheduled after stereo lands. Scope for phase 2: keep every level's static background
resident, draw far levels' tfrag/tie/shrub in true world positions via a far-frustum pass
over per-level draw lists, no actors or GOAL processes running for far levels, and fog
becomes atmospheric falloff instead of the per-level fog volumes. Not on the lighting
critical path; it neither depends on nor blocks ADR-0013's pre-pass.

## Consequences
- The vista facts feed the Stage 2 design as a scoped, budgeted feature, not a guess.
- Phase 2 is not scheduled until after stereo and its build/game locks are clear.
