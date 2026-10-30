# Spatial audio how-to (Jak 1)

Positional sound effects through OpenAL Soft, ADR-0017. Jak 1 only; Windows only for now.
State of the spike: `kb/findings/2026-09-24-spatial-audio-spike.md`; the Audio page and why
surround was silent on a receiver: `kb/findings/2026-10-28-audio-output-settings.md`.

## The Audio page (Options > Sandover > Audio)

| Row | Values | What it opens |
|---|---|---|
| Output | Auto (default), Stereo, 5.1 surround, 7.1 surround, Headphones (binaural), Spatial (Atmos / Sonic / DTS) | Auto: `surround` at the layout Windows reports for the device; on a stereo Windows default device the vanilla stereo stream instead, while a chosen (non-default) stereo device gets OpenAL Soft's stereo render of the game, not the original mix. Stereo: the vanilla stream (OpenAL stereo when a device is picked). 5.1 / 7.1: `surround` with that layout forced. Headphones: `binaural`. Spatial: `objects`, OpenAL Soft's Windows Spatial Sound stream, from the next game start (below) |
| Device | Windows default, then each device OpenAL Soft lists (`ALC_ENUMERATE_ALL_EXT`) | the device for every OpenAL output; a saved device that cannot be opened falls back to the default (logged) |
| Speaker test | button | one 0.7 s burst per channel of the running output, in turn |

Under the rows the page prints what runs: the device, the game's output layout and mode
(`GAME OUTPUT: 5.1, MODE SURROUND`), the layout Windows reports for that device
(`WINDOWS SPEAKER SETUP: 5.1 (BACK)`) and the spatial format Windows has enabled on it
(`WINDOWS SPATIAL SOUND: DOLBY ATMOS FOR HEADPHONES`, or `OFF`). Two warnings:

- red: the game sends more channels than Windows gives the device (Windows downmixes): set the
  speaker layout in Windows (Sound settings, the device, Configure speakers);
- the game sends stereo while the device is surround: pick 5.1 or 7.1. Red under Auto, yellow
  when Stereo or Headphones was picked on purpose.

Two notes: a chosen stereo device runs OpenAL Soft's stereo render, not the original mix (the
original mix plays only on the Windows default device); and, in yellow, a session started with
`--audio-spatial` runs that output until the Output row is confirmed (it is never saved).

Speaker test: front left, front right, centre, subwoofer (LFE), surround left and right, then
back left and right on 7.1. Mains get band-limited noise, the LFE a 50 Hz tone. The page shows
`PLAYING: CENTER (3 OF 6)` while it runs, and the log has the same lines in order:

```
[audio] speaker test start: 6 bursts; device 'OpenAL Soft on Speakers (Steam Streaming Speakers)', layout 5.1, mode surround, Windows reports 5.1 (back)
[audio] speaker test 1/6: FRONT LEFT (output channel 0)
...
[audio] speaker test done
```

The bursts are written straight to the named output channel (OpenAL Soft direct channels), not
placed in space, so the speaker that plays is the one the receiver gets on that channel. A
burst from the wrong speaker is a wiring or receiver setting; a centre or surround burst from
the front pair means something downstream (Windows, Steam streaming, the receiver) downmixes.

Auto decides when it is applied: if the Windows default device changes during a session (Steam
streaming starting), change the Output row once (or restart) to have it look again. Device
opening, the device list and the Windows queries run on a thread of their own (`AudioControl`,
`sandover/bridge_audio.cpp`), never on GOAL's stack, where an exception inside OpenAL Soft hung the game.

Audio is not rendering: vanilla rendering leaves the page available.

Saved in `sandover.gc` (and `sandover-vr.gc`) as `(audio-output auto)` (auto, stereo, 5.1, 7.1,
headphones, spatial) and `(audio-device "OpenAL Soft on ...")` (`""` is the Windows default). The
game pushes them with `(pc-set-audio-output <0-5> "<device>")`; the engine reopens the device only
when one of them changes.

### Spatial (Dolby Atmos, Windows Sonic, DTS:X)

