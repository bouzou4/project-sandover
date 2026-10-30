# ADR-0017: Spatial audio pipeline (surround, spatial object, and binaural output)
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
Spatial audio was promoted from an optional late item to a real phase: surround multichannel output (5.1/7.1),
spatial object output so Atmos/DTS:X/Windows Sonic decoders the user owns actually
render it, and binaural HRTF for headphones, with the VR listener coming from the
headset pose. The recorded engine facts (unverified pending the spatial audio integration spike, kept as recorded in
the audio research note) are that every 3D sound already carries a world position in
`sound-play-parms`, the listener is Jak's position plus camera yaw, and today's path
pans each voice to stereo through a 361-entry pan table before a cubeb output stream
hard-wired to 2 channels at 48 kHz (`game/sound/989snd/player.cpp:43-48`, verified
directly against this checkout, `2dd18a9`).

Three output families are now required at once, not one: a channel bed for surround
receivers without an object decoder, an object stream for the Atmos/DTS:X/Sonic
decoders that do, and HRTF for headphones. The question this ADR answers is whether
those are three independent spatializers or one shared pipeline.

`kb/findings/2026-09-23-audio-research.md` has the full verification; the load-bearing
facts are summarized under Options below.

## Options (with cost/risk)

**Three direct per-output pipelines (rejected).** Extend cubeb's stream to a
multichannel layout for the bed (cubeb's public API already has 5.1/7.1
`CUBEB_LAYOUT_*` constants), add a second, independent HRTF library (OpenAL Soft or
Steam Audio) for headphones, and hand-write a third `ISpatialAudioClient` client
following Microsoft's sample pattern for object output. Verified: cubeb's WASAPI
backend has zero references to `ISpatialAudioClient` anywhere in its source, so the
object path cannot be added to cubeb — it would be a fully separate, hand-rolled COM
client. Cost: three independent per-voice attenuation/panning implementations that
must all read `sound-play-parms` and the listener pose the same way, three places for
a parity bug between "spatial audio on" and "off" to hide, and a hand-written
`ISpatialAudioClient` client is real Windows Core Audio work (activation, format
negotiation, the `BeginUpdatingAudioObjects`/`EndUpdatingAudioObjects` render loop on
an MMCSS-registered thread) that the project would own and maintain alone.

**Steam Audio as the shared engine (rejected as primary).** Apache-2.0, encodes
sources to ambisonics and models occlusion/reflection/reverb from scene geometry —
useful, and licensing is not a blocker. But its documented ambisonic decode path is
HRTF-binaural only (its Ambisonic Source component "is spatialized using HRTF-based
binaural rendering"); there is no documented decode to a multichannel speaker bed or
to `ISpatialAudioClient` objects. It would still need a separate multichannel/object
output stage bolted on, so it doesn't remove the three-pipeline problem by itself.
Scene-geometry occlusion is also out of scope for v1 regardless (no occlusion mesh
plumbing exists yet).

**Ambisonic bus feeding three decoders, via OpenAL Soft (chosen).** Encode every
positional voice into a shared ambisonic soundfield once, from its `sound-play-parms`
world position and the listener pose, then decode that one soundfield three ways:
HRTF binaural for headphones, a channel-bed decode for 5.1/7.1 speakers, and an
`ISpatialAudioClient` object/bed stream for Atmos/DTS:X/Windows Sonic endpoints.
OpenAL Soft (LGPL 2 or later, dynamically linked) already implements exactly this
shape on Windows: its internal mixer renders through ambisonics ("OpenAL Soft ... can
render using Ambisonics and decode the ambisonic mix for a high level of accuracy over
what simple pan-pot could provide," `docs/ambisonics.txt`), it supports mono/stereo/
quad/5.1/7.1/HRTF/B-Format output selection, and — verified directly in
`alc/backends/wasapi.cpp` on its current `master`, not just in its docs — its WASAPI
backend already activates `ISpatialAudioClient`, opens a static channel bed sized from
the negotiated channel mask, and drives dynamic objects through the standard
`BeginUpdatingAudioObjects`/`EndUpdatingAudioObjects` loop on an
`AvSetMmThreadCharacteristicsW`-registered (MMCSS) thread. One dependency, one encode
stage, one place the VR listener pose and `sound-play-parms` positions get read.

## Decision
Adopt the ambisonic-bus shape, built on OpenAL Soft. Positional (3D) voices are
encoded into an ambisonic soundfield from their `sound-play-parms` world position and
the current listener pose; OpenAL Soft's own output-device selection then decodes that
soundfield to whichever of HRTF, a 5.1/7.1 channel bed, or an `ISpatialAudioClient`
object/bed stream matches the endpoint and the user's Windows Spatial Sound setting.
Non-positional music and streams (not positional today) stay
on the untouched cubeb path; nothing about the vanilla mix changes for them.

**Parity toggle.** With `spatial_audio` off, the existing pan-table plus cubeb 2-channel/
48 kHz path (`player.cpp:43-48`) is byte-for-byte untouched — no code on that path
changes, and the check is a PCM capture comparison against the noise floor from
vanilla-vs-vanilla runs. OpenAL Soft is only opened, and only receives
voices, when `spatial_audio` is on.

**VR listener.** Unchanged from the pre-promotion design: in diorama stereo, distance
stays measured from Jak (loudness independent of `world_scale`), and orientation is the
anchor yaw composed with the head orientation from the same `xrLocateViews` pose the stereo path
uses, so sounds stay fixed in the diorama as the head turns. In the experimental
first-person mode the listener is the head pose itself. This rule is unchanged by the
pipeline choice — it only changes what consumes the listener pose (an ambisonic encode
instead of a stereo pan lookup).

**Object budget.** `ISpatialAudioClient::GetMaxDynamicObjectCount` can legitimately be 0
(spatial sound not enabled on the endpoint) or as low as 20 (Dolby Atmos home theater).
Dynamic objects are a limited resource for a handful of prioritized "hero" voices;
every other positional voice always has a channel-bed fallback from the same ambisonic
encode, so a low or zero object budget degrades gracefully rather than losing
spatialization entirely.

## Consequences
- One new dependency (OpenAL Soft, LGPL 2 or later) dynamically linked; no change to
  the project's license posture beyond the existing OpenXR/Apache-2.0-class
  dependencies the project already uses.
- The spatial audio integration spike must pin the exact OpenAL Soft version/commit and re-verify the
  `ISpatialAudioClient` backend against that pin — the upstream source has a
  `// TODO: GetMaxFrameCount` comment suggesting it is still being refined, and a 2021
  feature-request issue for this exact capability is still open upstream (stale
  relative to the current source, but worth not taking on faith).
- The spatial audio integration spike must also confirm empirically that OpenAL Soft's WASAPI stream and the
  engine's existing cubeb WASAPI stream can coexist on the same endpoint at once;
  Microsoft's two-endpoint integration pattern is documented for a single renderer
  using both `IAudioClient` and `ISpatialAudioClient` itself, not for two independent
  audio libraries in one process.
- Steam Audio remains a candidate *input* stage (occlusion, reflections, reverb from
  scene geometry) layered before the ambisonic encode in a later phase; not v1 scope,
  and not the chosen output engine.
- cubeb's own multichannel layout support (5.1/7.1 `CUBEB_LAYOUT_*` constants) goes
  unused by this design; it remains available as a fallback for the plain-multichannel
  case if OpenAL Soft integration stalls, without reopening the object-output question.
- Source: `kb/findings/2026-09-23-audio-research.md`.
