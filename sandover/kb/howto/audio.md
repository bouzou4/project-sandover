# Spatial audio how-to (Jak 1)

Positional sound effects through OpenAL Soft, ADR-0017. Off by default; Jak 1
only; Windows only for now. State of the spike: `kb/findings/2026-09-24-spatial-audio-spike.md`.

## Turn it on

- Command line: `gk --audio-spatial <mode> ...`
- GOAL: `(pc-set-audio-spatial "<mode>")`. goalc needs the declaration from
  `kernel-defs.gc`; on an older `out/`, declare it by hand first:
  `(define-extern pc-set-audio-spatial (function string none))`.

| Mode | What it does |
|---|---|
| `off` | Default. The vanilla cubeb stereo stream; OpenAL is never loaded or opened. |
| `binaural` | HRTF over the default device (OpenAL Soft's built-in dataset). For headphones. |
| `surround` | Decodes to the default device's speaker layout as Windows reports it (5.1, 7.1, ...). A stereo endpoint gets a plain stereo pan. |
| `objects` | OpenAL Soft's Windows Spatial Sound stream: a static bed (up to 7.1.4) handed to the enabled spatial format (Windows Sonic, Dolby Atmos, DTS). Falls back to `surround` when the endpoint has no spatial format. |

`objects` sets `ALSOFT_CONF` to `sandover-alsoft-objects.ini` (next to `gk`), which OpenAL
Soft reads once per process. Start with `--audio-spatial objects`; switching into it later
in the same process keeps whatever OpenAL was first configured with (a warning is logged).

The log has a `[spatial]` line per switch naming the device, output mode, HRTF state and
rate. OpenAL Soft's own log: set `ALSOFT_LOGLEVEL=3` and `ALSOFT_LOGFILE=<file>`.

## What moves where

- Sounds started through the overlord with a falloff curve (`fo_curve != 0`) get an OpenAL
  source at their `sound-play-parms` position, updated every frame with the listener.
  Their voices are mixed to mono before the pan (the pan is forced to centre).
- The listener is the camera position and yaw (the vanilla pan's reference); loudness
  still comes from the game's falloff against Jak's position (OpenAL's distance model is off).
- Everything else (music, non-positional effects, streamed VAG ambients, sounds beyond the
  32 source slots) stays on the vanilla stereo mix, played on the front left/right pair.
- While a mode is on, OpenAL's mixer clocks the synth and the cubeb stream is stopped.

## Windows Spatial Sound (needed for `objects` only)

1. Settings → System → Sound → the output device → **Spatial audio**: pick Windows Sonic
   for Headphones (free), or Dolby Access / DTS Sound Unbound (need their apps and, for
   Atmos/DTS:X, a licence).
2. Check: `spatial_probe --endpoint` prints the default endpoint's dynamic-object budget
   and static mask. `gk` logs the same budget when `objects` starts.
3. Budget 0 means no spatial format on that endpoint: `objects` then skips the spatial
   stream and decodes to the speaker layout, exactly as `surround`. To test that path on a
   machine where spatial sound is on, set `SANDOVER_SPATIAL_OBJECT_BUDGET=0`.

OpenAL Soft 1.25.2 writes a 7.1.4 static bed into the spatial stream (no dynamic objects),
so the budget decides whether the spatial path is taken, not how many sources it carries.
The Windows format (Sonic, Atmos, DTS) then renders that bed for the endpoint.

## Test scene: `spatial_probe`

Built with `gk` on Windows (`ninja spatial_probe`). It drives the real `SpatialOutput`
class with a looping noise source, 4 m from a fixed listener, at front, left, right,
behind, above and (listener turned 90°) front again, rendering through OpenAL Soft's
loopback device (no audio hardware involved):

```sh
spatial_probe <out_dir>        # binaural.wav, surround-5.1.wav, surround-7.1.wav, levels.md
spatial_probe --endpoint       # Windows Spatial Sound state of the default endpoint
spatial_probe --device objects # opens the real device in a mode for 1 s of silence
```

`levels.md` has per-channel RMS per position, binaural interaural level and time
differences, and the mixer cost with 0, 8 and 32 moving sources. The WAVs play the
positions in order (0.75 s each, 0.25 s gaps).

## Listening checks (human)

- Headphones: `gk --audio-spatial binaural`; a positional sound (a waterfall, a
  precursor door, an eco vent) should stay put in the world as the camera turns,
  including behind.
- Surround receiver: set Windows' speaker configuration to 5.1 or 7.1 first, then
  `--audio-spatial surround`.
- `objects`: enable a spatial format (above), then start with `--audio-spatial objects`.
