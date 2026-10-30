---
title: HDR screenshot format - a hand-written uncompressed half EXR, not a library
date: 2026-09-25
issue: none
status: verified
sha: branch tools/rig-hdr-capture (base 7ce790fdf)
---
## Answer (≤5 lines)
The engine writes HDR captures with its own minimal OpenEXR writer (`common/util/exr.cpp`,
~250 lines with the matching reader): scanline, B G R HALF, no compression. There is no EXR
library in `third-party/` (no OpenEXR, no TinyEXR) and no zlib to build ZIP compression on.
A second implementation in Python (`sandover/hdr.py`) reads and writes the same layout; its
output is read back exactly by OpenCV's OpenEXR reader, an independent implementation.

## Evidence (path:line@sha / URL, short excerpts)
- `third-party/` listing at 7ce790fdf: fpng, stb_image, tiny_gltf, zstd; no EXR code, no zlib.
- Spec followed: https://openexr.com/en/latest/OpenEXRFileLayout.html (magic 20000630,
  version 2, header attributes `channels chlist`, `compression`, `dataWindow`, `displayWindow`,
  `lineOrder`, `pixelAspectRatio`, `screenWindowCenter`, `screenWindowWidth`; line offset
  table of u64; chunks `y, byte count, channel rows in channel-list order`).
- `test/test_exr.cpp`: every finite non-negative half survives float -> half -> float bit-exactly;
  ties round to even; a 37x11 synthetic HDR image round-trips with its string attribute.
- `sandover/tools/tests/test_hdr.py`: Python round trip; `cv2.imread` with
  `OPENCV_IO_ENABLE_OPENEXR=1` on the Python writer's output: max abs difference 0.0.

## Implications for the design
- Uncompressed costs disk, not time: 2560x1440 is 22 MB per shot. At the capture budget
  (2-4 shots per round) that is under 100 MB a round; not worth a compression dependency.
- The values are the framebuffer values the tonemap consumes (display-encoded game colour,
  1.0 = SDR white), not sRGB-decoded linear light. Offline tonemaps reproduce the game's PNG;
  external viewers show the image lighter. Documented in `kb/howto/hdr.md`.
- TinyEXR would add ZIP (it bundles miniz) if file size ever matters; it is one header plus
  miniz and could replace `exr.cpp` without changing callers.

## Follow-ups
- Confirm on the first FP16 capture that `sandover-hdr stats --preview --curve clamp`
  (or `filmic` with the game's settings) matches the PNG.
