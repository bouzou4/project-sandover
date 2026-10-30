---
title: Why surround was silent on the receiver, and the Audio page that fixes it
date: 2026-10-28
issue: none (play report, session through Steam streaming to a surround receiver)
status: verified (loopback and device logs); listening test on the receiver pending
sha: branch feat/audio-output-settings
---
## Answer (5 lines)
The session never ran surround. The play launcher passed `--audio-spatial binaural` by default
and the settings had no audio row, so the game opened OpenAL Soft in headphone mode: a two-channel
HRTF mix (`output stereo-hrtf, hrtf on`) on "Speakers (Steam Streaming Speakers)", which Windows
reports as 5.1 (6 channels, mask 0x3f). The centre, LFE and surround speakers got nothing.
Surround itself works on that endpoint: opened in `surround`, OpenAL Soft takes the 5.1 layout.

## The chain, as the code runs it
- `play.py` (local launcher) added `--audio-spatial binaural` unless `--audio off`. `main.cpp`
  stored it in `g_spatial_mode_request`, and `Player()` opened `SpatialOutput` in binaural mode
  as soon as the sound system started. Nothing in GOAL could change it: there was no row.
- Binaural asks OpenAL Soft for `ALC_OUTPUT_MODE_SOFT = ALC_STEREO_HRTF_SOFT`. The WASAPI backend
  then requests a 2-channel stream from the endpoint (`Requesting playback format: Channels 2`),
  and Windows upmixes nothing: two channels arrive at the receiver, filtered for headphones.
- The endpoint is right. Steam's virtual "Speakers (Steam Streaming Speakers)" device is a 5.1
  endpoint here: its shared-mode mix format is 6 channels, `dwChannelMask 0x3f` (FL FR FC LFE
  BL BR). Steam was the Windows default while streaming; on the desk the default is a stereo DAC.

## What each Output setting opens (this machine, `spatial_probe --outputs`, `ALSOFT_LOGLEVEL=3`)
Logs: `$SANDOVER_DATA/runs/audio-output-settings/outputs-*.txt`, `alsoft-outputs-*.log`.

| Output | Steam Streaming Speakers (Windows: 5.1, 0x3f) | Windows default here (stereo DAC, 0x3) |
|---|---|---|
| Auto | surround, OpenAL Soft "Post-reset: 5.1 Surround", built-in 5.1 decoder | opens stereo-basic, so the vanilla cubeb stream plays |
| Stereo | OpenAL stereo-basic (a named device) | vanilla cubeb stream |
| 5.1 | 5.1 | 5.1, which Windows downmixes to the DAC's 2 channels |
| 7.1 | requested 8 ch (mask 0x63f), Windows offered 6 (0x3f): 5.1 | 7.1, downmixed by Windows |
| Headphones | stereo-hrtf, hrtf on | stereo-hrtf, hrtf on |

Two things in that table drive the on-screen diagnostic. Forcing a layout does not always fail
on a smaller endpoint: Windows' shared mode accepted 5.1 and 7.1 on the stereo DAC and downmixes
them. And a 7.1 request on a 5.1 endpoint is cut to 5.1. So the page compares the game's channel
count with what Windows reports and says which way it is wrong.

## Speaker test: why direct channels, and the proof
OpenAL source positions go through the ambisonic decoder: a source at the left surround angle
also leaks into the neighbouring speakers, and the 7.1 decoder leaves the centre silent
(`findings/2026-09-24-spatial-audio-spike.md`). Positions cannot drive the LFE at all. So the
test is a callback buffer in the output's own channel layout (`AL_FORMAT_51CHN32`,
`AL_FORMAT_71CHN32`, ...) played with `AL_DIRECT_CHANNELS_SOFT = AL_DROP_UNMATCHED_SOFT`, with one
channel non-zero per burst. In `alu.cpp`, direct channels skip panning: `CalcDirectPanning`
writes each input channel to the `RealOut` channel of the same name (`ChannelIndex[FrontLeft]`
...), and `DROP_UNMATCHED` drops a channel the output does not have. The WASAPI backend writes
`RealOut` in the endpoint's channel order; on a 0x3f endpoint the side pair goes out as the back
pair, which is what a 5.1 receiver calls surround.

Loopback render (`spatial_probe --speaker-test`, `speaker-test.md` in the run folder): every burst
is -21 dBFS on its own channel and -200 dBFS (digital silence) on every other; the LFE burst is a
50 Hz tone at -12 dBFS. The vanilla bed (music, non-positional effects) is -16.8 dBFS on FL and
FR and silent on the other channels in 5.1 and 7.1, so the stereo mix reaches the front pair.

The vanilla stream (Stereo, or Auto on a stereo default device) has no OpenAL device; its test
adds the bursts to the cubeb stream's left and right samples.

