# Synthetic VR eye images (Jak 1, OpenXR)

Judge the headset image without wearing the headset. The real OpenXR session runs (SteamVR on,
headset plugged in): `xrWaitFrame`/`xrBeginFrame`, the per-eye swapchain images, the crop into
them and `xrEndFrame` with the real layers. Only the head pose is scripted. Each eye keeps the
runtime's field of view and head-relative view pose (the real IPD), located every frame.

## Engine

- `(pc-set-xr-fake #t)` (or `gk --xr --xr-fake`): the head pose comes from
  `(pc-set-xr-fake-option which value)` instead of tracking. 0-2: yaw, pitch, roll in
  millidegrees (yaw left, pitch up, roll about OpenXR +z); 3-5: head x, y, z in micrometres
  (OpenXR axes); 6: sweep the yaw back and forth over +/- option 0 at this rate (millidegrees
  per second, 0 = held; no per-frame pose lines while sweeping). Turning it on recentres at the
  identity pose; turning it off recentres on the tracked head.
- `SANDOVER_XR_MARGIN_DEG=N` (dev): a fixed head-turn margin for the game's view instead of the
  one sized from the pose age (`findings/2026-10-28-vr-eye-geometry.md`).
- With the fake pose on, frames are drawn and submitted also while the runtime does not ask for
  them (`shouldRender` false: headset not worn). The runtime discards those layers; the engine
  path runs end to end.
- Every pose change logs `[xr] fake head: ...` and one `[xr] fake eye i: rot ... pos_m ...
  head_x_m ... tan ...` line per eye (the game camera to eye rotation, the eye offset in the
  game camera's axes, the eye's tangents). The measurements read these lines.
- A screenshot during an XR frame keeps the session's eye size, render scale included (no target
  re-creation), and its MSAA:
  `msaa_samples` is the session's value, so the rig yaml's `msaa` is ignored for XR shots. With
  the fake pose on it also writes `<name>-left.png` and `<name>-right.png`, read back from the swapchain images before they are
  released: exactly what the runtime was given (not with a tracked head, so F2 in a worn session
  stays cheap). The main PNG is the side-by-side buffer: both
  eyes in the one symmetric frustum they are cropped from.
- Each session logs the located views once: `[xr] view VIEW|LOCAL i flags pos quat fov_rad rec`.
  `sandover-vr-synth profile <gk log> $SANDOVER_DATA/vr/psvr2-profile.json` stores them.

### Without a headset: emulated session

