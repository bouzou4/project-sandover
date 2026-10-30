---
title: Surround routing: dialogue in the centre, music and ambience bleeds, positional width kept
date: 2026-10-29
issue: none (design decision: keep the spatialiser, build the surround routing on top)
status: verified (loopback renders per layout, in-game endpoint captures); listening on a receiver pending
sha: branch feat/audio-surround-routing
---
## Answer (5 lines)
The multichannel outputs (5.1, 7.1, quad, the Spatial output's 7.1.4 bed) now route each voice by
role, from a map resolved by speaker name when the device opens: the stream voice (dialogue,
stingers) in the centre at equal power, music on the front pair plus a 15 ms delayed bleed at -8 dB (side) / -11 dB (back),
non-positional ambience the same at -3 / -6 dB, and positional sounds keep their authored width
and the original's loudness per bearing. Stereo and headphones are bit-identical to before.

## What the code did, and what it does now
Before, `SpatialOutput` saw two kinds of voice: a voice whose sound had claimed a positional source
(`Route`, summed to mono `(L + R) / 2`), and everything else, mixed by the synth into one stereo
pair (`EndSample`) played as a direct-channel stereo source, so FL and FR only. Music, dialogue
and ambience were indistinguishable in that pair, and a positional sound's tone and LFO pans (the
authored width, `sfxgrain.cpp`) cancelled in the mono sum.

Now each voice carries a role (`Voice::Role`), set where it starts:

| Role | Set in | Means |
|---|---|---|
| Dialog | jak1 `ssound.cpp` tags the overlord's stream voice (`gVoice`) through `snd_SetDialogVoice` | every VAG stream in Jak 1: the stream voice. VAGWAD holds spool hints and other dialogue, and three stingers (`VAGDIR` lists `ST-WIN`, `ST-LOSE`, `ST-DEATH`; the game spawns `st-lose` with mode `'stinger`: `fisher.gc`, `rolling-obs.gc`, `rolling-race-ring.gc`, `sunken-pipegame.gc`). Cutscene streams were not verified |
| Music | `midi_handler.cpp` (MIDI and AME both run it) | the music banks |
| Ambient | `sfxgrain.cpp`, bank volume group 4 (`sound-group ambient`) | ambient loops |
| Effect | default | everything else |

`Synth::TickRouted` offers each voice to the positional slots first, then to `RouteRole`; only
what neither takes goes into the stereo mix. On a routed layout the bed is no longer a stereo
pair: `EndSample` builds one frame per output channel from eight inputs (front L/R = the stereo
mix plus music plus ambience, the dialog line's common part and each side's excess, music L/R
and ambience L/R delayed by 720 samples, `kBleedDelay` from `kSpatialRate`) through `BedRouting::gain`, and the bed source plays that buffer in the layout's own
format with direct channels, the same format and channel order as the speaker test
(`LayoutSpeakers`, `BufferFormat`). OpenAL Soft's direct-channel path (`CalcDirectPanning`) writes
each buffer channel to the output channel of the same name, so nothing is decoded or smeared.

`ResolveBedRouting(channels)` builds the gains by name, not by index: centre present -> the
dialog line's common part there at `sqrt 2`, each side's excess on its own front speaker
(absent: the line unchanged on the front pair); side pair -> bleeds at -8 / -3 dB; back pair
-> the same, 3 dB lower when a side pair exists (alone, as in quad, at the full bleed); back centre
-> `(L + R) / 2` 3 dB lower; LFE and the four heights -> nothing. Stereo or mono layouts and the
binaural output stay inactive, which keeps the old code path (proved bit-identical below).

Positional sounds keep their mid source exactly as before. Their side signal `(L - R) / 2` goes
to a second, stereo source per slot carrying `(+S, -S)`, not spatialised, with `AL_STEREO_ANGLES`
set every update to the sound's azimuth +-90 degrees: a stereo pair facing the listener around
the sound. The side source starts only when the slot first carries a non-zero `S` (all but 4 of
the 1048 sounds never do, census below): normally before the game frame's `Flush` starts the mid,
so both start together (`alSourcePlayv`) and `TrimFirstFill` drops the same samples from both
queues on the first pull, keeping them sample-aligned. When the width appears after the mid is
already playing (a pan grain after a delay, an LFO), `Flush` stops the mid and restarts the pair
from aligned queues, which drops the few samples OpenAL had read ahead for the mid, once per
sound. A mono-authored sound has `S = 0` and renders exactly as before, with no second source.
Position, gain and angle updates of every slot go out in one `alDeferUpdatesSOFT` /
`alProcessUpdatesSOFT` batch, so a mid and its side never change in different mixer updates.

The mid takes `VanillaPanPowerGain(azimuth)`: the overlord pan table's `L^2 + R^2` at that
bearing relative to the sides, +2.86 dB ahead and behind, so the centre is as much louder than
the sides as in the original's stereo mix (OpenAL's equal-power panning had it flat). The boost
fades to the sides' 0 dB with the elevation (`horizontal / distance`), so a sound straight above
or below the camera, which has no bearing, no longer reads `atan2(0, 0) = 0` and gets the ahead
gain. `AL_MAX_GAIN` goes to 2 on the mid; its default of 1 clamped the gain away in the first
render. The width pair stays at unity: the pan law's boost ahead belongs to the centred part
(both channels at full), while a hard-panned part already has its full pan-law power at every
bearing. With the boost on the pair too, a hard-panned tone ahead measured +1.00 dB (5.1) and
+1.02 dB (7.1) over the original's `L^2 + R^2`; with the pair at unity it is -0.19 / -0.13 dB
(proof 1).

Why `sqrt 2` for dialogue: the original plays it `L = R = v`. Two speakers at `v` carry `2 v^2`;
one centre at `v * sqrt 2` carries the same, and an ITU downmix (`L' = L + 0.707 C`) folds it back
to exactly `v` per side.

