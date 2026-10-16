# HDR captures: EXR next to every screenshot (Jak 1, FP16 target)

The PNG a screenshot writes is always 8-bit and tonemapped. With the FP16 target on
(`(pc-set-hdr-target #t)`, `kb/howto/lighting.md` §6) gk also writes the untonemapped frame as
`<name>.exr`, so range questions and tonemap variants are answered offline from one capture.

## What is in the EXR

- **Buffer:** the one the final pass (`do_pcrtc_effects`, `post_processing.frag`) tonemaps:
  MSAA resolved, bloom and emissive composited, HUD and text included (they draw into the
  target). Stereo: the stereo buffer. Code: `OpenGLRenderer::save_hdr_scene`.
- **Values:** the framebuffer values as the tonemap sees them, 1.0 = SDR white. They are the
  game's display-encoded colour, not decoded from sRGB: `tonemap(exr) * 255` is the PNG. A
  generic EXR viewer (tev, Photoshop) treats EXR as linear and applies its own sRGB curve, so
  the image looks lighter there than in game; use `sandover-hdr stats --preview` to see it
  as the game would.
- **Format:** OpenEXR 2, single part, scanline, channels B G R as HALF, uncompressed,
  top row first. The target is `RGBA16F`, so the half values are exact. 2560x1440 is 22 MB.
- **Header** (string attributes): `sandoverTonemapMode` (`clamp` | `filmic` | `toon`),
  `sandoverTonemapExposure`, `sandoverTonemapKnee`, `sandoverTonemapToonKnee`,
  `sandoverTonemapToonRange`, `sandoverTonemapWhiteStart`, `sandoverTonemapWhiteFull`, `sandoverTonemapHaloStart`, `sandoverTonemapHaloFull`, `sandoverPng` (`internal`: rig and debug
  menu shots, tonemap without brightness/contrast; `window`: F9 and F2, the presented back
  buffer including brightness/contrast).
- **Resolution:** the game render resolution. For rig shots that equals the PNG; for F9 the
  PNG is the window, which can differ from the render resolution.
- Written before the PNG, so a complete PNG means a complete EXR.

With the target off nothing changes: no EXR, and the sidecars say `hdr: false`.

## Where it shows up

| Path | EXR | Sidecar |
|---|---|---|
| rig (`sandover-capture`) | copied next to the PNG, renamed with it by `one` | `hdr`, `hdr_tonemap`, `hdr_stats` (`kb/howto/rig-capture.md`) |
| F9 snapshot (snapshot run) | `<run>/NNNN.exr`; triage moves it with the PNG | `hdr` (`kb/howto/critic-run.md`) |
| compare view | a line per side under the shot label when that side's image has an EXR | none |

## `sandover-hdr`

```sh
uv run sandover-hdr stats shot.exr              # table: peak, mean, % > 1.0, % > 2.0 per R, G, B, max
uv run sandover-hdr stats shot.exr --json
uv run sandover-hdr stats shot.exr --preview shot-filmic.png --curve filmic --knee 0.8 --exposure 1
```

Curves (`sandover/hdr.py`, `tonemap`):

| Curve | What it does |
|---|---|
| `clamp` | the game's clamp mode: clip each channel to 0..1 (exposure unused, as in the shader) |
| `filmic` | the game's filmic mode: exposure, then per channel identity below the knee and an exponential shoulder to 1.0 above it |
| `toon` | the game's toon mode (`--toon-knee`, default 0.95; `--toon-range`, default 1.5): luminance (Rec. 709 weights) identity below the knee, extended-Reinhard shoulder reaching 1.0 at 1 + range, RGB scaled by the luminance ratio; a channel still above 1 is desaturated toward the luminance (at most 25%), then divided by the max channel; hot cores then blend toward `min(c, 1)` by a smoothstep of the max channel from `--white-start` (2) to `--white-full` (3.5), and every overbright halo from `--halo-start` (1.2) to `--halo-full` (1.8). See `kb/howto/lighting.md` section 6 |

Emitter cores against vanilla (#263): `core` prints the mean RGB of the brightest 1% of pixels
(Rec. 709 luma) per window, for the EXR under a curve and for every PNG given with `--vs`:

```sh
uv run sandover-hdr core 0002.exr --curve toon --rect 2150,1130,320,280 --vs 0002.png --vs vanilla/0002.png
```

`clamp`, `filmic` and `toon` previews match the game's PNG for the same settings (up to rounding),
which is the check that the EXR is the tonemap input.

## Env

`SANDOVER_HDR_MASK` in gk's environment still writes `<name>.hdr.png` (above-1 mask) for
every screenshot with the target on.
