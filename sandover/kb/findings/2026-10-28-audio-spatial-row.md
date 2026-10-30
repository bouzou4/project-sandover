---
title: Dolby Atmos, Windows Sonic and DTS:X from the Audio page (the Spatial output)
date: 2026-10-28
issue: none (play request: Atmos reachable from the Audio page)
status: verified (logs, page captures, endpoint loopback); listening on an Atmos receiver pending
sha: branch feat/audio-spatial-row
---
## Answer (5 lines)
Spatial is now an Output entry, saved like the others. It cannot switch live: OpenAL Soft reads
its config, including the WASAPI `spatial-api` switch, once per process on its first call. So gk
reads the saved row from `sandover.gc` in `main.cpp`, before the sound system starts, and sets the
objects config there; a pick during a session is saved and the page says to restart. The page now
names the spatial format Windows has enabled on the device, and the speaker test plays the 7.1.4 bed's four height channels.

## Why it is a restart, as the code runs it
- `sandover-alsoft-objects.ini` holds `channels = surround714` and `[wasapi] spatial-api = true`.
  `ConfigureForObjects` points `ALSOFT_CONF` at it. OpenAL Soft reads the file on its first
  `alc*` call and never again in that process.
- With the switch on, `WasapiPlayback::resetProxy` calls `initSpatial`: it activates
  `ISpatialAudioClient` on the endpoint, opens an `ISpatialAudioObjectRenderStream` with a static
  object mask for 7.1.4 (`ChannelMask_X714`), and the mixer's channels (`RealOut`, 12 of them in
  WFX order: FL FR FC LFE BL BR SL SR TFL TFR TBL TBR) are written one per static object.
  Windows hands the objects to the format (Sonic, Atmos, DTS), which renders them for the
  endpoint. OpenAL Soft logs `Post-reset: 7.1.4 Surround` and reports the output mode as
  `ALC_ANY_SOFT`.
- The same switch applies to every device the process opens afterwards, so leaving Spatial is a
  restart too: 5.1 or Headphones opened through it would still go through the spatial stream
  (HRTF output would be virtualised twice).
- So the rule is per process, in one pure function, `snd::PlanOutputRequest(configured, first,
  requested, cli)` (unit-tested case by case): in a process set up for objects every request runs
  the spatial stream on the requested device (`Keep`, restart pending, for any other output); in
  any other process a Spatial request keeps the running output's plan and reopens it on the
  requested device, so a Device pick while Spatial waits still moves the sound, and the page's
  device lines describe the device the restart will use.
- The first request of a session is the exception (`Fallback`): a saved Spatial on a device
  without a format was not set up at start and runs Auto's plan (`PlanOutput(Auto, device)`), so
  a stereo DAC keeps the vanilla stereo stream and a 5.1 endpoint gets surround.
- `RESTART THE GAME TO APPLY THIS OUTPUT` shows only when a restart would apply the request
  (`snd::RestartPending`): a Spatial request on an endpoint with no format (dynamic-object budget
  <= 0, also `--audio-spatial objects` there) shows only the yellow enable note, since a restart
  alone changes nothing.

## Reading the saved row at start
`main.cpp`, after logging starts and before the runtime: when `--audio-spatial` was not given,
the profile file gk will run is read with `snd::ParseSavedAudio`: `sandover.gc`, or
`sandover-vr.gc` under `--xr` (`snd::SavedAudioFileName`; the game switches to the VR profile for
the XR session), and the log names the file and the saved output. The two keys are read exactly
as `sandover-settings-write` prints them: a bare word, and a quoted device name (an unquoted
device word reads as the Windows default, as `sandover-read-quoted` does). `ConfigureForObjects`
checks that `sandover-alsoft-objects.ini` is next to gk before it claims the config, and logs
when it replaces an `ALSOFT_CONF` the user set. `spatial` calls `ConfigureObjectsAtStart(device)`: the endpoint's dynamic-object budget
(`ISpatialAudioClient::GetMaxDynamicObjectCount`) must be above 0, otherwise no config is set.
Nothing in that path touches OpenAL, so the config is in place for its first call (the GOAL push
`pc-set-audio-output 5` then opens the stream).

## Which format Windows has enabled
`QueryEndpoint` now also asks `Windows.Media.Audio.SpatialAudioDeviceConfiguration` for the
endpoint (`GetForDeviceId` with the endpoint's device interface path,
`\\?\SWD#MMDEVAPI#<IMMDevice id>#{e6327cad-dcec-4949-ae8a-991e976a79d2}`) and reads
`ActiveSpatialAudioFormat`, a subtype GUID string compared with `SpatialAudioFormatSubtype`'s
(Windows Sonic, Dolby Atmos for Headphones / home theater / built-in speakers, DTS Headphone:X,
DTS:X Ultra, DTS:X for home theater). The interfaces are declared locally with the SDK's IIDs and
vtable order; `RoGetActivationFactory` and the HSTRING functions come from combase at run time.
"Off" reads as the null GUID, not an empty string.

`spatial_probe --list` on the test machine (`$SANDOVER_DATA/runs/audio-spatial-row/list.txt`):

