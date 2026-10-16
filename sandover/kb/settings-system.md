# OpenGOAL PC settings, end to end (Jak 1)

Code read for #228 (Sandover settings in the OpenGOAL menu) and the later HDR calibration
screen. All references are `path:line` at master `6bbdafd0c`. Nothing here was run.

First cut implemented for #228: `howto/settings.md`. Differences from the original plan: one page with the
switches and lists (the options scroll) instead of eight sub-pages; the apply path compares each
field with what was last pushed (starting from the engine's boot state) instead of a single dirty
flag, so cancel needs no hook and an all-off file pushes nothing; list rows use one generic
`sandover-list` option type whose values come from the row's name, with no new `game-option`
field. Not yet: SSAO strength list, exposure, spatial audio. The HDR calibration page is in
(`howto/settings.md`, `howto/hdr-output.md`): `pc-set-hdr-output 2-4` for the values,
`pc-set-hdr-output 8` for the pattern, `pc-get-hdr-output` for the display facts; values below 0
mean "from the display" rather than 0.

## 1. GOAL side: `pc-settings`

### Type and fields

| What | Where |
|---|---|
| Base type `pc-settings` (all games' fields, method table) | `goal_src/jak1/pc/pckernel-h.gc:104-247` |
| Jak 1 subtype `pc-settings-jak1` (cheats, languages, `secrets` inline group) | `goal_src/jak1/pc/pckernel-impl.gc:73-84` |
| Global `*pc-settings*`, created at engine load with `define-once` | `goal_src/jak1/pc/pckernel.gc:514` (declared `pckernel-impl.gc:86`) |
| Settings version constant (1.10.4.0) | `goal_src/jak1/pc/pckernel-impl.gc:11` |

A field is a plain slot in the `deftype`. The menu can only bind to three storage shapes:
`symbol` (on-off), `float` (slider) and an integer read through a carousel backup (see §2).

### Defaults

`reset` (`pckernel-h.gc:286-311`) calls `reset-gfx`, `reset-audio`, `reset-input`, `reset-ps2`,
`reset-misc`, `reset-extra`. Jak 1 overrides `reset-misc` and `reset-extra`
(`pckernel-impl.gc:119`, `:146`). A new field group gets its defaults by overriding one of
these (or adding a `reset-sandover` called from them).

### Load

1. `new pc-settings` calls `initialize` (`pckernel-common.gc:672-674`).
2. `initialize` runs `reset obj #f` first, then `load-settings` (`pckernel-common.gc:658-670`).
   Jak 1 then copies volumes into `*setting-control*` (`pckernel.gc:17-25`).
3. `load-settings` (`pckernel-common.gc:642-656`): file is
   `*pc-settings-folder*/pc-settings.gc`. Missing file: reset with handlers and write the file.
   Read failure: `reset obj #t` (all defaults), file not rewritten until the next commit.
4. `*pc-settings-folder*` is set by C++ from `file_util::get_user_settings_dir`
   (`game/kernel/jak1/kmachine.cpp:566-567`, `common/util/FileUtil.cpp:89-96`):
   `<user config>/OpenGOAL/jak1/settings/`, or `<--config-path>/OpenGOAL/jak1/settings/`
   (`game/main.cpp:139`). Sandover instance slots use per-slot config dirs this way.

### File format and versioning

S-expression text, one group:

```lisp
(settings #x<PC_KERNEL_VERSION>
  (fps 60)
  (vsync #t)
  ...
  (secrets (cheats #x0 #x0) (music ...))
  )
```

| Aspect | Behaviour | Where |
|---|---|---|
| Writer | `write-to-file` prints the header then `handle-output-settings` | `pckernel-common.gc:630-640`, `:558-628`; Jak 1 extras `pckernel.gc:471-512` |
| Reader | `read-from-file`, `dosettings` loop, `case-str` on the key | `pckernel-common.gc:406-444`, `:390-404`; per-key `:446-556`; Jak 1 `pckernel.gc:435-469` |
| Nested group | a key whose value is its own `dosettings` loop (`secrets`) | `pckernel.gc:447-469` |
| Version check | major or minor differ: file rejected, full reset | `pckernel-common.gc:417-430` |
| Migration | inline `when` on the old build/revision after parsing (1.10 envmap) | `pckernel-common.gc:432-438` |
| Missing key | keeps the value from the `reset` that ran before the load | `pckernel-common.gc:661-663` |
| **Unknown key** | no skip: its value is left unread, the `)` check fails, `read-from-file` returns `#f`, and **every** setting resets to default | `pckernel-common.gc:376-378`, `:401-403`, `:648-650` |
| Removed keys | kept as read-and-discard cases because of the above | `pckernel-common.gc:485-490`, `:515-517`, `:533`, `:538` |

### Save triggers

`commit-to-file` (`pckernel-common.gc:77-82`) is called on every confirmed menu option
(`progress-pc.gc:2803`), on menu buttons (`:2561`), resolution/monitor/controller/aspect pickers
(`:2591`, `:2601`, `:2609`, `:2637`), and on every auto-save (`engine/game/game-save.gc:908-910`).

### Per-frame apply

`main.gc:625` calls `(update *pc-settings*)` once per frame (then Sandover's
`pc-world-lights-update`, `main.gc:626`). `update` (`pckernel-common.gc:182-205`) runs
`update-from-os` (window size, aspect; `:84-112`) and `update-to-os` (`:114-169`), which pushes
letterbox, vsync, frame rate, game resolution, MSAA, LOD, discord, sound hacks and LED to C++
**every frame**, unconditionally. Other settings are pushed at change time by the option's
`on-change` / `on-confirm` (e.g. mouse, `pckernel-common.gc:286-291`).

## 2. The Jak 1 PC progress menu

### Pieces

| Piece | Where |
|---|---|
| `game-option` type: `option-type`, `name` (text-id), `param1/2` (slider min/max), `param3` (target screen), `value-to-modify` (pointer), PC additions `option-disabled-func`, `name-override`, `on-change`, `on-confirm`, `slider-step-size`, `slider-show-decimal?` | `engine/ui/progress/progress-h.gc:204-219` |
| `progress-screen` enum, PC screens appended before `max` | `progress-h.gc:98-133` |
| `game-option-type` enum, PC types appended | `progress-h.gc:145-164` |
| `game-option-menu` = copy of `progress-screen` | `progress-h.gc:167-169` |
| Page arrays (`*main-options-pc*`, `*graphic-options-pc*`, `*misc-options*`, ...) | `pc/progress-pc.gc:113-1008` |
| Screen to page table `*options-remap*`, **allocated length 68** | `engine/ui/progress/progress-static.gc:258-259` |
| Page binding and `value-to-modify` wiring, by array index | `progress-pc.gc:2041-2179` |
| Input handling (left/right, cancel, confirm) | `progress-pc.gc:2181-2804` |
| Drawing per option type | `progress-pc.gc:2961-3335` |
| Per-screen draw dispatch (position, scale) | `progress-pc.gc:3456-3500` |
| Screens allowed to go back | `engine/ui/progress/progress.gc:584-634` |
| Rows per page before scrolling: 7 | `progress-pc.gc:11`, `:2204` |
| Menu stack depth: 5 | `progress.gc:294-301`, `progress-h.gc:267` |

`progress-screen max` is 67 today (35 original + 32 PC), so `*options-remap*` has one free
slot. Every added screen beyond one needs the 68 raised.

### Option types a Sandover page needs

| Type | Storage | Semantics | Code |
|---|---|---|---|
| `menu` | none; `:param3 (game-option-menu <screen>)` | pushes the screen | `progress-pc.gc:2452-2466` |
| `button` | none; `case` on `name` | action; `back` pops | `progress-pc.gc:2467-2562` |
| `on-off` | `(pointer symbol)` | left = on, right = off; `on-change` replaces the write if set | `:2243-2260`, `:2335-2346` |
| `slider` | `(pointer float)` in `[param1, param2]`, step `slider-step-size` | `on-change` gets the float every step | `:2299-2327`, `:2385-2408` |
| carousel (`msaa`, `lod-bg`, `speaker`, ...) | `*progress-carousell* int-backup` | hard-coded per type in five places: left `:2276-2295`, right `:2362-2381`, select-init `:2670-2710`, confirm `:2759-2800`, draw `:3155-3166`; texts from `def-progress-carousell` `:86-104` | |
| `confirmation` | yes/no prompt | used for resets | `:2273`, `:3128` |

Lifecycle of a value option: X/circle selects and stores a backup (`:2644-2657`); left/right
edit the live field (so the effect previews immediately if `on-change` pushes it); square/triangle
restores the backup (`:2409-2425`); X/circle again confirms, runs `on-confirm`, then
`commit-to-file` (`:2733-2803`). `option-disabled-func` greys an option out (`:2443-2447`).

### Adding a page (checklist)

1. Enum entry in `progress-screen` before `max` (`progress-h.gc:98-133`); raise
   `*options-remap*` allocated length (`progress-static.gc:259`).
2. `(define *my-options* (new 'static 'boxed-array :type game-option ...))` in `progress-pc.gc`,
   last entry a `back` button.
3. A `menu` option pointing at it from a parent page; for the top level, both
   `*main-options-pc*` and `*main-options-secrets*` (`progress-pc.gc:113`, `:162`).
4. `(set! (-> *options-remap* (progress-screen my)) *my-options*)` and the `value-to-modify`
   pointers in `init-game-options` (`progress-pc.gc:2041-2179`). Binding is by array index:
   inserting a row shifts every later index.
5. Add the screen to the draw dispatch (`progress-pc.gc:3456-3500`) and to `can-go-back?`
   (`progress.gc:584-634`).

### Strings

| Step | Where |
|---|---|
| Text id enum entry, `(my-label #xNNNN)`; PC ids start at `#x1000`, highest today `#x1619` | `engine/ui/text-h.gc:556-798` |
| Same entry in the decompiler's type mirror (kept in sync between the `GAME-TEXT-ID ENUM` markers) | `decompiler/config/jak1/all-types.gc:885-1703` |
| String, keyed by hex id without `#x` (`"1619": "ANALOG SENSITIVITY"`) | `game/assets/jak1/text/game_custom_text_en-US.json` |
| Project file listing the JSON per language (group `common`) | `game/assets/jak1/game_text.gp:9-42` |
| Build step (`(mi)` rebuilds `*COMMON.TXT`) | `goal_src/jak1/game.gp:407-415` |
| English is always loaded as fallback, so other languages can lag | `text-h.gc` fallback block, end of file |

Labels are looked up with `(lookup-text! *common-text* (text-id ...) #f)`. `name-override`
(a runtime string) bypasses text ids and is used only by generated pages (`progress-pc.gc:1177`,
`:1262`).

## 3. C++ side

### How GOAL calls C++

Jak 1 `kmachine.cpp:543` passes `make_function_symbol_from_c` into
`init_common_pc_port_functions` (`game/kernel/common/kmachine.cpp:1885`), which registers each
`pc-*` symbol. Sandover's are inside `if (g_game_version == GameVersion::Jak1)`
(`kmachine.cpp:1983-2018`). GOAL declares each with `define-extern` in
`goal_src/jak1/kernel-defs.gc:571-682` (the index tables are the comments there).

Conventions: numeric values are integers in **millionths** (`value / 1000000.f`); `which` selects
a field through an enum in `gfx.h` (`SsaoParam:76`, `TonemapOption:109`, `WaterOption:119`,
`FurOption:166`, `ShadowOption:178`, `PbrParam:196`). Setters write straight into
`Gfx::g_global_settings` from the EE thread; the renderer reads it on its own thread without a lock.

`GfxGlobalSettings` is `game/graphics/gfx.h:239-493`, the global `gfx.h:506`. Every Sandover
feature defaults to off there (`gfx.h:302`, `:312`, `:320`, `:343`, `:353`, `:360`, `:402`,
`:412`, `:420`, `:451`, `:456`, `:459`).

### Sandover forms today

| Form | C++ setter | Settings written (`gfx.h`) | `pc-settings` field | Persisted |
|---|---|---|---|---|
| `pc-set-ssao-preset n` | `kmachine.cpp:1081` → `SsaoApplyPreset` (`SsaoRenderer.cpp:99-107`) | `ssao_params`, `ssao_preset`, `ssao_enable` | none | no |
| `pc-set-ssao-param which v` | `kmachine.cpp:1343` | `ssao_enable`, `ssao_params.*`, `ssao_emissive*` | none | no |
| `pc-set-bloom which v` | `kmachine.cpp:1100` | `bloom_enable/strength/radius/threshold/hue_clip` (`:312-316`) | none | no |
| `pc-set-emissive-lights which v` | `kmachine.cpp:1278` | `emissive_lights_*`, `world_lights_*` (`:320-332`) | none | no |
| `pc-set-shadows mode strength` (GOAL, `engine/draw/drawable.gc:439-444`) | wraps `pc-set-shadow-option` (`kmachine.cpp:1467`) and sets `*pc-shadow-caster-margin*` | `shadow_mode`, `shadow_strength` (`:412-418`) | none | no |
| `pc-set-fur which v` | `kmachine.cpp:1245` | `fur_*` (`:402-409`) | none | no |
| `pc-set-water which v` | `kmachine.cpp:1124` | `water_*`, wave sets (`:360-400`) | none | no |
| `pc-set-pbr-param which v` | `kmachine.cpp:1498` | `pbr_*` (`:420-446`); `LOOK` reloads the preset | none | no |
| `pc-set-hdr-target sym` | `kmachine.cpp:1410` | `hdr_target` (`:353`) | none | no |
| `pc-set-tonemap which v` | `kmachine.cpp:1414` | `tonemap_mode/exposure/knee`, `paper_white_nits` (`:354-357`; paper white unused by the renderer) | none | no |
| `pc-set-sun-option` / `pc-set-sun-dir` | `kmachine.cpp:1436` | `sun_*` (`:343-350`) | none | no |
| `pc-set-vista name`, `-hide`, `-option` | `kmachine.cpp:1626`, `:1636`, `:1641` | `vista_*` (`:459-491`) | none | no |
| `pc-set-vista-fog`, `-haze`, `-neighbour-fog`, `-neighbour-haze` | `kmachine.cpp:1712`, `:1789`, `:1764`, `:1810` | `vista_fog_mode`, `vista_atmo_*`, `vista_haze_*`, `vista_nb_fog` | none | no |
| `pc-set-stereo`, `-stereo-option`, `pc-set-xr` | `kmachine.cpp:1587`, `:1609` | `stereo_*`, `xr_enable` (`:451-456`) | none | no |
| `pc-set-audio-spatial "mode"` | `kmachine.cpp:1613` | sound bus mode (also `gk --audio-spatial`, `game/main.cpp:122`) | none | no |

None of them is persisted anywhere: no `pc-settings` field, nothing in `game/settings/settings.cpp`
(display/input/debug JSON only). Every boot starts vanilla; the play launcher re-sends the forms
listed in `kb/howto/play.md`. The ImGui checkboxes (`SsaoRenderer.cpp:901`,
`BloomRenderer.cpp:231`, `EmissiveLights.cpp:399`, `Materials.cpp:1605`, `Merc2.cpp:725`) edit the
same globals and are equally transient.

## 4. Presets

| Preset | How it reaches the engine | When |
|---|---|---|
| `pbr.toml` | `pbr_load_preset` reads `$SANDOVER_PBR_PRESET`, else `<project>/sandover/presets/pbr.toml` (`Materials.cpp:1359-1364`, `:1498-1600`) | renderer construction (`OpenGLRenderer.cpp:136-139`), which runs in `Gfx::Init` before the EE thread starts (`game/runtime.cpp:443`, `:482`); again on `pc-set-pbr-param 11` (`kmachine.cpp:1535-1537`) and the ImGui look combo / reload button (`Materials.cpp:1613-1614`, `:1640-1641`) |
| `ssao.toml` | compiled in: `sandover-preset gen-header ssao` writes `SsaoPresets.inc`; `pc-set-ssao-preset n` indexes it | build time |
| `water`, `fur`, `emissive`, `vista.toml` | never parsed by the engine; `sandover/tools/sandover/presets.py:1-5` turns them into `pc-set-*` forms that the rig or play launcher sends over nREPL; engine defaults in `gfx.h` mirror the chosen preset | per run |

`SANDOVER_ROOT` is only a tools-side path (`sandover/tools/sandover/paths.py:10`); the engine
finds the project through `OPENGOAL_PROJ_PATH` / `get_jak_project_dir`.

Who wins, today: `gfx.h` defaults < `pbr.toml` at renderer start < any `pc-set-*` form after
boot (GOAL, rig, REPL) = ImGui edits; last writer wins. Two traps for a menu layer:

- `pbr_load_preset` first restores every `pbr_*` scalar to its built-in value, then applies the
  file (`Materials.cpp:1556-1575`). A look change therefore wipes any menu-set PBR scalar.
- `update-to-os`-style unconditional per-frame pushes would stomp rig and REPL forms every frame.

So the "menu overrides preset" layer belongs in GOAL, applied on load and on change only, and it
should expose only switches and named looks, never the scalars a preset or look owns.

## 5. Recommendation for #228

### Persistence: a separate file, never `pc-settings.gc`

An unmodified OpenGOAL resets **all** of the user's settings on the first unknown key
(§1 table). So Sandover keys never go into `pc-settings.gc`, and `PC_KERNEL_VERSION` major/minor
stays untouched (a mismatch is also a full reset). Write `*pc-settings-folder*/sandover.gc`:

```lisp
(sandover 1
  (ssao-preset 4)
  (bloom? #t)
  ...
  )
```

Same helpers (`dosettings`, `file-stream-read-*`), with one difference: the reader skips unknown
keys (read to the matching `)`), so an older Sandover build reads a newer file. The leading
integer is the Sandover schema version; migrations follow the `pckernel-common.gc:432-438` pattern.
Upstream ignores the file; switching back to upstream loses nothing.

Hooks, all in `pc/pckernel.gc` (Jak 1 only, so Jak 2/3 are untouched):
`initialize` override (`:17`) reads `sandover.gc` after the parent load;
a `commit-to-file` override calls the parent then writes `sandover.gc` (this catches every
menu confirm and auto-save); `reset-extra` override sets Sandover defaults.

### Field group

An inline structure in `pc-settings-jak1` (like `secrets`, `pckernel-impl.gc:82`), so menu
pointers are `(&-> *pc-settings* sandover <field>)`:

| Field | Type | Menu | Pushes |
|---|---|---|---|
| `ssao-preset` | `int32` (-1 off, 0..4) | carousel | `pc-set-ssao-preset` / `pc-set-ssao-param 0 0` |
| `emissive-lights?` | `symbol` | on-off | `pc-set-emissive-lights 0` |
| `bloom?` | `symbol` | on-off | `pc-set-bloom 0`, `4` |
| `shadows?` | `symbol` | on-off | `pc-set-shadows` |
| `pbr?` / `pbr-look` | `symbol` / `int32` (0..3) | on-off / carousel | `pc-set-pbr-param 0`, `11` |
| `fur?` | `symbol` | on-off | `pc-set-fur 0` |
| `water?` | `symbol` | on-off | `pc-set-water 0` |
| `vista?` | `symbol` | on-off | the `vista.toml` forms (baked into a GOAL function) / `pc-set-vista "none"` |
| `hdr-target?` / `tonemap-mode` / `exposure` | `symbol` / `int32` / `float` | on-off / carousel / slider | `pc-set-hdr-target`, `pc-set-tonemap 0`, `1` |
| `audio-spatial` | `int32` (off, binaural, surround, objects) | carousel | `pc-set-audio-spatial` |
| `hdr-output?`, `paper-white`, `peak`, `black-lift` | `symbol`, `float` x3 (nits; 0 = from display) | calibration page | new calibration setter |
| `dirty?` | `symbol`, not saved | none | set by `on-confirm` and after load |

Defaults: the look in `play.md` (the product is one opinionated look). Rig and parity slots have
their own config dir, so they get an all-off `sandover.gc` seeded, keeping vanilla parity.

### Apply path

One GOAL function `sandover-apply!` pushes every field through the existing forms. It runs from
the Jak 1 `update` path when `dirty?` is set: after load (first frame, so functions defined after
`pckernel` such as `pc-set-shadows` exist) and after each confirm. On-off options also call it
from `on-change` for a live preview; cancel restores the backup and re-applies. No
unconditional per-frame push. No new C++ is needed for the toggles.

### Pages

Top-level entry "SANDOVER" in `*main-options-pc*` and `*main-options-secrets*`, then:

| Screen | Rows |
|---|---|
| `sandover` | Lighting, Materials, Water, World, Display, Audio, Restore defaults (confirmation), Back |
| `sandover-lighting` | Ambient occlusion (carousel), Tile lights, Bloom, Actor shadows |
| `sandover-materials` | Materials, Look (carousel), Fur |
| `sandover-water` | Water |
| `sandover-world` | Neighbour levels and fog |
| `sandover-display` | FP16 target, Tonemap (carousel), Exposure (slider), HDR calibration (menu) |
| `sandover-hdr` | see below |
| `sandover-audio` | Spatial audio (carousel) |

Eight screens: `*options-remap*` goes from 68 to 75. Depth is settings → sandover → display →
hdr = 4, inside the stack of 5. Carousels: add one generic type (e.g. `carousell-int`) with a
`(carousell (array text-id))` field on `game-option` that reads and writes `(pointer int32)`,
instead of five hard-coded cases per list. Text ids in a block of their own (e.g. `#x5a00`
upward) so upstream additions after `#x1619` never collide; add to `text-h.gc`,
`all-types.gc` and the en-US JSON.

### HDR calibration page: what C++ must provide

Nothing exists yet: no HDR swapchain, and `paper_white_nits` is written but never read
(`gfx.h:357`, `kmachine.cpp:1427-1428`).

| Need | Hook (name only) |
|---|---|
| HDR output (DXGI swapchain with GL interop) | `pc-set-hdr-output sym` |
| Display facts: HDR active, min / max / full-frame luminance (DXGI output desc), SDR white level, MHC2 profile present and its values | `pc-get-display-hdr-info (function display-hdr-info symbol)` — the DXGI/MHC2 spike |
| Calibration values: paper white, peak, black lift, in millionths like the other setters | `pc-set-hdr-calibration which v` |
| Test patterns drawn by the final pass (the GOAL font cannot emit values above 1.0): paper white, peak clip, near-black steps | `pc-set-hdr-pattern n` (0 none) |

Page rows: HDR output, Paper white (slider), Peak (slider), Black level (slider), each showing its
pattern while selected (`on-change` sets the pattern, confirm/cancel clears it), plus
"Use display profile" (a button that zeroes the three values so the display facts apply).
Stored values of 0 mean "from display", so a recalibrated monitor is picked up without a menu visit.
