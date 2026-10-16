---
title: Spatial audio — verified license and platform-capability facts
date: 2026-09-23
issue: not filed (design-review follow-up)
status: verified
sha: 2dd18a9ca76cb6876d6e99667b42c647516f8ad4
---

## Answer (≤5 lines)

The review's promotion of spatial audio to a real phase (`design-inputs.md` "Review
decisions") is buildable as a single pipeline, not three separate ones. OpenAL Soft's
current WASAPI backend already implements binaural HRTF, standard multichannel output,
and Windows Spatial Sound object output (`ISpatialAudioClient`) from one internal
ambisonic mixer — verified directly against its source, not just its docs. cubeb (the
engine's current backend) supports multichannel channel layouts in its public API but
has no `ISpatialAudioClient` code anywhere in its WASAPI backend. Steam Audio's
documented ambisonic decode path is HRTF-binaural only. This is the basis for
ADR-0017's decision to route positional voices through one ambisonic-encode stage
feeding three decoders, using OpenAL Soft, rather than three independent pipelines.

## Evidence (path:line@sha / URL, short excerpts)

**Engine's current output (in-repo, verified against this checkout):**
- `game/sound/989snd/player.cpp:43-48` (`sha:2dd18a9`): `cubeb_stream_params outparam`
  is hard-wired `channels = 2`, `format = CUBEB_SAMPLE_S16LE`, `rate = 48000`,
  `layout = CUBEB_LAYOUT_STEREO`. One mixed stereo stream, no per-voice output.
  Matches the earlier planning claim exactly.

**OpenAL Soft — license:**
- License is "LGPL 2 or later." — [openal-soft README](https://raw.githubusercontent.com/kcat/openal-soft/master/README.md);
  same statement on the project site: [openal-soft.org](https://openal-soft.org/).
- The bundled default HRTF dataset (derived from SADIE II) is separately Apache-2.0
  licensed, so it carries no LGPL copyleft of its own.

**OpenAL Soft — output modes and internal architecture:**
- Supports mono, stereo, quad, 5.1, 6.1, 7.1, HRTF, UHJ and B-Format (ambisonic)
  output, and first- through fourth-order ambisonic decoding (FuMa/ACN layouts,
  FuMa/SN3D/N3D scaling) with dual-band decode —
  [openal-soft.org](https://openal-soft.org/), [`docs/ambisonics.txt`](https://raw.githubusercontent.com/kcat/openal-soft/master/docs/ambisonics.txt).
- `docs/ambisonics.txt`: "OpenAL Soft ... can render using Ambisonics and decode the
  ambisonic mix for a high level of accuracy over what simple pan-pot could provide."
  Ambisonics is the library's internal mixing stage; every source is encoded into the
  soundfield once, then decoded to whichever output (speaker layout, HRTF, or
  passthrough B-Format) the active device is configured for. This is exactly the
  "one pipeline, three outputs" shape the review's decision needs.

**OpenAL Soft — Windows Spatial Sound (`ISpatialAudioClient`), verified in source, not
just requested:**
- A 2021 feature request for Microsoft Spatial Sound support
  (https://github.com/kcat/openal-soft/issues/586) is still open and unassigned, which
  by itself would suggest the feature doesn't exist — but that issue predates the
  work. Pulling `alc/backends/wasapi.cpp` directly off `master`
  (https://raw.githubusercontent.com/kcat/openal-soft/master/alc/backends/wasapi.cpp,
  fetched 2026-09-23, 3125 lines) shows it implemented:
  - `#include <spatialaudioclient.h>`; `ComPtr<ISpatialAudioClient> mClient`;
    `ComPtr<ISpatialAudioObjectRenderStream> mRender` (~line 1171-1172).
  - Two overloads of `mixerProc`: one for the plain `IAudioClient` device path, one
    for the `SpatialDevice` (`ISpatialAudioClient`) path (~line 1176-1177,
    1256, 1374).
  - The spatial `mixerProc` calls `audio.mRender->BeginUpdatingAudioObjects(...)`,
    writes into the returned object buffers, then
    `audio.mRender->EndUpdatingAudioObjects()` (~line 1403-1465) — the exact pattern
    Microsoft's own sample uses (see below).
  - `audio.mClient->ActivateSpatialAudioStream(...)` with a
    `SpatialAudioObjectRenderStreamActivationParams` carrying a `StaticObjectTypeMask`
    (~line 1894-1911) — i.e. it opens a static channel bed sized from the device's
    negotiated channel mask, not just bare dynamic objects.
  - Both `mixerProc` overloads call
    `AvSetMmThreadCharacteristicsW(taskname, &sAvIndex)` (~line 1275, 1383) before
    entering their render loop — registers the mixer thread with MMCSS (the same
    "Pro Audio"/"Audio" real-time thread class WASAPI apps are expected to use), which
    is the concrete form of "runs on the audio thread" for this backend.
  - A `// TODO: ISpatialAudioClient::GetMaxFrameCount ...` comment (~line 1926) shows
    the implementation is recent/still being refined, not a finished-years-ago
    feature — worth re-checking against the exact pinned OpenAL Soft version at
    integration time.

**Steam Audio — license and output shape:**
- Apache-2.0 for the full SDK and all integration plugins as of release 4.5.2 (Valve
  open-sourced the previously-closed core in that release) —
  [Phoronix](https://www.phoronix.com/news/Steam-Audio-SDK-Fully-Open),
  [Valve's Steam Audio announcement](https://steamcommunity.com/games/596420/announcements/detail/7745698166044243233).
- Its Ambisonic Source component's documented behavior: "the Ambisonic audio clip is
  spatialized using HRTF-based binaural rendering" —
  [Steam Audio Unity docs, Ambisonic Source](https://valvesoftware.github.io/steam-audio/doc/unity/ambisonic-source.html).
  No documented decode path to multichannel speaker layouts or to object-based
  output; Steam Audio's ambisonics is an *encode* format for sources and reverb, with
  HRTF binaural as the only documented decoder. Matches the general pattern for
  game-engine ambisonic decoder plugins (Unity's own docs describe first-order
  ambisonic decoders as binaural/stereo/quad-output plugins, not surround-bed
  decoders).

**cubeb — multichannel API exists; no Windows Spatial Sound path:**
- `include/cubeb/cubeb.h` (https://raw.githubusercontent.com/mozilla/cubeb/master/include/cubeb/cubeb.h)
  defines `CUBEB_LAYOUT_*` including 5.1 (`CUBEB_LAYOUT_3F2_LFE`,
  `..._3F2_LFE_BACK`) and 7.1-class layouts (`..._3F4_LFE`), so a cubeb stream can be
  opened with more than 2 channels and a named layout — reaching 5.1/7.1 multichannel
  bed output is a matter of changing `outparam.channels`/`.layout` in
  `player.cpp:44-47` and mixing each positional voice to N channels instead of
  stereo pan, not a new backend.
- `src/cubeb_wasapi.cpp` (https://raw.githubusercontent.com/mozilla/cubeb/master/src/cubeb_wasapi.cpp,
  3706 lines): zero matches for `SpatialAudio` or `ISpatialAudioClient`. cubeb only
  ever activates the standard `IAudioClient` interface. It cannot reach Atmos/DTS:X/
  Windows Sonic object endpoints; that path needs either OpenAL Soft's WASAPI backend
  (above) or a hand-written `ISpatialAudioClient` client mirroring Microsoft's sample.

**`ISpatialAudioClient` — object limits and threading pattern:**
- [Spatial sound for app developers](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound)
  (updated 2024-07-18, checked 2026-09-23): static channel bed up to 8.1.4.4 (17
  channels). Current (post-May-2023-servicing) Windows dynamic-object budgets: Dolby
  Atmos home theater (HDMI) 20, DTS:X for home theater 32, Dolby Atmos headphones/
  built-in speakers 128, DTS Headphone:X 128, DTS:X Ultra 128, Windows Sonic
  headphones 128 (LFE not counted as an object). `GetMaxDynamicObjectCount` returns 0
  when Microsoft Spatial Sound isn't enabled on the endpoint — an app must treat that
  as "no dynamic objects available, fall back to the channel bed," not an error.
- Three documented integration patterns: (a) extend an existing 7.1 panner to the
  four height channels of a 7.1.4/8.1.4.4 static bed, (b) keep the existing stereo/
  multichannel endpoint *and* add a static-bed `ISpatialAudioClient` endpoint side by
  side (both run on the same buffer size/clock), or (c) send select "hero" voices to
  dynamic objects while everything else stays on the bed — recommended for a limited
  number of foreground sounds given the per-format object budgets above.
- [Render spatial sound using spatial audio objects](https://learn.microsoft.com/en-us/windows/win32/coreaudio/render-spatial-sound-using-spatial-audio-objects)
  (checked 2026-09-23): the render loop is `WaitForSingleObject(bufferCompletionEvent,
  100)` → `BeginUpdatingAudioObjects` → write each active object's buffer plus
  `SetPosition`/`SetVolume` → `EndUpdatingAudioObjects`, driven entirely by an
  OS-signaled event on its own thread — the audio-thread requirement is structural
  (you can only touch object buffers inside that Begin/End window), not just a
  performance recommendation. OpenAL Soft's own implementation (above) runs this
  exact loop on an MMCSS-registered thread.

## Implications for the design

- Recommend a single ambisonic-encode stage (per active positional voice) feeding
  three decoders — HRTF binaural, multichannel channel-bed, and
  `ISpatialAudioClient` object/bed — rather than three separate spatializers. OpenAL
  Soft already implements exactly this shape end to end on Windows, so ADR-0017 picks
  it over hand-rolling a custom ambisonic bus or wiring cubeb + a raw
  `ISpatialAudioClient` client + a separate HRTF library as three uncoordinated paths.
- The engine's existing cubeb stereo path (`player.cpp`) is untouched when
  `spatial_audio` is off — it stays the parity anchor. OpenAL Soft becomes a second,
  opt-in output path for positional voices only; non-positional music/streams can
  stay on cubeb (per the existing note that they aren't positional) or move over
  later, out of v1 scope.
- Steam Audio is not a fit for the core output pipeline (binaural-only decode
  documented), but its physics-based occlusion/reflection/reverb modeling remains a
  candidate *input* stage layered before OpenAL Soft's mixer in a later phase — this
  matches the existing note that occlusion needs scene geometry and is out of scope
  for v1.
- `GetMaxDynamicObjectCount` can legitimately be 0 (spatial sound not enabled on the
  user's endpoint); the design must degrade to the static channel bed or to plain
  multichannel/stereo without treating that as a failure.

## Follow-ups

- The spatial audio integration spike pins the exact OpenAL Soft version/commit used and re-checks the
  `ISpatialAudioClient` backend against that pin (the `// TODO: GetMaxFrameCount`
  comment suggests it's still evolving upstream).
- The spatial audio integration spike also verifies, empirically, whether OpenAL Soft's WASAPI stream and the
  engine's existing cubeb WASAPI stream can be open on the same endpoint
  simultaneously (the design assumes yes per the "two endpoints" pattern Microsoft
  documents, but that's for one renderer using both APIs itself, not two separate
  audio libraries in the same process — worth confirming directly).
- Whoever files the spatial audio GitHub issues should attach this finding and ADR-0017.
