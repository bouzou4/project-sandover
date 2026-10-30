# ADR-0016: Texture baseline for PBR and upscale comparison
Status: accepted    Date: 2026-09-23 project lead

## Context
The texture census and the pack inventory (`kb/findings/2026-09-23-esrgan-pack-inventory.md`)
found an existing community 4x-upscaled texture pack already installed as a drop-in
replacement set (86.4% vanilla coverage). PBR and upscaler work needs a fixed comparison
point rather than re-litigating "better than vanilla" every time a new model is tried.
The pack keeps the PS2 alpha-opaque-at-128 convention only on average, with per-pixel
noise that the engine's replacement path does not clamp.

## Options (with cost/risk)
**No baseline; compare every candidate to raw vanilla.** Cost: cheap short-term. Risk:
re-decides a question already settled community-side, and undersells real upscaler
progress since the pack is already a meaningful step past vanilla.
**Adopt the pack as-is, alpha unfixed.** Cost: none up front. Risk: the pack's alpha
noise (range ~113-129, up to ~146 on textures with real alpha) bakes visible artifacts
into every PBR pass built on it, since replacement is unclamped at extract time.
**Adopt the pack as the baseline, alpha normalized before baking (chosen).** Cost: one
normalization pass to snap near-128 alpha back to the PS2 convention before any PBR bake
consumes it. Risk: low; scoped to alpha channel only, no other pixel data touched.

## Decision
The existing 4x-upscaled community pack (`kb/facts/assets.md`,
`kb/findings/2026-09-23-esrgan-pack-inventory.md`) is the private-use baseline for PBR and
upscale work. Its alpha is normalized to the PS2 128-opaque convention before baking.
Newer upscalers are judged against this baseline only through the pick-one-of-five
loop on style replication, never by metric alone; nothing ships unless a human judges it
better than the baseline on contact sheets. The pack's license is unknown (no readme,
credits, or metadata in the archive), so it is never redistributed.

## Consequences
- Any pipeline reading pack textures must run the alpha-normalize step first; raw pack
  files are not bake-ready.
- The pack stays local-only; Stage 2 design and any public repo output must not ship or
  link its files.
