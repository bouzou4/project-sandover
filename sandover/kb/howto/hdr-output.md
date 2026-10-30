# HDR output how-to (Jak 1, Windows)

The game can present to an HDR display: the FP16 scene is mapped to nits with a toon-safe
curve and written to a DXGI swapchain in HDR10/PQ (default) or scRGB. Off by default; off, the
window presents exactly as before. Desktop only: there is no HDR in VR.

## Turn it on

```lisp
(pc-set-hdr-output 0 1000000)   ; on (turns the FP16 target on with it)
(pc-set-hdr-output 0 0)         ; off
```

Play script: `uv run python ../local/play.py` (HDR10 by default; `--hdr-output scrgb` or `off`), optionally with
`--hdr-paper <nits>`, `--hdr-peak <nits>`, `--hdr-black <nits>`.

All options, values in millionths (`kernel-defs.gc`):

| # | Option | Default | Notes |
|---|---|---|---|
| 0 | enable | 0 | implies the FP16 target while on |
| 1 | format | 1 PQ | 1 = PQ (HDR10: R10G10B10A2, `G2084_P2020`, HDR10 metadata from the panel, below); 0 = scRGB (FP16, `G10_P709`, no metadata) |
| 2 | paper white, nits | 200 | where game white (1.0) lands; same value as `pc-set-tonemap 3` |
| 3 | peak, nits | from the display | negative = display profile (MHC2), then DXGI, then 800 |
| 4 | black, nits | from the display | negative = display profile (MHC2), else 0 (DXGI's EDID minimum is logged, not used) |
| 5 | saturation | 1.0 | around luminance, before the curve |
| 6 | knee | 0 | shoulder start: 0 = paper white, 1 = peak |
| 7 | switch Windows HDR on | 0 | opt-in: turns HDR on for the window's display while HDR output runs, restores it after. Off (default), a display with Windows HDR off logs it and stays SDR |
| 8 | calibration pattern | 0 | 0 none, 1 peak, 2 paper white, 3 black; drawn by the final pass, not saved (the calibration page sets it) |
| 9 | peak, thousandths of a nit | | the same setting as 3, for GOAL: its integer and float/int math is 32-bit, so a peak in millionths overflows above 2147 nits |

The calibration values are plain settings (`GfxGlobalSettings::paper_white_nits`,
`hdr_peak_nits`, `hdr_black_nits`); the game's settings menu drives them from the HDR calibration
page (below). `(pc-get-hdr-output n)` reads back, in thousandths: 0 presenting HDR (1000/0), 1 the
display's peak and 2 its black (what the page calls detected; negative before the display was
queried), 3 and 4 the effective peak and black, 5 the peak setting as last sent, 6 Windows' SDR
content brightness (the paper white pattern's fixed half; negative if unknown). Thousandths
because GOAL's integer and float/int math is 32-bit: a peak in millionths overflows above 2147
nits, which is also why the menu sends the peak through option 9.

## Calibration page

Options > Sandover > HDR calibration (`settings.md` has the rows). Like a console calibration
screen: peak brightness (400-10000 nits, defaults to the display's value), paper white (80-400,
default 200) and black level (the shadow lift, 0-1 nit, defaults to the display's value), each with
a test pattern while its row is focused, and "reset to detected". The values apply while a slider
moves and are saved in `sandover.gc`.

The patterns are drawn by `cal_pattern` in `post_processing.frag`, in output nits, after the
mapping (so the shoulder and the peak clip do not touch them), centred below the menu rows:

- **Peak:** a square at full signal (10000 nits; PQ code 1.0) with a diamond at the chosen peak. A
  display clips the square at its own maximum; while the chosen peak is below it, the diamond is
  darker. Raise the peak until the diamond just disappears. With the display's tone mapping off
  (HGiG), that is its real clip point.
- **Paper white:** a card with a mid-grey (0.5) square across it. The left half is game white
  through `hdr_map`, so at paper white: how bright menus, HUD and most of the frame will be. The
  right half stays at Windows' SDR content brightness (the desktop's white; 200 nits if Windows
  does not report it), so the seam shows which way the slider moves and vanishes when they match.
  It is re-read from Windows each time the page opens, so a change to Windows' slider shows there.
  The whole frame moves with paper white, so without that fixed half the card had no reference.
- **Black level:** eight steps of 0, 2, 4 .. 14 / 255 in the game's encoding through `hdr_map`,
  in a black frame. Raise the lift until the second step just separates from the first.

With SDR output the page is greyed; opened anyway (REPL), the patterns draw as an SDR preview.

## Where the defaults come from

At start the log prints what Windows reports for the window's display (`hdr:` lines):

1. **Display profile.** The advanced-colour (HDR) ICC profile Windows uses for that display
   (`ColorProfileGetDisplayDefault`, extended display colour mode; the SDR profile from GDI if
   there is none). The **Windows HDR Calibration** app writes its result into such a profile,
   named after the monitor and the date. Its `MHC2` tag holds the calibrated min and max
   luminance (and a colour matrix Windows applies itself). These become black and peak.
2. **DXGI** (`IDXGIOutput6::GetDesc1`): min, max and full-frame max luminance, primaries,
   colour space. Its max is the peak when there is no MHC2 tag. Usually EDID values, often
   coarse, so its minimum never sets the black lift.
3. Paper white is always 200 nits unless set: Windows' own "SDR content brightness" is printed
   for reference, but a game that draws its own frame sets its own reference white.

Explicit option values override all of this. To seed from a fresh calibration, run the Windows
HDR Calibration app (Microsoft Store), keep the profile it creates as the display default, then
restart the game.

## The mapping

Per pixel, in the final pass (`post_processing.frag`, `hdr_map`):

- The scene value is display-encoded (the PS2 output), so it is decoded with gamma 2.2 (pure
  power, not the sRGB curve, so near-black stays black on an OLED).
- Luminance `Y` times paper white gives nits. Below the knee it is identity: everything up to
  game white looks like SDR at paper-white brightness. Above it an exponential shoulder, same
  slope at the knee, approaches peak and never passes it.
- Toon tonemap only (`tm_mode` 2): cores and halos (max channel past the halo and white points,
  1.2 to 1.8 and 2 to 3.5) first take the chroma of vanilla's per-channel clip at their own
  luminance, like the SDR toon tonemap, so stacked eco reads near-white, not deeper blue. With
  filmic or clamp this step is off and chroma is kept, matching their SDR output.
- Colour rides on luminance (chroma kept), then a hue-preserving clip keeps every channel under
  peak.
- Black: 0 stays 0; shadows are lifted by at most `black` nits (a toe for panels whose floor
  crushes near-black).
- Fades and the brightness/contrast setting apply before decoding, as on the SDR path.

## What to expect

- Most of the frame is at or below game white, so it looks like the SDR game at 200 nits.
  What changes is the overbright part of the FP16 target: eco glow, bloom, sun glints, the sky
  at noon, additive effects. Those used to clip to white; now they rise towards peak.
- The debug UI (ImGui) and the startup splash are drawn unmapped in scRGB (1.0 = 80 nits), so
  they look dimmer than in SDR.
- F9 snapshots and screenshots while HDR output runs save the SDR tonemap of the game image
  at game resolution (the HUD is in it, the debug UI is not), not the HDR frame.

## Known limits

- Windows HDR must be on for the display (Settings > Display, or Win+Alt+B) unless option 7 is
  set; otherwise the game logs `Windows HDR is off` and stays SDR.
- Windows only, and it needs a GL driver with `WGL_NV_DX_interop2` (NVIDIA, AMD and Intel all
  ship it). If anything fails the log says `hdr: HDR output unavailable` and the SDR window
  stays; toggle off and on to retry.
- The swapchain lives on a disabled child window that covers the game window's client area, so
  it works with any driver present mode: NVIDIA's "Vulkan/OpenGL present method" can stay on
  "Prefer layered on DXGI swapchain", and AMD's DXGI present with Windows HDR on does not
  collide either. Mouse and keyboard pass through the child. The log names the composition
  mode DXGI granted (`hdr: DXGI present mode ...`) after the first 120 frames.
- If `CreateSwapChainForHwnd failed (0x80070005)` still appears, something else owns a flip
  chain on the child (a hooking overlay or capture tool); the game stays SDR. Setting the NVIDIA
  present method to "Auto" or "Prefer native" was the old workaround and is no longer needed.
- Window modes present the same swapchain, colour space and metadata; exclusive fullscreen
  stays exclusive. The log states the mode on start, on each change and about 240 frames later
  (`hdr: started: ...`, `window mode changed`, `settled`): window mode, swapchain format and
  colour space, the output's colour space, Windows HDR, the link (encoding, bpc) and the
  metadata sent. `present:` lines carry the window mode, colour space and composition.
- HDR10 metadata (PQ only), sent at start, on each mode change and when the calibration changes:
  mastering max and MaxCLL = the panel's peak (MHC2, else DXGI) capped at the peak setting;
  mastering min = the panel's black (MHC2, else DXGI); MaxFALL = DXGI's full-frame maximum,
  capped the same way. DWM ignores it when it composes (windowed); an overlay plane or
  independent flip (fullscreen) hands it to the display, so it must describe the panel.
- Turning HDR output off in the middle of a session destroys the child window; the game window's
  own GL swap was never touched and resumes.
- With option 7, Windows HDR is restored on exit and on an unhandled crash; if the process is
  killed outright, Windows stays in HDR: turn it off in Settings > Display (or Win+Alt+B).
- The display's own tone mapping should be off (HGiG or the monitor's equivalent), so the
  mapping happens once, in the game.
- The calibration patterns cannot be checked in a screenshot: an F9 snapshot or rig capture while
  HDR output runs saves the SDR tonemap, where they are only a preview.
