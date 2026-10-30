# ADR-0015: Review viewer for ripped assets and lighting previews
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The review UI (ADR-0006) needs an interactive 3D view of ripped models to check
texture-set swaps, PBR bakes and light placement without booting the game. This is a
Stage 2 design item, scoped for after the Stage 1 spikes settle, not a Stage 1 spike
itself.

## Options (with cost/risk)
**Engine-in-the-loop preview (boot the real renderer per asset).** Cost: couples every
review action to the build/game locks and a full game boot. Risk: high —
review throughput drops to game-launch speed.
**three.js viewer over the ripped glTF (chosen).** Cost: needs a merc-to-glTF export path
that does not exist yet; must be verified during lightbench before this is buildable.
Risk: low once export is confirmed — three.js and glTF are already load-bearing
(ADR-0010).

## Decision
Build a three.js viewer into the review UI: orbit/zoom camera, texture-set switching, a
movable point light for quick checks, and a fitted sun light added later once the unbake
fit is available. Two preview modes: an engine-exact mode using lightbench turntable frame
strips for ground truth, and a PBR-preview mode through three.js's standard material for
fast iteration on baked textures. Requires a merc-to-glTF export path; confirm feasibility
during lightbench before committing viewer work.

## Consequences
- Blocked on lightbench confirming merc-to-glTF export; if that path is not viable, this ADR is
  revisited before Stage 2 implementation.
- Engine-exact mode still depends on lightbench producing turntable frame strips;
  the viewer does not replace lightbench, it consumes its output.
- This is a Stage 2 design item — no build work starts before the Stage 2 design review.
