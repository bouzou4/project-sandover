# Motion blur reference: Rift Apart and published filters (2026-10-19)

Labels: VERIFIED (URL fetched or in search result), INFERRED (reasoned from verified facts), EXTERNAL (author recall of the literature, not re-fetched this session; treat as a lead to check).

## Headline gap
No Insomniac GDC/SIGGRAPH talk on motion blur (Spider-Man, Miles Morales, Rift Apart) was found in two rounds of searching. Rift Apart's internal technique is NOT publicly described as far as this search could tell. Everything below on its internals is inference.

## 1. Technique
- VERIFIED: Insomniac describes the option as "Adjust the blurring effect created by camera motion and fast moving objects. Lower settings have less blur, while higher settings have more apparent blur." https://support.insomniac.games/hc/en-us/articles/46716208107795-What-Accessibility-options-does-Ratchet-Clank-Rift-Apart-feature and https://twitter.com/insomniacgames/status/1395407263691689985
- INFERRED: camera plus per-object blur from one velocity buffer, via a tile-max / neighbour-max scatter-as-gather filter (the standard console approach of that generation). No source states tile size, samples, shutter or half-res tricks for Insomniac.
- No verified information on limb/melee handling.

## 2. PC settings
- VERIFIED: PC exposes a single "Motion Blur Strength" slider, described as "Makes movement appear smoother by blurring geometry along the direction it is moving." Range/default not stated. https://steamcommunity.com/app/1895880/discussions/0/3803903751087729405/
- VERIFIED (user posts, weak): values like 10 and 5 reported in settings dumps. https://steamcommunity.com/app/1895880/discussions/0/3801652295709005205/?ctp=3
- VERIFIED: the description lumps camera and object blur under one control; no separate quality tier was found. (Absence of a tier is INFERRED from not finding one.)
- NOT FOUND: Digital Foundry analysis of the strong setting, trail length at 60/120 fps, or frame-rate scaling. Not verified either way. DF PC coverage exists but a snippet-level search surfaced nothing on blur: https://multiplayer.it/notizie/ratchet-clank-rift-apart-pc-analisi-completa-digital-foundry-bello-ma-imperfetto.html
- VERIFIED (weak): a Steam thread reports uneven camera motion with frame generation, mitigated by disabling motion blur.

## 3. Comparable references
- VERIFIED: Jimenez, SIGGRAPH 2014 "Next Generation Post Processing in CoD: AW": 16.6 ms budget for 60 fps; commenter reply by Jimenez: camera rotation motion blur is removed in that FPS. The page holds no sample counts or shutter numbers (they are in the linked slides). https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
- VERIFIED (secondary note): pipeline is tile (nearest depth + max velocity/CoC) then 3x3 neighbour gather. https://scrapbox.io/0b5vr/Next_Generation_Post_Processing_in_Call_of_Duty:_Advanced_Warfare
- VERIFIED: NVIDIA GameWorks sample of the McGuire/Guertin family: max blur radius K ~20 px (also the tile size, TileMax buffer is w/K x h/K), S ~15 reconstruction taps (odd), exposure fraction = fraction of frame the shutter is open; cost depends on resolution, not scene. https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/motionblurgl4gles3advancedsample.htm
- VERIFIED: Guertin et al. 2014 (feature-aware, stable filter) PDF exists but was not text-extracted; Blender EEVEE credits Jimenez as a modification of it. https://research.nvidia.com/sites/default/files/pubs/2013-11_A-Fast-and/Guertin2013MotionBlur-small.pdf , https://lists.blender.org/pipermail/bf-blender-cvs/2020-April/138561.html
- EXTERNAL: McGuire et al. 2012 (I3D, "A Reconstruction Filter for Plausible Motion Blur") uses tile size = max radius (r=40 px at 1280x720 in the paper), neighbour-max pass, ~15 taps, soft-depth-weighted cone/cylinder reconstruction, and reports a ~1-2 ms class cost on 2012 hardware at 720p-1080p. Jimenez CoD:AW: tile max + neighbour max, gather with roughly 8 taps per ring, interleaved-gradient noise, half-res options (EXTERNAL, check slides). Typical cost scale at 4K: ~4x the 1080p cost; 16 taps full-res 4K is plausibly 1-2 ms on a current mid-range GPU (INFERRED, not measured).

## 4. Recommendation (INFERRED; Rift Apart's "strong" look is not documented, so "strong" is set by film convention, not by matching a measurement)
Film shutter 180 deg = 0.5 of frame time. Games commonly use 0.5 at most; stronger reads as smear.

| Strength | Shutter fraction (of 60 fps frame) | Notes |
|---|---|---|
| Off | 0 | |
| Weak | 0.25 | barely visible except fast limbs |
| Medium | 0.5 (180 deg) | film-like default |
| Strong | 0.75-1.0 (270-360 deg) | exaggerated, Rift Apart-like heavy look (INFERRED target) |

- Samples: 16 reconstruction taps for weak/medium; 24-32 for strong at 4K, because strong stretches trails over ~2x the pixels so tap spacing doubles and banding/ghosting appears below ~24. Use per-pixel jitter (interleaved gradient noise) to trade banding for noise. A separate quality tier on top of strength is warranted only if cost matters: offer samples as an internal scale (16 default, 32 on strong) rather than a user option; a user-facing Low/High is optional at 4K.
- Max blur radius: clamp ~3-5% of screen height (about 65-110 px at 2160p; tile size = that radius, e.g. 64 px tiles at 4K). 20 px at 1080p-class (NVIDIA sample) is ~1.9%, which is the conservative end.
- Frame-rate scaling: shutter is time-based, so blur length in pixels = velocity-per-second x (shutter_fraction / fps). Keep the shutter FRACTION fixed relative to the 60 fps design and let velocity (px/frame) naturally halve at 120 fps, giving half-length trails (physically correct). Alternatively scale the fraction by fps/60 to hold trail length constant; this is an aesthetic call. Fix the velocity buffer in per-frame units and multiply by shutter_fraction * (actual_fps/60 if holding length).
- Camera blur: consider attenuating pure camera rotation (Jimenez removes it in a first-person game, VERIFIED above) while keeping object and translation blur.
