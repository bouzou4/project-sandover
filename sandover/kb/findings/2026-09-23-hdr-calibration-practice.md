---
title: How shipped PC/console games do in-game HDR calibration
date: 2026-09-23
issue: "HDR calibration screen"
status: verified
sha: 2dd18a9 (2dd18a9ca76cb6876d6e99667b42c647516f8ad4)
---

## Answer (≤5 lines)

Shipped titles run two or three short screens — black floor, peak brightness, and (on PC)
paper white — each a checkerboard or patch pattern the player dials until it just
disappears, exactly the pattern HGiG standardized as MinTML/MaxTML(FF)/paper-white and
that Xbox's and PlayStation's own HDR calibration apps implement. The three resulting
numbers (in nits) become the tonemapper's toe, shoulder, and diffuse-white scale; Windows'
own `DXGI_OUTPUT_DESC1`/EDID luminance values are only ever a starting point for the
sliders, never trusted outright — that's HGiG's own documented behavior, not an invention.

## Evidence (verified vs inferred, path/URL)

**HGiG itself — verified, primary source.** "For a Better HDR Gaming Experience" v1.1,
HDR Gaming Interest Group (Microsoft + Sony Interactive Entertainment), July 2019:
https://www.hgig.org/doc/ForBetterHDRGaming.pdf (fetched and read in full).
- Defines three display-side quantities games must consume: `MaxFullFrameToneMapLuminance`
  (MaxFFTML, full-panel peak, nits), `MaxToneMapLuminance` (MaxTML, ~10%-window peak,
  nits), `MinToneMapLuminance` (MinTML, black floor, nits). "Primary HDR range" = MinTML to
  MaxFFTML; "Extended HDR range" = MinTML to MaxTML for small bright objects (pp. 9–14).
- The **reference calibration UI is three checkerboard screens** (pp. 20–22): a full-frame
  checkerboard for MaxFFTML ("adjust slider until gray squares just disappear"), a small
  windowed checkerboard for MaxTML (same rule, expected to differ from MaxFFTML), and a
  near-black checkerboard for MinTML ("adjust slider until gray squares just disappear").
  This is the literal shipped pattern, not a paraphrase.
- **EDID/database values are declared a starting point only**, explicitly overridden by
  the console's own calibration (pp. 15–19, flowchart on p. 19: "Execute calibration.
  MaxTML, MinTML and MaxFFTML are overridden by the setting"). Worked example on pp. 24–25
  shows a real TV's EDID-derived Category default (4000 nits) being wrong for the actual
  panel (2000 nits) and the consumer re-running calibration to fix it — this is the
  documented reason not to trust reported display metadata outright.
- Guiding principle 4 explicitly favors simple manual setup over none, but principle 4 also
  says avoid *complicated* manual setup — hence checkerboards and sliders, not raw numbers,
  for the consumer-facing version (numbers are shown only in the display manufacturer's
  internal calibration build, p. 15).

**Xbox HDR calibration app — verified, matches HGiG's reference flow.**
https://www.slashgear.com/1549604/your-xbox-series-x-s-hdr-settings-are-probably-wrong-heres-how-to-get-them-right/
and https://www.asurion.com/connect/tech-tips/how-to-optimize-hdr-on-xbox-series-x/ —
Settings > General > TV & display options > Calibrate HDR for games. Two checkerboard
screens: minimum luminance (raise until the pattern just disappears into black) and
maximum luminance (raise until the pattern is just barely visible, not fully gone). The
exact nits values are hidden from the normal flow but can be revealed by holding
LB+RB+LT+RT — i.e. the app stores real numbers under an abstracted slider, same as HGiG's
consumer-vs-manufacturer split.

**Windows HDR Calibration app — verified.**
https://support.microsoft.com/en-us/windows/hardware/display-graphics/calibrate-your-hdr-display-using-the-windows-hdr-calibration-app
and corroborating how-tos. Exposes exactly the three HGiG-shaped quantities under
different names: **Max Full Frame Luminance** (slider raised until a checker pattern is no
longer visible), **Minimum Luminance** (slider raised until a pattern is no longer
visible), and **SDR Content Brightness** (a percentage that maps to nits, e.g. 0%=81,
25%=181, 50%=287, 75%=401, 100%=498 nits on a typical panel) — SDR content brightness is
Windows' equivalent of a game's "paper white."

**DXGI_OUTPUT_DESC1 / why in-game calibration still overrides OS metadata — verified,
primary source.** https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/ns-dxgi1_6-dxgi_output_desc1
(fetched in full). Three luminance fields, all `FLOAT`, in nits, "usually from the EDID of
the corresponding display or sometimes from an override":
- `MinLuminance` — minimum the display can render; content should not go below it.
- `MaxLuminance` — maximum, "likely only valid for a small area of the panel."
- `MaxFullFrameLuminance` — maximum "valid for a color that fills the entire area of the
  panel" — the same full-panel-vs-windowed distinction HGiG draws between MaxFFTML/MaxTML.
These map almost one-to-one onto HGiG's three terms, which is why a renderer *can* seed
calibration defaults from `IDXGIOutput6::GetDesc1`. It still cannot skip the calibration
screen: EDID luminance data is frequently wrong or a coarse default-category guess (HGiG's
own worked example, above), and Microsoft's own advanced-color article says explicitly
that games must let the user set or read the SDR reference white level rather than assume
a fixed value.