Positioned lines: the overlord pans the mono stream with `gPanTable` when a VAG is `positioned`
(`iso.cpp` `CalculateVAGVolumes`; e.g. `fisher.gc` `ambient-hint-spawn` at the fish, the
taskable NPC hints), so `L != R`. The first version split the line into mid `(L + R) / 2` (to the
centre) and side `(L - R) / 2` (`+` on FL, `-` on FR): a hard-left line (L 1, R 0.19) became
C 0.84, FL +0.405, FR -0.405, so FR played the line polarity-inverted against FL and FC on every
layout with a centre. Now `BedRouting::SplitDialog` takes the common part `c` = the side nearer
zero (L and R of a panned mono line share a sign per sample; opposite signs share nothing), sends
`sqrt 2 * c` to the centre and only `L - c`, `R - c` to FL, FR: every gain is >= 0 (the far side
of a hard-panned line is silent), and the ITU fold-back `FL + 0.707 C = L - c + c = L` is exact.

Stingers stay in the centre. `ST-WIN`, `ST-LOSE`, `ST-DEATH` are mono VAG streams on the same SPU
voice, played `L = R` under the dialogue volume (`gDialogVolume` scales every stream in
`CalculateVAGVolumes`), so they already were part of the stream mix, not the music mix. Moving
them to the music role would split one voice's role by stream name, and the `'stinger` spawn mode
does not identify them either: `fisher.gc` also spawns the dialogue line `FIS-LO03` with it. In
the centre they keep their exact stereo level and fold back exactly; only the 15 ms surround
bleed that music gets is missing for these three short cues.

The downmix trade-off: a receiver or a Windows endpoint that folds the 5.1 / 7.1 output to
stereo adds the bleed back onto the front pair 15 ms late at -8 dB (music) or -3 dB (ambience),
which comb-filters: notches every 66.7 Hz; with the ITU surround fold at 0.707, music ripples
about +2.2 / -2.9 dB in 5.1 and +3.4 / -5.7 dB in 7.1 (side and back pairs on the same side),
ambience +3.5 / -6 dB in 5.1 and up to +5.4 / -16.7 dB in 7.1.
Stereo and headphones never get the bleed; the trade-off applies only to a downmixed
multichannel output.

## The sound banks (`spatial_probe --census iso_data/jak1/SBK`)
1048 sounds: group 0 925, group 4 (ambient) 120, group 2 1, group 3 2. Of the 120 ambient sounds,
119 have a falloff curve and so are positional sources; the one non-positional ambient loop is
`white-eco-lp` (FINALBOS). The ambience bleed therefore applies to that loop and to ambient loops
beyond the 32 positional sources. Jak 1 has no streamed (VAG) ambience: the stream voice plays
VAGWAD, which is dialogue plus the three stingers (above).

Sounds with an authored pan (tone or LFO), all positional: `floating-rings` (JUNGLE, JUNGLEB:
tones at pan 270 and 90, the wide one), `rope-stretch` (BEACH: 72, 72, 300), `scrate-break`
(COMMON: 20, 20), `cavewind` (MAINCAVE: 10).

## Proof 1: loopback, every layout (`spatial_probe --routing`, `runs/audio-surround-routing/probe/routing.md`)
Synthetic voices through the real `Route` / `RouteRole` / `EndSample` path on OpenAL Soft's
loopback device; noise at -16.8 dBFS per voice channel; `before` is the same build with routing
off (= master's behaviour); 7.1.4 renders through a loopback layout added to the vendored
OpenAL Soft (`ALC_7POINT1POINT4_SANDOVER`, `DevFmtX714`), the same mixer layout the Spatial
output's stream runs. Per-channel RMS in dBFS; `total` = power over all channels.

Non-positional, after (before: everything on FL/FR, -16.8 each for dialog):

| Layout | Signal | FL | FR | FC | BL | BR | SL | SR | heights | total |
|---|---|---|---|---|---|---|---|---|---|---|
| 5.1 | dialog L = R | -200 | -200 | -13.8 | | | -200 | -200 | | -13.78 (before -13.78) |
| 7.1 | dialog | -200 | -200 | -13.8 | -200 | -200 | -200 | -200 | | -13.78 (before -13.78) |
| 7.1.4 | dialog | -200 | -200 | -13.8 | -200 | -200 | -200 | -200 | -200 | -13.78 (before -13.78) |
| 5.1 | music, pan 45 L | -16.8 | -21.3 | -200 | | | -24.8 | -29.3 | | -14.84 |
| 7.1 | music | -16.8 | -21.3 | -200 | -27.8 | -32.3 | -24.8 | -29.3 | | -14.55 |
| 7.1.4 | music | -16.8 | -21.3 | -200 | -27.8 | -32.3 | -24.8 | -29.3 | -200 | -14.55 |
| 5.1 | ambience, L/R uncorrelated | -16.8 | -16.8 | -200 | | | -19.8 | -19.8 | | -12.03 |
| 7.1 / 7.1.4 | ambience | -16.8 | -16.8 | -200 | -22.8 | -22.8 | -19.8 | -19.8 | -200 | -11.36 |

Cross-correlation against FL: SL 720 samples (15.0 ms) later at -8.00 dB (music) and -3.00 dB
(ambience), BL 15.0 ms later at -11.00 / -6.00 dB, in 5.1, 7.1 and 7.1.4. LFE -200 everywhere.

Positional, mono-authored (centre gain):

| Layout | at left before / after | at centre before / after | centre minus side, after | original (pan table) |
|---|---|---|---|---|
| 5.1 | -16.95 / -16.95 | -16.94 / -14.08 | +2.87 dB | +2.86 dB |
| 7.1 | -16.81 / -16.81 | -16.81 / -13.95 | +2.86 dB | +2.86 dB |
| 7.1.4 | -20.00 / -20.00 | -20.03 / -17.17 | +2.83 dB | +2.86 dB |

Against the original's absolute level (one voice at -16.8 dBFS: stereo total +0.15 dB at a side,
+3.01 dB ahead), 5.1 now sits 0.3 dB under it at both bearings.

Positioned dialogue, a hard-left line (L 1.0, R 0.19), per-channel correlation with the line
and the ITU fold-back against the stereo line:

| Layout | FL | FR | FC | correlation | fold-back error |
|---|---|---|---|---|---|
| 5.1 / 7.1 / 7.1.4, first version | -24.6 | -24.6 | -18.3 | FL +1, FR **-1**, FC +1 | 73.7 dB under the signal |
| 5.1 / 7.1 / 7.1.4, now | -18.6 | -200 | -28.3 | FL +1, FR silent, FC +1 | 73.7 dB under the signal |

Total -18.15 dB against -16.64 in stereo: the excess and the centre are the same signal, so the
discrete channels carry 1.5 dB less power for this line while the downmix stays exact; an
`L = R` line is power-exact (the rows above). `AudioRouting.
PositionedDialogNeverInvertedAndFoldsBack` fails on the first version's split (FR gain -0.406)
and passes now.

Positional, authored width: voice A panned 45 L (L 1.0, R 0.59) alone at the centre, FL minus FR:
before 0.0 dB (mono sum) in every layout; after +2.8 dB (5.1), +1.8 dB (7.1), +4.1 dB (7.1.4)
with the pair at unity (+3.9 / +2.6 / +5.1 with the bearing gain on it); the original gives
+4.6 dB. A hard-panned tone (pan 270) ahead against the original's `L^2 + R^2` at the same
bearing (noise power times the pan table):

| Layout | mono ahead | hard-panned ahead, pair boosted | hard-panned ahead, pair at unity |
|---|---|---|---|
| 5.1 | -0.30 dB | +1.00 dB | -0.19 dB |
| 7.1 | -0.17 dB | +1.02 dB | -0.13 dB |
| 7.1.4 | -3.39 dB | -1.44 dB | -2.81 dB |

The 7.1.4 loopback layout renders every positional source about 3 dB lower (the mono row too);
relative to it the hard-panned tone sits +0.58 dB. The same tone whose width starts 0.1 s after
the mid is playing (the restart path) gives the same per-channel levels as the immediate one
within 0.1 dB. A mono sound straight above the camera renders as before (5.1 total -20.08 dB
before and after); with `atan2(0, 0)` it had taken the +2.86 dB ahead gain. Voice B (45 R) mirrors it. With both voices at a side the width spreads
front and back of the sound (the stereo pair faces the listener): 5.1 at left, FR -40.3 -> -31.0,
SR -57.3 -> -31.6.

Binaural, every scene: after vs before maximum sample difference 0. Master's `spatial_probe`
and this branch's render the default scene's `binaural.wav` byte-identical (SHA-256
`52eeda6d...7520f9`), and the speaker test WAVs (stereo-hrtf, 5.1, 7.1, test and bed) are
byte-identical too. Stereo (surround on a 2-channel device) is unchanged in every row.

## Proof 2: in game, Steam Streaming Speakers (`runs/audio-surround-routing/session-after`, `session-before`)
Jungle start, rig shot held, one gk per run; WASAPI loopback of the endpoint, which Windows now
reports as 7.1 (8 ch, mask 0x63f; it was 5.1 on 2026-10-28), so a 5.1 output arrives on
FL FR FC LFE BL BR. `before` = master's binaries, same steps. Volumes isolate each role: dialogue
`spool-sagevb01` (dialog 75, others 0), the jungle music (music 40), `floating-rings` 4 m from
the camera with the game paused so nothing else plays.

Dialogue, energy over the first 0.45 s after the line's onset (the stream stops at different
points from run to run, before and after alike, so the full-line energy is not comparable):

| Run | Output | FL | FR | FC | other | total |
|---|---|---|---|---|---|---|
| after | stereo | 20.5 | 20.5 | -200 | -200 | 23.53 |
| after | 5.1 | -200 | -200 | 23.5 | -200 | 23.53 (+0.00 vs stereo) |
| after | 7.1 | -200 | -200 | 23.5 | -200 | 23.53 (+0.00) |
| before | 5.1 / 7.1 | 20.5 | 20.5 | -200 | -200 | 23.53 |

Music (RMS dBFS over 6 s): 7.1 FL -32.5, FR -32.6, SL -40.5, SR -40.6, BL -43.5, BR -43.6; the
cross-correlation puts SL -8.00 dB and BL -11.00 dB under FL, both 720 samples (15.0 ms) later.
5.1: FL -33.3, FR -33.0, the bleed on BL/BR (-41.3, -41.0) at -8.00 dB, 15.0 ms. Before: music on
FL/FR only (-200 elsewhere).

`floating-rings` (tones at pan 270 and 90), RMS dBFS; placements 4 m along the camera matrix's
axes (the run's `stereo` row shows the sound system's listener yaw sits about 20 degrees off that
matrix in this shot, before and after alike, so "ahead" leans right):

| Output | Placement | FL | FR | FC | BL | BR | SL | SR | total |
|---|---|---|---|---|---|---|---|---|---|
| 5.1 before | camera-right | -77.0 | -57.0 | -72.3 | -83.4 | -53.6 | | | -51.94 |
| 5.1 after | camera-right | -59.6 | -55.1 | -55.8 | -55.8 | -51.7 | | | -47.90 |
| 5.1 before | ahead | -63.8 | -59.7 | -58.7 | -84.6 | -58.3 | | | -53.66 |
| 5.1 after | ahead | -55.9 | -54.8 | -54.4 | -54.0 | -53.1 | | | -47.37 |
| 5.1 before | camera-left | -59.4 | -65.3 | -59.7 | -57.4 | -82.0 | | | -53.61 |
| 5.1 after | camera-left | -55.0 | -56.3 | -55.0 | -52.7 | -54.2 | | | -47.51 |
| 7.1 before | camera-right | -69.0 | -64.1 | -200 | -69.0 | -63.9 | -69.6 | -52.9 | -51.97 |
| 7.1 after | camera-right | -56.6 | -56.1 | -200 | -56.5 | -56.0 | -69.5 | -52.7 | -48.28 |
| 7.1 before | ahead | -58.4 | -58.4 | -200 | -73.1 | -68.6 | -71.1 | -57.3 | -53.01 |
| 7.1 after | ahead | -53.9 | -53.9 | -200 | -58.6 | -58.0 | -56.2 | -53.9 | -47.55 |
| 7.1 before | camera-left | -59.1 | -59.6 | -200 | -67.7 | -72.7 | -56.6 | -71.3 | -53.14 |
| 7.1 after | camera-left | -54.6 | -54.8 | -200 | -58.1 | -58.1 | -53.3 | -56.6 | -47.77 |

The near-side channel keeps its level (7.1 camera-right SR -52.9 -> -52.7); the hard-panned tones
now also reach the channels either side of the sound's direction, which the mono sum had
cancelled. The total rises 4 to 6 dB: the mono sum of two hard-panned tones kept
`2 * 0.595^2 = 0.71` of their power, 4.7 dB under what the original's stereo mix gives them; the
side pair returns `2 * 2 * 0.405^2 = 0.66`, and ahead the +2.86 dB bearing gain adds the rest.

Headphones (before / after, same steps): music -30.71 / -30.77 dBFS, rings -52.61 / -52.67,
-54.56 / -54.73, -54.40 / -54.32: within the timing noise of a one-shot capture; the loopback
render above is the exact proof.

## Proof 3: in game, Spatial on the default DAC (`runs/audio-surround-routing/session-spatial`)
Saved Spatial, Dolby Atmos for Headphones, game volumes at 20 %: `[spatial] routing (12 ch): ...
FC: dialog mid +3.0 dB ... SL: music L -8.0 dB ... TFL: - ...` and `output 7.1.4 bed (Windows
Spatial Sound stream), format Dolby Atmos for Headphones`. The endpoint carries Atmos's binaural
render, so only the ears are measurable: the dialogue line is 13.4 / 13.4 dB on L / R (centred);
the per-channel bed levels are the 7.1.4 rows of proof 1, the same mixer layout.

## No LFE
Nothing is sent to the LFE: the original has no low-frequency send, and a receiver's bass
management already routes each channel's low end to the subwoofer. An explicit send would add
energy the original never produced (research note, section 4).

## Gaps
| Gap | Why it matters | Where |
|---|---|---|
| Nobody has listened on a receiver | the levels prove the routing, not the image in a room | user test with 5.1 / 7.1 |
| The ambience bleed has one non-positional loop to act on | Jak 1's ambience is positional by design; the rule matters for overflow and the final boss | census above |
| Positional width faces the listener | a wide sound to the side spreads front and back of itself, not left and right | `Flush`, `AL_STEREO_ANGLES` |
| Listener yaw vs camera matrix in the rig shot | the sound system's yaw comes from `camera-angle`; the placements came from the camera matrix | proof 2 note |
| Cutscene streams not verified | only spool hints were captured in game; cutscene audio may reach the stream voice differently | in-game capture of one cutscene line |
| Proof 2 and 3 predate the review fixes | the in-game `floating-rings` rows had the bearing gain on the width pair (ahead ~1 dB hotter than now); dialogue there was `L = R`, unchanged by the split | loopback proof 1 is current |
| Downmixed multichannel output | the 15 ms bleed comb-filters when a receiver folds 5.1 / 7.1 to stereo | trade-off above |