Spatial runs `objects`: OpenAL Soft's Windows Spatial Sound stream, a static 7.1.4 bed handed to
the spatial format Windows has enabled on the device, which renders it (binaural for Sonic and
the headphone formats, the receiver's speakers for Atmos or DTS:X for home theater). Positional
sounds are decoded into that bed, so the heights carry what is above the camera.

It is a property of the process. OpenAL Soft reads its config (`ALSOFT_CONF`, the
`[wasapi] spatial-api` switch) once, on its first call, so:

- gk reads the saved `audio-output` and `audio-device` in `main.cpp`, before the sound system
  starts, from the profile it runs: `sandover-vr.gc` under `--xr`, `sandover.gc` otherwise (the
  log names the file). When the value
  is `spatial` and that device's endpoint has a spatial format (dynamic-object budget above 0), it
  sets the objects config and logs
  `[audio] saved output spatial on ...: Windows Spatial Sound set up for this process`.
- Picking Spatial during a session saves it and changes nothing until the game starts again. The
  row's hint says so while it is being picked (`SPATIAL TAKES EFFECT THE NEXT TIME THE GAME
  STARTS`), and once picked the page shows `RESTART THE GAME TO APPLY THIS OUTPUT` in yellow while
  the row differs from what the process runs. The running output stays, and a Device pick
  reopens it on the new device.
- In a process started in Spatial, any other Output pick is saved and shows the same restart note;
  the spatial stream keeps running (a Device pick reopens the spatial stream on that device).
- Saved Spatial on a device without a format: the start log says
  `not set up (no spatial format on the device); surround until a restart`, the session runs
  what Auto would (the vanilla stereo stream on a stereo default device, surround otherwise), and
  the page shows `WINDOWS SPATIAL SOUND: OFF` and, in yellow (no restart note: a restart alone
  changes nothing), `ENABLE DOLBY ATMOS OR WINDOWS SONIC FOR THIS DEVICE IN WINDOWS SOUND
  SETTINGS`. Enable the format, then restart.
- `gk --audio-spatial <mode>` still overrides the saved row for the session; given, gk does not
  read the saved value at start. `--audio-spatial objects` keeps holding the rows (greyed, with
  the reason on the page).
- A `--xr` start whose XR session fails runs flat with the VR profile's row applied at start, so
  the restart note can show until the next `--xr` start.

Where the format comes from: `Windows.Media.Audio.SpatialAudioDeviceConfiguration` for the
endpoint (`ActiveSpatialAudioFormat`, compared with `SpatialAudioFormatSubtype`), queried in
`QueryEndpoint` next to the mix format, through combase at run time (no C++/WinRT, no extra
link library). "Off" reads as the null GUID. The dynamic-object budget and native static mask come
from `ISpatialAudioClient` on the same endpoint.

What the formats need: Dolby Atmos for home theater and DTS:X for home theater need a direct
HDMI (or eARC) endpoint to a receiver that decodes them, with the format enabled for that endpoint
in Windows Sound settings (and the Dolby Access or DTS Sound Unbound app). Steam streaming
(Remote Play, Steam Link) carries PCM only: its "Speakers (Steam Streaming Speakers)" endpoint has
no spatial format (`OFF` on the machine it was tested on), so Spatial there runs surround. Windows
Sonic and the headphone formats render binaural stereo on any endpoint, including a stereo DAC.

Speaker test in Spatial: the bed's 7.1 (front left, front right, centre, LFE, surround left and
right, back left and right), then, when the format renders heights (the endpoint's native static
mask has the four top channels), `TOP FRONT LEFT`, `TOP FRONT RIGHT`, `TOP BACK LEFT`,
`TOP BACK RIGHT`: 12 bursts. The heights need a 7.1.4 buffer format, which OpenAL Soft does not
have: the vendored copy adds one (`AL_FORMAT_714CHN_FLOAT32_SANDOVER`, `FmtX714`: the 7.1 in WFX
order, then the four tops) for direct channels only, so each burst lands on the bed channel of
the same name. `SANDOVER_SPEAKER_TEST_GAIN_DB` (test hook) lowers the bursts for loopback proofs
on a device someone may be listening to.

## Turn it on from the command line or the REPL

- Command line: `gk --audio-spatial <mode> ...`. Given, it overrides the Output row in the engine
  for that session (off: Stereo, binaural: Headphones, surround: Auto; objects holds the page)
  until the Output row is confirmed; the saved row is never changed by it, in either profile.
  Not given, the saved row applies. The play launcher passes it only with `--audio`.
- GOAL: `(pc-set-audio-spatial "<mode>")`, or `(pc-set-audio-output <output> "<device>")`. A REPL
  form stays until the menu row changes. goalc needs the declarations from `kernel-defs.gc`; on
  an older `out/`, declare them by hand first:
  `(define-extern pc-set-audio-spatial (function string none))`.

| Mode | What it does |
|---|---|
| `off` | The vanilla cubeb stereo stream; OpenAL is never loaded or opened. |
| `binaural` | HRTF over the default device (OpenAL Soft's built-in dataset). For headphones. |
| `surround` | Decodes to the default device's speaker layout as Windows reports it (5.1, 7.1, ...). A stereo endpoint gets a plain stereo pan. |
| `objects` | OpenAL Soft's Windows Spatial Sound stream: a static bed (up to 7.1.4) handed to the enabled spatial format (Windows Sonic, Dolby Atmos, DTS). Falls back to `surround` when the endpoint has no spatial format. The Output row's Spatial entry. |

`objects` sets `ALSOFT_CONF` to `sandover-alsoft-objects.ini` (next to `gk`), which OpenAL
Soft reads once per process. Save Spatial on the Audio page (applied at the next start) or start
with `--audio-spatial objects`; switching into it later in the same process keeps whatever OpenAL
was first configured with (a warning is logged). The `[spatial]` line names the stream, the
bed, the format and whether the heights are tested:
`mode objects on '...': output 7.1.4 bed (Windows Spatial Sound stream), format Dolby Atmos for
Headphones, native static mask 0xffffe, heights yes, hrtf off, 48000 Hz`.

The log has a `[spatial]` line per switch naming the device, output mode, HRTF state and
rate. OpenAL Soft's own log: set `ALSOFT_LOGLEVEL=3` and `ALSOFT_LOGFILE=<file>`.

## What moves where

- Sounds started through the overlord with a falloff curve (`fo_curve != 0`) get an OpenAL
  source at their `sound-play-parms` position, updated every frame with the listener (the
  sound's own pan is forced to centre).
- The listener is the camera position and yaw (the vanilla pan's reference); loudness
  still comes from the game's falloff against Jak's position (OpenAL's distance model is off).
- While a mode is on, OpenAL's mixer clocks the synth and the cubeb stream is stopped.

Headphones (binaural) and any stereo output keep the vanilla stereo mix on the front pair and
the positional voices summed to mono, as before. The multichannel outputs (5.1, 7.1, quad, and
the Spatial output's 7.1.4 bed) route each voice by what it carries:

| What | How it is found | Where it goes |
|---|---|---|
| Stream voice (dialogue, stingers) | the overlord's VAG stream voice (jak1 `ssound.cpp` tags it at start): spool hints and other dialogue, and the stingers `ST-WIN`, `ST-LOSE`, `ST-DEATH` (mono, under the dialogue volume, kept here on purpose); cutscene streams not verified | the centre at `sqrt 2` of the part common to L and R (`L = R = v` becomes `C = v * sqrt 2`: the same total power, and what an ITU downmix folds back to `v` on each side). A positioned line (`L != R`, the overlord's pan) keeps only the louder side's excess on its front speaker, so no speaker plays it inverted and the downmix gives back L and R exactly. Without a centre (quad): the line unchanged on the front pair, as on stereo |
| Music (MIDI / AME) | voices started by the MIDI handler | the front pair at its authored pan, plus the same signal 15 ms later on the surround pairs: the side pair (5.1's surround) at -8 dB, the back pair 3 dB lower (-11 dB) when a side pair exists; quad's back pair at -8 dB; 6.1's back centre takes `(L + R) / 2` at -11 dB |
| Ambience that is not positional | bank volume group 4 (`sound-group ambient`), not on a positional source: the one `fo_curve 0` ambient loop (`white-eco-lp`, final boss) and ambient loops beyond the 32 sources | front pair as authored, plus the 15 ms delayed copy at -3 dB on the side pair and -6 dB on the back pair |
| Other non-positional effects | everything else | the front pair, as authored |
| Positional sounds | a source per sound | mid `(L + R) / 2` at the sound's direction, as before; the authored width `(L - R) / 2` (tone and LFO pans) as a stereo pair 90 degrees either side of that direction (`AL_STEREO_ANGLES`), `+S` on the left, `-S` on the right; a mono-authored sound has no width, no second source, and renders as before. The mid carries the vanilla pan law's loudness for the bearing: ahead and behind +2.9 dB over the sides (`VanillaPanPowerGain`), fading to 0 dB toward straight above or below; the width pair stays at unity |

The map is resolved by speaker name when the device opens, from the layout the speaker test
plays in (`LayoutSpeakers`, `ResolveBedRouting`): a layout OpenAL Soft reports gets its routing
without code. The `[spatial] routing (N ch): ...` log line prints every channel's inputs.

Nothing goes to the LFE or the height channels: no LFE send (the original has none, and a
receiver's bass management already moves the low end of every channel to the subwoofer), and the
heights carry only what the positional decoder places above the camera. There is no setting:
this is what 5.1, 7.1 and Spatial mean; stereo and headphones are unchanged.

Proof on loopback: `spatial_probe --routing <dir>` (`routing.md`, one table per layout, before
and after); `spatial_probe --census <iso_data/jak1/SBK>` lists the volume groups, the
non-positional ambience and the sounds with an authored pan.

## Windows Spatial Sound (needed for Spatial / `objects` only)

1. Settings → System → Sound → the output device → **Spatial audio**: pick Windows Sonic
   for Headphones (free), or Dolby Access / DTS Sound Unbound (need their apps and, for
   Atmos/DTS:X, a licence). Atmos or DTS:X for home theater: a direct HDMI endpoint to the
   receiver; Steam streaming carries PCM only.
2. Check: `spatial_probe --list` prints each endpoint's format, dynamic-object budget and native
   static mask (`--endpoint`: the default endpoint's budget and mask). `gk` logs the budget when
   `objects` starts, and the Audio page shows the format.
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
spatial_probe --list           # Windows' layout and spatial format for each OpenAL Soft device
spatial_probe --saved <file>   # the audio keys of a sandover.gc as gk reads them at start
spatial_probe --outputs ["OpenAL Soft on <name>"]   # what each Output row opens there (silent)
spatial_probe --play-test 5.1 ["OpenAL Soft on <name>"]  # the speaker test, audible, no game
spatial_probe --speaker-test <out_dir>  # the test and the bed on loopback: speaker-test.md + WAVs
spatial_probe --routing <out_dir>  # each role per layout (stereo, binaural, 5.1, 7.1, 7.1.4), before/after
spatial_probe --census <SBK dir>   # sound banks: volume groups, falloff, authored pans
```

With `ALSOFT_LOGLEVEL=3 ALSOFT_LOGFILE=<file>` OpenAL Soft logs the endpoint's mix format, the
format it requested and the one it got (`Device mix format`, `Requesting playback format`,
`Post-reset: 5.1 Surround`).

`levels.md` has per-channel RMS per position, binaural interaural level and time
differences, and the mixer cost with 0, 8 and 32 moving sources. The WAVs play the
positions in order (0.75 s each, 0.25 s gaps).

## Listening checks (human)

- Headphones: Output Headphones; a positional sound (a waterfall, a precursor door, an eco
  vent) should stay put in the world as the camera turns, including behind.
- Surround receiver: Output Auto (or 5.1 / 7.1), then the speaker test: each speaker named on
  screen should play once, in order. If the page warns that Windows runs the device as stereo,
  set the speaker layout in Windows first. Steam streaming: the device is "Speakers (Steam
  Streaming Speakers)", a 5.1 (back) endpoint (6 channels, mask 0x3f) on the machine it was
  tested on; a WASAPI loopback capture of that endpoint has each burst on its own channel.
- Spatial: enable a spatial format for the device (above), pick Output Spatial, restart the
  game, then the speaker test: 12 bursts with the heights where the format renders them. On a
  headphone format (Atmos for Headphones, Sonic) the tops should sound above the matching
  floor speaker.