**scRGB vs PQ swapchains, and paper white / reference white — verified, primary source.**
https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
(fetched in full).
- Two supported swapchain configurations: (1) `DXGI_FORMAT_R16G16B16A16_FLOAT` +
  `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (scRGB) — linear, BT.709 primaries, values
  outside [0,1] represent >80 nits or out-of-gamut color, e.g. scRGB (12.5,12.5,12.5) =
  1000 nits D65 white. Works for any Advanced Color display and any content. (2)
  `DXGI_FORMAT_R10G10B10A2_UNORM` + `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (PQ/HDR10)
  — half the bandwidth of scRGB, but D3D11/12 only, no blending, HDR-display-only. This
  is the PQ-vs-scRGB tradeoff: scRGB for correctness and compatibility (recommended
  default), PQ10 as a fullscreen-only bandwidth optimization.
- **Reference/paper white, in Microsoft's own words**: "Reference white indicates the
  brightness at which a diffuse white object... appears in an HDR scene... Windows allows
  the user to adjust the SDR reference white level... typically set to around 200 nits" on
  desktop monitors. A game rendering both HDR and SDR/UI content into one surface "is
  responsible for performing the SDR reference-white adjustment [itself]" — this is
  precisely the paper-white value a game's own calibration screen must capture, because
  the OS-wide slider does not reach into a game's fullscreen composited frame.
- Tone mapping guidance: "the most important single parameter to adapt for is max
  luminance... more sophisticated tone mappers will also adapt min luminance and/or color
  primaries" — i.e. peak first, black floor second, matching HGiG's and Xbox's screen
  order.

**PC game paper-white/peak sliders — inferred (secondary sources, consistent across
several titles, not primary documentation).**
https://twinfinite.net/guides/cyberpunk-2077-best-hdr-settings/ and
https://hdrtune.com/cyberpunk-2077-hdr-settings/: Cyberpunk 2077 exposes "Paper White"
(UI/menu luminance, recommended 150–250 nits depending on panel), "Maximum Brightness"
(peak, slider to 3000+ nits), and a black-point control, as separate sliders shown directly
in nits rather than abstracted steps — this is the PC convention (numbers shown), versus
the console convention (numbers hidden behind a checkerboard slider). Both conventions
store the same three quantities.

**HGiG mode on a display — verified via HGiG document + corroborating explainer.**
https://www.whathifi.com/advice/hgig-explained-what-is-hgig-how-do-you-get-it-and-should-you-use-it
and the HGiG PDF itself (pp. 4, 8–10): "HGiG mode" on a TV/monitor means the display turns
off its own dynamic tone mapping / dynamic contrast for game sources and instead
reproduces the incoming PQ signal up to its declared MaxTML/MaxFFTML faithfully. The whole
point is that tone mapping happens exactly once — in the game, using the three calibrated
values — rather than once in the game and again, differently, in the display.

## Implications for the design

- The planned HDR calibration screen should become: black floor → peak brightness → paper
  white, each a checkerboard/patch pattern dialed until it just disappears or barely
  survives (HGiG/Xbox pattern), not a "faint logo"/"reference sunset" invention. Paper
  white is the PC-specific third screen beyond the console two-screen flow, because a
  fullscreen game must set its own SDR reference white rather than rely on the OS slider
  (MS Advanced Color article, above).
- The three stored values are, in effect, `black_nits`, `peak_nits`, `paper_white_nits` —
  HGiG's MinTML, MaxFFTML (or MaxTML), and Windows' SDR-reference-white concept
  respectively. The planned settings schema already stores calibration as "the step index the
  viewer picked... conversion to output scale is code, so the file carries no physical
  units" — that is compatible with this finding (Xbox does the same: abstracted slider,
  real nits underneath) and does not need to change for the screen rewrite. It is flagged below
  as a place to double check when the HDR calibration screen is scoped, since the step→nits conversion table is
  not yet designed.
- `DXGI_OUTPUT_DESC1` (`MinLuminance`/`MaxLuminance`/`MaxFullFrameLuminance`) is the right
  API to seed slider defaults and ranges from the OS/EDID, matching HGiG's own "Category
  Designation" fallback — but per both HGiG's worked examples and the project's
  existing "not trusting OS monitor metadata" decision, it is a starting point only, never
  applied without the calibration screen confirming it.
- FP16/scRGB (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`)
  is the correct swapchain choice for the FP16 target, not PQ10, given sandover already blends
  UI and post effects into one surface (PQ10's no-blending restriction rules it out); this
  matches ADR-0013's existing note that FP16 is a deliberate non-parity-neutral choice.

## Follow-ups

- The planned settings schema defines the step→nits conversion
  as "code" with no schema yet; the HDR calibration screen needs to pick the actual step table/curve before
  implementation, ideally seeded from `DXGI_OUTPUT_DESC1` ranges.
- Whether sandover shows nits directly (PC convention) or hides them behind steps (console
  convention) is a UX call for the HDR calibration screen, not settled by this finding; both are established
  practice and either is defensible. The design-doc replacement below keeps the existing
  "steps, never numbers" choice since it was already decided and is equally well
  precedented (Xbox/PlayStation).
- Not verified from a primary source: exact current-generation PlayStation calibration
  screen flow (inferred to match Xbox/HGiG's reference implementation from secondary
  sources only); worth a follow-up look if PS-specific wording is ever needed.
