---
title: Inventory of the existing ESRGAN-upscaled Jak 1 texture pack
date: 2026-09-23
workstream: assets
status: verified
sha: 5b7218d5f
---

## Answer (<=5 lines)
The pack ("Jak1 ESRGAN Edition v1.0.1") is laid out exactly as
`texture_replacements/<tpage>/<tex>.png`, a direct drop-in for the engine's bake-time
replacement mechanism. It covers 3456/4002 vanilla textures (86.4%) across 107/126
tpages (49 fully covered, 58 partially, 19 entirely missing - mostly the small
`*-vis-alpha` translucency tpages). Every sampled texture (47, stratified across 31
tpages) upscales at exactly 4.00x with no variance. The pack keeps the PS2
alpha-opaque-at-128 convention on average but with per-pixel noise (observed range
~113-129 on vanilla-uniform-128 textures, occasionally exceeding 128 up to ~146 on
textures with real alpha content) that the engine's replacement path does not clamp.
No readme, license, credits, or author metadata exists anywhere in the archive.

## Evidence (path:line@sha / URL, short excerpts)
- **Mechanism match (verified against source, not just docs):** `custom_assets/jak1/texture_replacements/<tpage>/<tex>.png`
  is read by `TextureDB::replace_texture()` (`decompiler/data/TextureDB.cpp:150-184`),
  which tries `<tpage>/<name>.png` then falls back to `_all/<name>.png`; invoked from
  `resolve_texture()`, called during the same `task rip-textures`/`task rip-levels` pass
  that writes level output, so replacement is baked in at extract time, not runtime
  (already recorded in `sandover/kb/facts/textures.md:6`). The pack's own top-level
  folder is `texture_replacements/`, containing 124 sub-folders and 3456 files, matching
  this exactly on first listing (`7z l -slt` against the archive, no extraction needed
  for this check).
- **File count / structure:** `7z l -slt "Jak1 ESRGAN Edition v1.0.1.zip"` (908 MiB
  packed, 3456 files, 124 folders) parsed into records: every file is a `.png` under
  `texture_replacements\<tpage>\<name>.png` (zero non-PNG files, zero files at other
  depths, zero use of the `_all` global-fallback folder). No archive comment, no
  `README`/`LICENSE`/`CREDITS` file inside the zip, none alongside it in the same
  folder, and no PNG text/metadata chunks beyond a default DPI tag (checked on a
  5-file sample with Pillow's `Image.info`).
- **Coverage vs. the texture census** (`sandover/kb/findings/2026-09-23-texture-census.md`,
  4002 vanilla PNGs / 126 tpages): matching pack `<tpage>/<name>.png` pairs against
  `decompiler_out/jak1/textures/<tpage>/<name>.png` (both listed, no image content
  needed) gives 3456 exact matches, 546 vanilla textures with no pack equivalent, and
  **zero** pack files whose name doesn't match a vanilla name in the same tpage (no
  renames, no added/invented textures). 49 tpages are fully covered, 58 partially, 19
  entirely absent from the pack:

  | tpage (entirely missing) | vanilla count |
  |---|---|
  | citadel-vis-alpha, darkcave-vis-alpha, demo-vis-alpha, finalboss-vis-alpha, firecanyon-vis-alpha, intro-vis-alpha, lavatube-vis-alpha, maincave-vis-alpha, misty-vis-alpha, ogre-vis-alpha, robocave-vis-alpha, rolling-vis-alpha, swamp-vis-alpha, title-vis-alpha, training-vis-alpha, village3-vis-alpha | 9 each |
  | jungleb-vis-alpha | 10 |
  | ocean | 1 |
  | title-vis-tfrag | 1 |

  The gap is heavily concentrated in the `*-vis-alpha` tpages (per-level translucency
  atlases - water, fog, cutout effects): 16 of the 19 fully-missing tpages are `-alpha`
  tpages, and most partially-covered `-alpha` tpages (e.g. `beach-vis-alpha` 1/10,
  `jungle-vis-alpha` 1/10, `snow-vis-alpha` 1/10, `village1-vis-alpha` 3/12) are also
  near-empty. Largest partial gaps by absolute count are in big `-pris`/`-tfrag` tpages
  (`beach-vis-pris` 142/160, `citadel-vis-pris` 154/170, `citadel-vis-tfrag` 82/96,
  `jungle-vis-tfrag` 69/82) - full per-tpage table in the scratch data, not reproduced
  here in full to keep this note short; regenerate from the pack listing plus the texture census
  census on demand.
