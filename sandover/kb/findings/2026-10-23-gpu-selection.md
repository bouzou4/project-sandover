---
title: Which adapter does gk run on, and do slow boots land on a different one?
date: 2026-10-23
issue: ""
status: verified (six boots, one spot); the 2-5x slow boot state did not recur
sha: branch tools/gpu-select (on master 01c4eef56)
---
## Answer (≤5 lines)
All six boots ran on the same adapter: the discrete card that drives the display (DXGI adapter 0,
Windows' high-performance rank 0), with the window on that card's own output. The second discrete
card stayed idle (0 % activity, memory clock parked) and the integrated GPU is not an available
adapter. Frame time ranged 23.47-23.76 ms (1.3 %), drifting with the card's temperature and clock.
No boot was slow, so adapter selection is ruled out for these boots; if the 2-5x state returns, the
adapter line now tells in one read whether it moved, and if it did not, clocks/power are the suspect.

## 1. What gk now logs
After `OpenGL context renderer: ...`, `log_gpu_adapter()` (`game/graphics/opengl_renderer/GpuAdapterLog.cpp`)
prints what the system reports (example from boot 6, LUIDs shortened):

```
GPU adapter: GL context on luid 00000000:000268.., "<model>", 32479 MiB dedicated, PCI bus 3 device 0 function 0, DXGI adapter 0 of 4, high-performance rank 0, node mask 0
GPU adapter: other: luid 00000000:0D431C.., "<model>", 32479 MiB dedicated, PCI bus 7 device 0 function 0, DXGI adapter 1 of 4, high-performance rank 1
GPU adapter: other: luid 00000001:2E0BA3.., "<model>", 32479 MiB dedicated, no PCI address, DXGI adapter 2 of 4, high-performance rank 4
GPU adapter: other: luid 00000000:00038D.., "<model>", 32479 MiB dedicated, no PCI address, DXGI adapter 3 of 4, high-performance rank 3
GPU adapter: window on display "<monitor>" (\\.\DISPLAY2, 3840x2160 at 0,0, primary), output of luid 00000000:000268.. (the GL adapter)
GPU adapter: GL free texture memory 31608 MiB, largest block 31608 MiB (GL_ATI_meminfo)
```

- LUID: `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)` (GL_EXT_memory_object_win32), the same query
  the D3D12 bridge and DLSS use to find the GL adapter. A LUID is fixed for one OS session only;
  across Windows reboots compare the PCI bus.
- PCI address: `D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERADDRESS)` (DXGI has no field for it).
- The two adapters with no PCI address are the indirect-display adapters of the virtual monitor
  drivers installed on the machine; DXGI gives them the render card's description and memory.
- `window on display ... output of luid X`: which adapter's DXGI output owns the window's monitor.
  "(not the GL adapter)" would mean every frame crosses adapters on present.
- Clocks: no GL query reports them. GL_ATI_meminfo (AMD) or GL_NVX_gpu_memory_info (NVIDIA) free
  memory is logged instead.

## 2. Six boots
`sandover-perf --only open-sea-low --no-toggles --frames 60` (game defaults after the load: FSR 3.1
native temporal AA, 3840x2160, vsync and limiter off), one gk per boot, six boots back to back.
Clocks are the driver's own power-management log (AMD ADL `ADL2_New_QueryPMLogData_Get`, sampled
every 0.5 s): the Windows performance counters (`GPU Engine`, `GPU Adapter Memory`) report
utilisation and memory per LUID but no clock. Clock columns are medians over the last 10 s of
samples with the GL card above 50 % activity (the load tail and the timer windows).

| boot | GL adapter | window's display on | frame ms | GPU ms | `fsr3` ms | `present-encode` ms | GFX clock MHz | mem clock MHz | GFX activity % | hot spot °C | second card activity |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | display card (bus 3, DXGI 0) | GL adapter | 23.47 | 23.45 | 1.48 | 2.69 | 3000 | 2505 | 90 | 70 | 0 % |
| 2 | display card (bus 3, DXGI 0) | GL adapter | 23.56 | 23.54 | 1.49 | 2.62 | 2985 | 2505 | 91 | 74 | 0 % |
| 3 | display card (bus 3, DXGI 0) | GL adapter | 23.56 | 23.56 | 1.50 | 2.63 | 2962 | 2505 | 91 | 78 | 0 % |
| 4 | display card (bus 3, DXGI 0) | GL adapter | 23.63 | 23.62 | 1.50 | 2.62 | 2966 | 2505 | 90 | 80 | 0 % |
| 5 | display card (bus 3, DXGI 0) | GL adapter | 23.63 | 23.61 | 1.49 | 2.66 | 2966 | 2505 | 90 | 79 | 0 % |
| 6 | display card (bus 3, DXGI 0) | GL adapter | 23.76 | 23.73 | 1.50 | 2.65 | 2929 | 2505 | 91 | 84 | 0 % |

The LUID was identical in all six. No boot lost its device. The fixed-size passes that grew 3-7x
in the slow boots of 2026-10-22 (`fsr3` 1.6 -> 5-10 ms, `present-encode` 2 -> 13.6 ms) stayed at
1.48-1.50 and 2.62-2.69 ms. The 1.3 % creep from boot 1 to 6 follows the hot spot (70 -> 84 °C)
and the GFX clock (3000 -> 2929 MHz): back-to-back boots heat the card. Another gk instance was up
during part of boots 4-6 (the sampler saw two gk); the second card stayed idle and the numbers
did not jump, so it was not rendering then.

Data: `$SANDOVER_DATA/gpu-select/` (`boot<n>-gk.log`, `boot<n>-adl.csv`, `boot<n>-perf.log`,
`runner.log`) and `$SANDOVER_DATA/perf/gpu-select-boot<n>/`.

## 3. Choosing the adapter
- The exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` already exist in
  `game/main.cpp` (from upstream) and are in gk.exe's export table. They only steer hybrid
  (integrated + discrete) laptops; they cannot pick between two discrete cards.
- Windows per-app graphics preference: there is no entry for gk under
  `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`. That setting (`GpuPreference=2`) picks
  the high-performance adapter for DXGI/D3D; Windows already ranks the display card first here.
- The GL context follows the display (inferred, not tested by moving the window): in all six
  boots the GL adapter was the card driving the window's monitor, so a monitor on the second card
  would likely move gk there.
  A GL program cannot name an adapter portably (WGL_NV_gpu_affinity is NVIDIA-only,
  WGL_AMD_gpu_association creates offscreen contexts only).

## Implications for the design
- Read the `GPU adapter:` lines before trusting a perf number: runs compared must share the PCI
  bus, and the window line must say "(the GL adapter)".
- The slow-boot state of 2026-10-22 is not explained by adapter selection in these boots. Next time
  it shows up, the adapter line plus a clock sample (`adl_sampler`-style, or the driver overlay)
  in the same boot decide between "moved adapter" and "clock/power state".
- Back-to-back boots drift about 0.3 % per boot from heat; a cold card reads about 1 % faster.

## Follow-ups
- If a slow boot recurs with the same adapter: capture the GFX/memory clock and power during it
  (the driver's PM log), and the power plan / PCIe link state.
