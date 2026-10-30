---
title: Can the game's positional voices go through OpenAL Soft's ambisonic mixer to binaural, a speaker bed and Windows Spatial Sound?
date: 2026-09-24
issue: none (not filed)
status: verified
sha: branch spatial-audio
---
## Answer (≤5 lines)
Yes for all three, behind `--audio-spatial` / `(pc-set-audio-spatial "<mode>")`. Positional
sounds (falloff curve set) leave the 989snd mix as mono before the pan and become OpenAL Soft
sources at their world position; the listener is the camera position and yaw. The loopback
test scene shows correct placement in binaural, 5.1 and 7.1. `objects` opens a 7.1.4 static
bed on the Windows Spatial Sound stream of this machine's endpoint and falls back to the
speaker decode when the object budget is 0. `off` never opens OpenAL. Nobody has listened to it yet.

## Evidence
Data: `$SANDOVER_DATA/audio/` (`probe/levels.md` and WAVs, `gk-*.log`, OpenAL Soft logs).

**Test scene** (`spatial_probe`, OpenAL Soft loopback device, 48 kHz): white noise 4 m
from a fixed listener. Per-channel RMS in dBFS; -200 is silence.

Binaural (built-in HRTF):

| position | L | R | ILD L-R dB | ITD (+: right ear later) |
|---|---|---|---|---|
| front | -22.3 | -22.3 | 0.0 | 0 us |
| left | -18.1 | -33.0 | +14.9 | +854 us |
| right | -33.1 | -18.1 | -15.1 | -854 us |
| behind | -27.3 | -27.3 | 0.0 | 0 us |
| above | -26.2 | -26.2 | 0.0 | 0 us |
| listener yawed 90°, source on its new front | -22.3 | -22.3 | 0.0 | 0 us |
| listener yawed 90°, source on its new left | -18.0 | -33.0 | +15.0 | +854 us |

Front, behind and above differ only in spectrum and overall level, as expected for HRTF on
the median plane.

5.1:

| position | FL | FR | FC | LFE | SL | SR |
|---|---|---|---|---|---|---|
| front | -21.0 | -21.0 | -24.0 | - | -42.0 | -42.0 |
| left | -23.0 | -41.3 | -45.7 | - | -18.2 | -58.2 |
| right | -41.3 | -23.1 | -45.8 | - | -58.3 | -18.2 |
| behind | -47.9 | -47.9 | -48.1 | - | -20.0 | -20.0 |
| above | -28.6 | -28.6 | -34.6 | - | -24.8 | -24.8 |

7.1:

| position | FL | FR | FC | LFE | BL | BR | SL | SR |
|---|---|---|---|---|---|---|---|---|
| front | -19.8 | -19.8 | - | - | -62.7 | -62.7 | -51.3 | -51.3 |
| left | -26.7 | -37.8 | - | - | -26.7 | -37.8 | -17.9 | -40.3 |
| right | -37.9 | -26.7 | - | - | -37.9 | -26.7 | -40.3 | -17.9 |
| behind | -62.3 | -62.3 | - | - | -19.8 | -19.8 | -50.9 | -50.9 |
| above | -29.4 | -29.4 | - | - | -29.4 | -29.4 | -29.4 | -29.4 |

OpenAL Soft's default 7.1 decoder leaves the centre channel silent (the 5.1 one uses it);
an ambdec preset could change that. With no height speakers, "above" spreads evenly.

**In game** (`gk -boot -fakeiso`, title and attract sequence, primary checkout data):

| mode | log |
|---|---|
| off | no `[spatial]` line; cubeb stream as before |
| binaural | `output stereo-hrtf, hrtf on, 48000 Hz`; 270 positional sounds placed as sources in 90 s, 2-3 at a time; a rerun on the final commit placed 234 in 75 s |
| objects | `endpoint dynamic-object budget 128`, `output device layout (spatial stream)`; 183 sounds placed in 60 s |