`SANDOVER_XR_EMULATE="w h left right up down ipd_m"` (view 0's fov in radians, view 1 mirrored)
replaces the runtime: no OpenXR instance, two GL textures stand in for the swapchains, the views
come from the variable, and frames are paced at 90 Hz. Everything from the eye targets on is the
real path (build_targets, the crop, the fake pose, the `-left`/`-right` readback). The PSVR2
profile is `"3400 3468 -1.0733775 0.7582840 0.9257251 -0.9257251 0.061"`. Use it to iterate;
confirm a final change on the real session.

## Rig

`sandover/shots/vr-synthetic.yaml`: four cameras (village1-hut, beach-start, jungle-start,
sunken-start), noon. Mono shots run without XR; XR shots need the session:

```sh
cd sandover/tools
uv run sandover-capture run ../shots/vr-synthetic.yaml --play-look --parallel 1 --only <mono shots>
uv run sandover-capture run ../shots/vr-synthetic.yaml --play-look --parallel 1 \
  --goal "(pc-set-xr #t)" --only <xr shots>
```

With `--play-look`, a shot with `(pc-set-xr #t)` (in `--goal` or its own forms) runs the VR
settings profile: the look first sets `*sandover-profile-force*` to `'vr` and resets that set to the
VR defaults (eye frame at 67%), so the session start never swaps profiles mid-shot. Without
`--play-look` the VR set is whatever the slot's `sandover-vr.gc` holds once the session starts.
The rig copies `-left`/`-right` next to the PNG (`xr_eyes` in the sidecar). The first XR shot of
a boot often misses `RIG:RELEASED` (the first eye frames are slow); put a throwaway shot first.
To see the image the play launcher gives, add the HDR presenter:
`--goal "(define-extern pc-set-hdr-output (function int int none))" --goal "(pc-set-hdr-output 1 1000000)" --goal "(pc-set-hdr-output 0 1000000)"`.

## Sheets and tests

```sh
uv run sandover-vr-synth sheet <dir> --pair village1-hut-mono=village1-hut-xr --out ab.png
uv run sandover-vr-synth inventory <gk log>     # SANDOVER_BG_PROBE=1: draws per category per eye
uv run sandover-vr-synth measure <dir> --log <gk log> --cam village1-hut --downscale 8 --out m.json
uv run sandover-vr-synth sky <dir> --log <gk log> --shot village1-hut=village1-hut-xr-nojak --out s.json
```

- `sheet`: mono, left swapchain, right swapchain, and an anaglyph of the side-by-side halves
  (one shared frustum, so content at infinity overlays exactly).
- `measure`: warps the mono shot through each eye's logged rotation and asymmetric window and
  registers it against the swapchain image (residual angle, scale, shift; all 0 when the eye is
  right). Poses: identity (`-xr-nojak`), yaw/pitch/roll 10 degrees. A negative control (the
  identity prediction against the pitch-10 image) must fail. Disparity: per row band, xL - xR on
  the side-by-side halves; 0 at the sky, positive and growing toward the camera.
- `sky`: the identity-head eyes against the mono shot above the horizon (from the shot's camera
  rotation): coverage (share not black) mono vs eye, the eye's coverage beyond the mono frame,
  agreement within 48 levels, and the same in the first 6 degrees above the horizon (far scenery).
  The sky band fix passed at |coverage difference| under 2 % (`kb/findings/2026-10-28-vr-sky-band.md`).

## Per-eye motion vectors and temporal AA

With temporal AA on (the VR profile's upscaler), each eye has its own motion vectors, upscaler
context and AO history (`findings/2026-10-28-vr-per-eye-upscaling.md`).

- `(pc-motion-vectors-dump "<dir>" N)` during XR (or debug stereo) writes both eyes per frame:
  `mv_NNN_e0_*` and `mv_NNN_e1_*` (colour, velocity, aux with the clip w, the upscaler output) and
  a JSON with `tick` (display frame), `eye` and `game_frame_new` (false on a redraw of a game frame
  already shown).
- Head step: dump a few frames and change `(pc-set-xr-fake-option 0 <millidegrees>)` while it
  runs; then

  ```sh
  uv run sandover-vr-synth mvstep <dump dir> --log <gk log> --out mv.json
  ```

  predicts every pixel's previous position of the step frame from its own depth and the two
  logged eye poses (no engine matrices) and reports the error per eye; exit 0 when the centre
  box (64 px) stays under 0.5 px in every step frame.
- `SANDOVER_MV_STATIC=1` (dev): the camera part of the vectors is identity (no camera or head
  motion), the negative control for ghosting runs.
- Debug stereo (`(pc-set-stereo #t)`, no session) runs the same per-eye path;
  `(pc-set-stereo-option 0 0)` (IPD 0) makes both eyes the mono camera, so each eye must match the
  other and the mono frame.

## Sky knobs

- `SANDOVER_XR_SKY_CLIPPED=1`: in XR the sky keeps the game's clipped GS polygons (reprojected at
  infinity) instead of the unclipped directions (A/B).
- `SANDOVER_XR_SKY_LOG=1`: logs the sky's GS vertices and `xr_gs` state for the first eye frames.

## Debug knobs (only with `SANDOVER_BG_PROBE=1`)

- `SANDOVER_BG_PROBE_SKIP=3,4`: those buckets are not drawn (which renderer made a region).
- `SANDOVER_BG_PROBE_NOGUARD=1`: the buckets start on whatever framebuffer the passes before
  them left bound (no rebind), so the probe shows whether any pass still leaks.
- The probe line lists `pre:<fb>` and `stages <stage>:<fb>` when a stage before the buckets
  leaves another framebuffer bound.
