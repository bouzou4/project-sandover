# Existing texture-pack ledger

One row per pre-existing texture pack inventoried for asset-pipeline baseline use.
Details and evidence: `sandover/kb/findings/2026-09-23-esrgan-pack-inventory.md`.

| pack | version | coverage | scale | alpha | license | fit |
|---|---|---|---|---|---|---|
| Jak1 ESRGAN Edition | v1.0.1 | 3456/4002 textures (86.4%), 49/126 tpages fully covered, 58 partially, 19 entirely missing (mostly `*-vis-alpha` translucency tpages) | 4.00x uniform (measured on 47 samples, zero variance) | keeps the PS2 128-opaque convention on average but noisy (~113-129 on vanilla-flat-128 textures), occasionally exceeds 128 (up to ~146) with no clamp in the engine's replace path | none found (no readme/license/credits/author metadata in the archive or its folder) | drop-in path match for `custom_assets/jak1/texture_replacements/<tpage>/<tex>.png`; usable as a partial baseline (86.4%) pending an alpha clamp/renormalize pass and a fresh upscale for the missing 13.6%, private use only until source/license is confirmed |