## In game (rig instance with sound on, staged build)
Run folder `$SANDOVER_DATA/runs/audio-output-settings/session7` (screenshots, `gk.out.log`,
`audio-lines.txt`, the slot's `sandover.gc`). The slot booted with `(audio-output auto)` and
`(audio-device "OpenAL Soft on Speakers (Steam Streaming Speakers)")` in its file:

- boot: `[audio] output auto on '...Steam Streaming Speakers)'`, then `mode surround ...: output 5.1`;
- the Audio row on the Sandover page stays available with vanilla rendering on;
- page status: `DEVICE: SPEAKERS (STEAM STREAMING SPEAKERS)`, `GAME OUTPUT: 5.1, MODE SURROUND`,
  `WINDOWS SPEAKER SETUP: 5.1 (BACK)`; the Device row lists the seven active devices;
- speaker test: `speaker test 1/6: FRONT LEFT` ... `6/6: SURROUND RIGHT`, `done`, one per second,
  and `PLAYING: CENTER (3 OF 6)` on screen;
- Stereo on that device: `GAME OUTPUT: STEREO` and the warning `THE GAME SENDS STEREO ONLY`;
- 7.1 on that device: `output 5.1 ... (7.1 requested)`, no warning (Windows gives 6 channels);
- confirming 5.1 writes `(audio-output 5.1)` and the device name, quoted, to `sandover.gc`.

A second instance on OpenAL Soft's null backend (the rig's mute, run `null-backend`) shows the
hang this branch also fixes: the saved device does not exist there, `alcOpenDevice` throws and
catches inside OpenAL Soft, and with that call on GOAL's own stack the game stopped at
`Opening playback device` (two earlier runs). Device opening, listing and the Windows queries now
run on a worker thread; the same boot logs `could not open ..., using the Windows default device`
and finishes its shot.

## Review fixes
- **Worker shutdown.** The `AudioControl` worker used to be detached. At exit, the overlord's
  SHUTDOWN command calls `snd_StopSoundSystem`, which resets the Player on its own thread while
  the worker could be inside `snd_SetAudioOutput`, `snd_GetAudioStatus` or
  `snd_StartSpeakerTest`. Later, the CRT destroyed `g_audio`'s mutex and condition variable while
  the worker still waited on them. Now the worker is a joined `std::thread`.
  `snd_StopSoundSystem` first runs a hook (`snd_SetBeforeStop`) that sets a stopped flag under
  `m_lock`, wakes the worker and joins it. The worker checks the flag under `m_lock` after every
  wait, before any `snd_*` call. An `std::atexit` handler, registered when the worker starts and
  so run before every static destructor, does the same for exits that skip SHUTDOWN.
- **Command line output.** `gk --audio-spatial` no longer writes into `pc-settings`.
  `sandover-audio-update` pushes it in place of the saved row, for either profile, until the
  Output row is confirmed. The saved field keeps what the user chose. The page says so in yellow.
- **5.1 (back) endpoints.** The Steam Streaming Speakers endpoint reports `Channels 6,
  ChannelMask 0x3f` (FL FR FC LFE BL BR). OpenAL Soft's WASAPI backend runs that endpoint as its
  `DevFmtX51` with `isRear51`: the mixer's channels are FL FR FC LFE SL SR, and only the
  WASAPI mask says back. `AL_FORMAT_51CHN32` maps to the same SL/SR (`alu.cpp` `X51Map`), so
  `AL_DROP_UNMATCHED_SOFT` drops nothing, and the source stays as it was. Proof: a WASAPI
  loopback capture of that endpoint during `spatial_probe --play-test 5.1` and `--play-test auto`
  (`runs/audio-output-settings/endpoint-capture`, `capture-*.md`). Each of the 6 bursts measures
  -20.7 to -20.9 dBFS on its own channel (LFE -12.1) and -200 dBFS on the other five.
- **Cubeb test on a switch.** `OpenOutput` stops the stereo stream's test when it leaves that
  stream. Before, the test froze mid-sequence and resumed when the stream came back.
- **Warnings.** "The game sends stereo only" is red under Auto and yellow for a deliberate
  Stereo or Headphones pick. Stereo through OpenAL on a chosen device adds a note: it is
  OpenAL's stereo render of the game, not the original mix.
- **In game** (`session8`, booted with `--audio-spatial binaural` over a saved
  `(audio-output auto)` on the Steam device):
  - `Sandover audio output 4 from the command line (not saved)`, `output headphones`, and
    `AUDIO:SAVED 0 CLI 4`;
  - confirming the Output row on 5.1 logs `output 5.1` and `AUDIO:SAVED 2 CLI -1`;
  - the speaker test logs 6 bursts, FL to SR, then `done`;
  - the file holds `(audio-output 5.1)`;
  - a speaker test and an output change sent just before shutdown give `GOAL Runtime Shutdown`,
    then `[audio] control thread joined`, and gk exit code 0.

Release note: Output defaults to Auto. The old play launcher defaulted to headphones (binaural).

## Gaps
| Gap | Why it matters | Where |
|---|---|---|
| Nobody has listened on the receiver | levels prove routing to the endpoint, not what Steam's stream and the receiver do with it | user test with the speaker test |
| Steam streaming's own downmix | Steam Link clients decide what they forward; the endpoint is 5.1 here, the client may still play stereo | the speaker test shows it in one pass |
| Objects (Windows Spatial Sound) stays command-line only | OpenAL Soft reads `ALSOFT_CONF` once per process | `--audio-spatial objects` |
| The vanilla stream always plays on the Windows default device | cubeb is opened on the default; a named device runs OpenAL stereo instead | `PlanOutput` |
| Auto decides when it is applied | a new Windows default (streaming starting mid-session) keeps the old choice until the Output row changes or the game restarts | `Player::SetAudioOutput` |