- **Resolution multiplier:** extracted 47 individual PNGs from the zip (selective `7z x`
  with a file list, ~14 MB total, original zip untouched) stratified across 31 tpages
  spanning the vanilla size histogram (4x4 up to 512x256) and texture kinds (terrain,
  metal, cloth, character/eyes, fur, HUD, fonts, effects). All 47 measured exactly
  `pack_w / vanilla_w == 4.00` (min = max = mean = 4.00x), i.e. a single fixed 4x model
  scale was used uniformly, not a variable/content-adaptive scale. `TextureDB::replace_texture`
  (`decompiler/data/TextureDB.cpp:159-181`) takes the replacement PNG's own width/height
  verbatim via `stbi_load` (unlike `merge_texture`, which requires matching dimensions),
  so this 4x-everywhere replacement bakes in cleanly with no engine-side resize step.
- **Alpha handling:** per-pixel min/max alpha compared on the same 47 samples (Pillow,
  no threshold applied). Vanilla textures that are uniformly opaque read flat `128`
  (confirming the texture census's 128-opaque PS2 convention); their pack equivalents are
  *not* flat - e.g. `village1-vis-tfrag/vil-wallplaster.png` vanilla `128-128` vs pack
  `113-129`, same pattern on 34 of the 47 samples. This reads as the alpha channel
  having passed through the same upscale model as RGB (introducing ringing/noise)
  rather than being preserved or nearest-neighbor-scaled. On textures with real
  translucency the pack sometimes exceeds the vanilla ceiling: `environment-generic/environment-ocean.png`
  vanilla `0-128` -> pack `0-146`; `title-vis-pris/precursor_legacy_japan.png` vanilla
  `0-128` -> pack `0-138`; `gamefontnew/ascii.12lo.png` vanilla `0-128` -> pack `0-141`;
  `ogre-vis-pris/ogre-ltmetal.png` vanilla `0-128` -> pack `0-135`. No sample exceeded
  255 (no accidental full-range 0-255 remap), but several exceed the PS2 GS 0-128
  ceiling that the rest of the pipeline assumes. `TextureDB::replace_texture` copies the
  loaded PNG's alpha bytes verbatim (`memcpy(result.rgba.data(), data, w*h*4)`,
  `decompiler/data/TextureDB.cpp:178`) with no clamp, so these out-of-range values would
  reach the renderer unmodified if baked in as-is.
- **Contact-sheet review** (3 sheets, 12 vanilla-vs-pack pairs each, Pillow, alpha
  display-corrected `v -> min(255, v*2)` per the census's thumbnail fix, composited over
  a checkerboard; sheets kept under `sandover/data/assets/esrgan-v1.0.1/contact_sheets/`,
  not committed):
  - Sheet 1: mostly faithful denoise/sharpen on stone/metal/fabric (`snow-fortwallbase`,
    `dark-eco-barrel-side`, `assis-glove`) - edges and macro structure preserved, PS2
    dither noise cleaned up. Two clear misses: `swamp-vis-pris/billy-tail.png` (flame
    texture) and `bab-allfur.png` (fur noise) both lose their high-frequency detail,
    smeared into soft blobs/flat color rather than sharpened.
  - Sheet 2: `effects/crate-metalbolt-splinter.png` and `eichar/bam-cotton-wrap.png` are
    clear *repaints*, not sharpens - the model invented plausible wood-grain/woven-fabric
    detail that isn't recoverable from the low-res source, i.e. hallucinated but
    plausible texture. `Hud/previous.png` (a cutout icon) shows a clean alpha edge with
    no visible halo. `robocave-vis-tfrag/cv-floorfringe-04-2x2.png` (sheet 3) shows
    visible chromatic/color-noise speckle on a flat rock texture - a typical ESRGAN
    color-noise artifact.
  - Sheet 3: `darkcave-vis-tfrag/cv-wall-stal-stripe-02.png` shows a visible cool/green
    color shift versus vanilla's warmer tan - either a genuine model color-shift
    artifact or a hue drift from the upscale; not confirmed against a third reference.
    No tiling-seam artifacts were visible on any of the tileable terrain samples in
    this set, but the sample is not exhaustive (47 of 3456).
