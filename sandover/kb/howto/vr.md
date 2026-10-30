# VR (OpenXR) how-to (Jak 1)

Stereo through OpenXR on any conformant runtime, ADR-0004. Off by default;
Jak 1 only; Windows only for now (the GL binding is `XR_KHR_opengl_enable` on Win32).
State of the spike: `kb/findings/2026-09-23-openxr-spike.md`.

## Turn it on

- Command line: `gk --xr ...` (starts trying at boot).
- GOAL: `(pc-set-xr #t)` / `(pc-set-xr #f)`.
- ImGui: Renderer Debug → **Stereo** → **OpenXR**. The line under the checkbox shows the XR
  state, runtime, per-eye size and format, session state and frame timing.

With the toggle on, the engine keeps trying: no runtime → retry every 5 s; runtime but no
headset → `xrGetSystem` every 2 s; session setup failure → retry after 10 s. Until a
session runs, the window renders exactly as with the toggle off. Once the session runs:

- both eyes are drawn with the stereo replay, following the head (rotation, a small
  translation and the runtime's per-eye field of view; see `howto/xr.md`), and each is
  copied into its swapchain image;
- the eye offset comes from the runtime's view poses (the real IPD), not
  `stereo_ipd_m`; `stereo_world_scale` still applies;
- the window shows the side-by-side mirror (left eye on the left);
- `xrWaitFrame` paces the loop at the headset's rate: window vsync and the frame limiter are
  off while it runs, the last game frame is drawn again every display frame, and the game
  keeps a 60 Hz vsync of its own. A vsync change during the session (the display-mode switch
  flips it) reaches the window only after the session ends; the `present:` log line shows the
  window's `swap interval` (0 in a session).

Every step is logged with an `[xr]` prefix (state changes, runtime name, extensions, system,
recommended sizes, swapchain formats, session states, a timing line every 5 s).

## Before any test: check the active runtime

Several runtimes can be installed and any of them can take `ActiveRuntime`. Check it:

```powershell
Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' | Select-Object ActiveRuntime
```

It should point at SteamVR's `steamxr_win64.json`. The engine also logs the value at
start. To force a runtime for one process only, set `XR_RUNTIME_JSON` to that runtime's
JSON before starting `gk`.

## Test with a headset

1. Connect the headset and start SteamVR (or the headset's own OpenXR runtime); wait until
   the headset shows as ready.
2. Check `ActiveRuntime` (above).
3. Start the game with `--xr` (or boot normally and run `(pc-set-xr #t)`).
4. Expect in the log: `state NO_SYSTEM -> IDLE (session created)`, then
   `session IDLE -> READY`, `state IDLE -> RUNNING`, `session ... -> FOCUSED`.
5. What you should see, the recentre key and the known limits: `howto/xr.md`.

## Test without a headset: SteamVR null driver

SteamVR ships a "null" headset driver that makes `xrGetSystem` succeed with no hardware.
Enabling it is a change to the SteamVR settings, so tools never do it. To do it by
hand:

1. Quit SteamVR.
2. Edit `Steam/config/steamvr.vrsettings` (back it up first):
   - in the `"steamvr"` section add
     `"requireHmd": false, "forcedDriver": "null", "activateMultipleDrivers": true`;
   - add a top-level section `"driver_null": { "enable": true }`.
3. Start SteamVR. It shows a "Null Model Number" headset; the compositor draws its view in a
   desktop window (defaults from the driver: 1512×1680 per eye, 90 Hz).
4. Run the game with `--xr` as above.
5. To undo: restore the backup (or remove the added keys and the `driver_null` section).

A null-driver session exercises the session, swapchains and frame loop, not the lens
distortion or the real frame budget.

## Frame cost

Where a display frame's time goes, at 33 % and 67 % render scale, and the next lever (one
asymmetric frustum per eye, about 24 % fewer output pixels): `kb/findings/2026-10-30-vr-frame-pipeline.md`.
The `[xr] pose age` line (every 5 s) splits the age of the game's view at its last display into
its stages: sample -> game read -> chain sent -> taken by the render thread -> last display,
plus the display period.

## Render stalls (keep-alive, experimental)

One render-loop iteration is one OpenXR frame, so a stall on the render thread (a driver
pipeline build on a first draw, a blocking load) leaves the runtime without frames.
`SANDOVER_XR_KEEPALIVE=1` in gk's environment turns on an experimental keep-alive thread; it is
off by default. When nothing reached the runtime for 150 ms it ends the render thread's begun
frame with no layers, then submits empty frames at the display rate (`xrWaitFrame` paces it)
until the render thread begins a frame again; that frame's late end is dropped. A failed
`xrBeginFrame` stops the empty frames until then. The headset shows black for the stall instead
of a frozen image.

It is off by default because `XR_KHR_opengl_enable` expects the frame calls on the thread whose
GL context the session was created with, and the keep-alive calls `xrBeginFrame`/`xrEndFrame`
from a second thread while the render thread holds that context. The keep-alive's calls and the
render thread's `xrWaitFrame`, `xrBeginFrame`, image release and `xrEndFrame` share one mutex;
image acquire and wait run outside it, and the thread is joined before `xrEndSession` and
session teardown. On SteamVR (PSVR2, one boot with the launcher's effects) it ran without
OpenXR errors but did not keep the display rate during the boot stalls: its empty frames went
out 3.0-5.3 s apart at worst, and the 5.4 s stall right after the session began got none
(on the emulated session, 12 s without a frame when the keep-alive is off). Where its calls
wait was not isolated (`kb/findings/2026-10-30-vr-frame-pipeline.md`). The log has
`[xr] submit gap <ms> ms` for
every gap over 50 ms and `[xr] keep-alive: <n> empty frames over a <ms> ms render stall`
(`kb/howto/perf.md`, hitch log; `kb/findings/2026-10-30-warm-up.md`).

## Known gaps

Head tracking, the field of view and the display rate are covered in `howto/xr.md`, with
its list of known limits. Still open from the spike: the diorama anchor and union camera
 and the HUD quad layer. Without tracking (lost, or a runtime that
reports none) the eyes fall back to a fixed stereo pair, head-locked, still drawn with the
runtime's per-eye field of view.

Without wearing the headset: synthetic eye images through the real session, `howto/vr-synthetic.md`.