**Windows Spatial Sound on this machine**: the default render endpoint (a USB DAC's S/PDIF
output) reports 128 dynamic objects and a full static mask (`0xffffe`), so a headphone
spatial format (Windows Sonic, Dolby Atmos for Headphones or DTS Headphone:X) is on. OpenAL
Soft's log: `Post-reset: 7.1.4 Surround, Float32, 48000hz, 480 / 960 buffer`, first-order
periphonic decoder. With `SANDOVER_SPATIAL_OBJECT_BUDGET=0` the same run opens
`stereo-basic` (the endpoint's speaker layout) instead.

**Cost** (indicative: render time as a share of real time, one thread, sources moving every 16.7 ms;
not acted on):

| layout | 0 sources | 8 | 32 (all slots) |
|---|---|---|---|
| binaural | 0.5-1.0% | 1.2-1.5% | 4.0-4.4% |
| 5.1 | 0.2% | 0.5% | 1.7-3.3% |
| 7.1 | 0.2% | 0.5% | 1.8-1.9% |

Ranges are two runs on a machine that was busy with other builds and games.

This is OpenAL Soft's mix only. The 989snd synth runs as before, now pulled by OpenAL's mixer thread.
The title sequence never had more than 3 sources active.

**Two OpenAL Soft 1.25.2 bugs**, patched in the vendored copy (listed in `vendor.yaml`):
- `alc/backends/wasapi.cpp`: the spatial stream's activation `PROPVARIANT` holds a blob
  pointing at a stack struct, and `PropVariantClear` frees it. That crashed with heap
  corruption (`0xC0000374`) on every `objects` start on an endpoint with spatial sound on.
  Worth reporting upstream.
- `common/opthelpers.h`: `[[clang::nonblocking]]` member function pointers break the MSVC
  STL's `std::ranges::for_each` under clang, so it is disabled there.

## Design as built
- `game/sound/989snd/spatial.{h,cpp}` `SpatialOutput`: 32 mono callback sources plus one
  stereo "bed" source (direct channels, front L/R) for everything non-positional. OpenAL's
  mixer pulls samples, which run the synth forward under the synth lock. cubeb is stopped
  while a mode is on and restarted when the mode goes back to `off`.
- `Synth::TickRouted` hands the voices of claimed sound handles to the router and not to the
  stereo mix. The voice's owner is set in `sfxgrain.cpp` when a tone starts. The pan of
  claimed sounds is forced to centre, so L+R is the unpanned signal.
- Positions are overlord sound units (game units / 16). Sources are sent in listener space.
  At camera yaw a, left is (cos a, 0, -sin a) and forward is (sin a, 0, cos a). Mirror mode flips left and right.
- Overlord: `RPC_Player` passes the position of sounds with `fo_curve != 0`.
  `UpdateLocation` moves them every frame. `SetEarTrans` sets the listener (camera
  translation, camera yaw, mirror mode) and flushes.
- The distance model is off, so loudness is still the game's falloff against Jak.
- Music, streams, VAG ambients and MIDI stay on the bed.

## Gaps
| Gap | Why it matters | Where |
|---|---|---|
| Nobody has listened | Levels prove direction, not quality, clicks or latency | Human check (howto) |
| Front/back is inferred | Left/right follows the vanilla pan (checked against `CalculateAngle` and the pan table at several yaws), but the stereo pan cannot tell front from back. Forward is taken as left × up, assuming a right-handed world. If that is wrong, front and back swap | Listening check, or a known emitter in front of the camera |
| Loopback scene drives `SpatialOutput` directly, not a 989snd bank | The voice routing is only proven in game by the claim counters | Capture a real bank sound through `sndplay` or the game |
| No dynamic objects | OpenAL Soft writes only the 7.1.4 bed; "hero" voices as objects need upstream work or our own `ISpatialAudioClient` | later spatial audio work |
| Streamed VAG ambients (`iso.cpp` pan) stay stereo | Some ambient emitters are streams | Later |
| Switching into `objects` mid-process keeps the first OpenAL config | It needs `--audio-spatial objects` at start | Pass config through `alcOpenDevice` attributes if possible |
| A1 parity capture for `off` not run | `off` code path is unchanged by inspection only | Parity rig with audio capture |

## Follow-ups
- Human: listen with headphones in `binaural`, and in `objects` with the current spatial format.
- Report the `PropVariantClear` bug upstream (OpenAL Soft).