| Endpoint | Windows layout | Spatial format | Dynamic objects | Native static mask |
|---|---|---|---|---|
| Windows default (a stereo USB DAC on S/PDIF) | stereo (0x3) | Dolby Atmos for Headphones (`{1459AC38-3875-49BF-BB59-0FE80F4D395D}`) | 128 | 0xffffe (7.1.4 with heights) |
| Speakers (Steam Streaming Speakers) | 5.1 back (0x3f) | off (null GUID) | 0 | 0xffffe |
| the five other render endpoints | stereo or mono | off | 0 | 0xffffe |

Windows reports the native static mask whether or not a format is on, so heights count only with
a budget above 0 (the speaker test checks the mask on a running spatial stream).

## The heights in the speaker test
Direct channels map each buffer channel to the output channel of the same name
(`CalcDirectPanning`, `RealOut.ChannelIndex[...]`), but OpenAL Soft has no buffer format with top
channels: the widest is 7.1. A positional source cannot stand in: the 7.1.4 decoder
(`X714Config`) is first order, so a source above front-left feeds all ten speakers. The vendored
OpenAL Soft gains one format, `AL_FORMAT_714CHN_FLOAT32_SANDOVER` (`FmtX714`, 12 channels: the 7.1
map, then TopFrontLeft, TopFrontRight, TopBackLeft, TopBackRight). It is for direct channels only
(24 lines in `storage_formats`, `buffer.cpp`, `alu.cpp`, `convolution.cpp`, `inprogext.h`; the
define has its own guard there and gk includes that header rather than repeating the value). The
test buffer uses it when the output is the spatial stream and the endpoint's native mask has the
four tops; the sequence is the 7.1 order, then `TOP FRONT LEFT`, `TOP FRONT RIGHT`,
`TOP BACK LEFT`, `TOP BACK RIGHT`.

## Proof (`$SANDOVER_DATA/runs/audio-spatial-row`)
- Start in Spatial (`session-a2`, slot seeded with `(audio-output spatial)` on the Windows
  default): `[audio] saved output spatial on the Windows default device: Windows Spatial Sound set
  up for this process`, then `[spatial] mode objects on 'OpenAL Soft on <default>': output 7.1.4
  bed (Windows Spatial Sound stream), format Dolby Atmos for Headphones, native static mask
  0xffffe, heights yes, hrtf off, 48000 Hz`; OpenAL Soft: `Post-reset: 7.1.4 Surround`. Page:
  `GAME OUTPUT: 7.1.4 BED (SPATIAL), MODE OBJECTS`, `WINDOWS SPATIAL SOUND: DOLBY ATMOS FOR
  HEADPHONES`. `AUDIO:STATE mode 3 objects-process 1 restart 0`.
- Speaker test there: 12 bursts logged in order, `PLAYING: TOP FRONT LEFT (9 OF 12)` on screen.
  A WASAPI loopback of the endpoint (the Atmos for Headphones render, binaural stereo; bursts at
  -40 dB with `SANDOVER_SPEAKER_TEST_GAIN_DB`, game volumes at 0) has every burst, the heights
  included, between -63 and -72 dBFS per ear and the gaps at -114 to -116 (`bursts.md`): left
  sources louder left by 7 to 8.6 dB, right ones mirrored, centre and LFE equal. The heights render
  differently from the floor channels at the same side: 4-8 kHz, TOP FRONT LEFT is 11/-1 dB
  (near/far ear, relative) against 5/-11 for FRONT LEFT and BACK LEFT (`bands.md`). A burst on a
  channel the output does not have would be dropped (`DROP_UNMATCHED`) and silent. The same
  capture through `spatial_probe --play-test objects` matches to 0.1 dB (`capture-default`).
- Restart note, spatial process: picking 5.1 logs `5.1 applies when the game starts again; the
  spatial stream keeps running`, state `restart 1`, the page shows the note and still
  `MODE OBJECTS` (`a3-spatial-process-51-picked.png`).
- Restart note, other process (`session-b2`, Auto on Steam Streaming Speakers): the row hint while
  picking (`b2`), then `spatial applies when the game starts again; the running output stays`,
  `restart 1`, the note, `WINDOWS SPATIAL SOUND: OFF` and the yellow `ENABLE DOLBY ATMOS OR WINDOWS
  SONIC ...` (`b3`); the slot's `sandover.gc` holds `(audio-output spatial)` after the confirm.
- Saved Spatial on a device without a format (`session-c2`, Steam): `endpoint dynamic-object
  budget 0`, `not set up (no spatial format on the device); surround until a restart`, `mode
  surround ... output 5.1`, page with the restart and enable notes.
- Tests: `goalc-test --gtest_filter=AudioSettings.*` (the start reader on the writer's form, every
  output, device names, unknown and missing keys, the heights mask) and `tests/test_settings.py`,
  `tests/test_settings_forms.py` (the Python mirror's round trip, both profiles, the GOAL reader and
  writer names, the push of output 5).

## Gaps
| Gap | Why it matters | Where |
|---|---|---|
| Nobody has listened to Atmos on a receiver | the levels prove the bed reaches the format, not what a receiver does with it | user test: Spatial on a direct HDMI endpoint with Atmos for home theater, then the speaker test |
| Steam streaming carries PCM only | Spatial there falls back to surround; no format can be enabled on that endpoint | howto: use a direct HDMI endpoint |
| A spatial process opens every device through the spatial config | a Device pick to an endpoint without a format then runs OpenAL Soft's plain WASAPI path with a 7.1.4 request | the page shows `OFF` and the enable note |