- **Web check, upscalers newer than plain ESRGAN** (original specification, not exhaustive; builds on the
  existing `sandover/kb/findings/2026-09-23-texture-pipeline.md` / ADR-0011, which
  already picked spandrel as the loader and left specific weights as a per-model
  decision):
  - spandrel (MIT loader, unchanged pick) now lists newer feed-forward architectures
    beyond ESRGAN in its supported set - SAFMN, RealPLKSR, DAT, MoESR, FDAT - all
    small/fast CNN- or lightweight-transformer models that stay well under 12 GB even at
    the pack's small (mostly <=512px) source sizes; per-checkpoint license still needs
    checking on OpenModelDB same as any ESRGAN weight.
  - SUPIR (Tencent ARC, SDXL-based diffusion upscaler): non-commercial/research-only
    license on the model, and out-of-the-box VRAM use exceeds a 12 GB card without tiled
    VAE (reports of OOM at 12 GB); with tiled VAE and fp8 it becomes usable near 8-10 GB,
    but it's a diffusion pass, i.e. orders of magnitude slower than a feed-forward model
    across thousands of small textures - same throughput objection the tooling evaluation already raised
    for Marigold-IID.
  - StableSR (IJCV 2024, diffusion-prior-based): open code, no single stated permissive
    license found in this check (needs its own license read before use); same
    diffusion-pass throughput concern as SUPIR.
  - No genuinely "semantic" or material-aware *upscaler* (as opposed to the intrinsic-
    decomposition models the tooling evaluation already evaluated - Marigold-IID, RGB<->X, Material Palette)
    turned up in this check; that category currently means pairing a plain
    upscaler with a separate material/channel-split stage, which is what ADR-0011
    already assumes.

  Sources: [openmodeldb.info/docs/licenses](https://openmodeldb.info/docs/licenses),
  [github.com/chaiNNer-org/spandrel](https://github.com/chaiNNer-org/spandrel),
  [github.com/kijai/ComfyUI-SUPIR OOM-at-12GB issue](https://github.com/kijai/ComfyUI-SUPIR/issues/12),
  [github.com/IceClear/StableSR](https://github.com/IceClear/StableSR).

## Implications for the design
- Usable as the asset-pipeline baseline for 86.4% of textures / 49 fully-covered tpages
  as-is (path structure needs zero translation), but the remaining 13.6% - concentrated
  in translucency (`-vis-alpha`) tpages - needs its own upscale pass regardless of what
  the pipeline eventually standardizes on.
- The alpha-channel noise and occasional >128 values are a real risk if this pack is
  baked in without review: `replace_texture()` performs no clamp, so any texture whose
  pack alpha exceeds the vanilla 0-128 ceiling would reach the renderer as an
  out-of-convention value. Before using any pack texture with non-uniform vanilla alpha,
  clamp/renormalize its alpha channel to 0-128, and treat "flat 128 in vanilla" as the
  correct target for pack textures that read noisy-but-roughly-128 today.
- Style is a genuine mix, not uniformly "faithful": clean denoise/sharpen on most
  hard-edged surfaces, but flame/fur/noise-heavy textures get smeared, and a couple of
  low-detail textures get outright repainted detail (plausible but invented). This
  supports per-texture or per-class QA rather than a blanket accept of the pack.
- No license/author information exists anywhere in the archive or its folder. Treat the
  pack as private-use-only baseline material (per project scope) until/unless the
  author or source page is identified and its terms confirmed; do not assume
  redistribution rights.
- SUPIR/StableSR-class diffusion upscalers are candidates for a later side-by-side but
  are not drop-in replacements for the current pipeline's throughput assumptions
  (ADR-0011 already flagged the same tradeoff for diffusion-based intrinsic-decomposition
  models); the newer spandrel-listed feed-forward architectures (SAFMN, RealPLKSR, DAT,
  MoESR, FDAT) are the more directly comparable "beyond ESRGAN" candidates for a
  same-cost bake-time swap.

## Follow-ups
- This inventory sampled 47/3456 pack textures (1.4%) for resolution/alpha and reviewed
  36 of them visually. A full-pack alpha audit (all 3456 files, cheap - just min/max per
  PNG) would confirm whether the >128 alpha excursions seen here are common or rare
  before any bake-time decision.
- Identify the pack's source/author if possible (mod page, Nexus/GitHub listing) to
  clarify redistribution terms; none of that information travels with the archive
  itself.
- Decide, in a future ADR, whether this pack is used as-is for its 86.4% coverage plus a
  fresh pass for the rest, or fully superseded by a new pipeline run once ADR-0011's
  runner/model choices are finalized.
- The Jak 2 pack in the same folder (`[OpenGOAL] (Jak2) ESRGAN.zip`) was not opened; out
  of scope for this Jak 1 inventory.
