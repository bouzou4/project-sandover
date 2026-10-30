---
title: GPU-side hand-overs for the D3D12 bridge (GL memory objects and semaphores) on the AMD GL driver
date: 2026-10-23
issue: ""
status: blocked by the driver; no engine change; decision needed (section 4)
sha: branch opt/bridge-semaphores (on master 01c4eef56)
---
## Answer (≤5 lines)
The planned fix (`2026-10-22-fsr4.md`, open issue 1) does not work on the rig's AMD GL driver
(26.10.1, RDNA 4). Both extensions are exposed and every import call returns without a GL error,
but a texture or buffer on an imported D3D12 resource crashes gk at the next flush, a GL semaphore
signal never reaches the D3D12 fence, and a GL semaphore wait does not hold for a value the fence
has not reached. The interop hand-over stays; nothing was merged. No FSR 4 numbers were taken.

## 1. What was built and tried
Branch code (not committed; kept with the logs in `$SANDOVER_DATA/shots/bridge-sync/direct/`):
`D3d12Bridge.experiments.cpp` (import path with probes), `D3d12Bridge.gpu-wait-selftest.{h,cpp}`
(second design, below). Each run: a direct 30-40 s gk boot of the branch build from a staged root
(`rig-capture.md`, "Worktree builds"), solo on the GPU, logs `gk-*.log` beside them.

- Import path as planned: a committed resource on `D3D12_HEAP_FLAG_SHARED`, `CreateSharedHandle`
  (NT handle), `glCreateMemoryObjectsEXT` + `GL_DEDICATED_MEMORY_OBJECT_EXT`,
  `glImportMemoryWin32HandleEXT(GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, size from
  GetResourceAllocationInfo)`, `glTexStorageMem2DEXT`; the queue's fence and a GL fence, both
  shared, imported with `glImportSemaphoreWin32HandleEXT(GL_HANDLE_TYPE_D3D12_FENCE_EXT)`; value
  set with `glSemaphoreParameterui64vEXT(GL_D3D12_FENCE_VALUE_EXT)`. A start-up self-test (4x4
  texture: GL clears it, the queue copies it out and writes another colour, GL reads it back, with
  bounded waits) was to decide between it and the interop.
- First rig probe: `RIG:CRASH slot0 boot` twice (no listener after 120 s); the log stops after
  `dlss: unavailable`, i.e. in the bridge's start-up.

## 2. Driver behaviour, measured (each line one or more boots)
| Step | Result |
|---|---|
| `glImportSemaphoreWin32HandleEXT`, D3D12 fence type, by handle or by name; also `OPAQUE_WIN32` | no GL error, `glIsSemaphoreEXT` true |
| `glImportMemoryWin32HandleEXT`, `D3D12_RESOURCE`, `OPAQUE_WIN32` or `D3D11_IMAGE` type, dedicated or not, size or 0, simultaneous-access resource | no GL error; `glFinish` after it returns |
| `glTexStorageMem2DEXT` (also with `GL_TEXTURE_TILING_EXT` optimal, or `glTextureStorageMem2DEXT`) or `glBufferStorageMemEXT` on that memory | segfault at the next `glFinish` (rc 139), with or without semaphores imported, with or without a draw into it |
| `glSignalSemaphoreEXT` on an imported D3D12 fence, values 1 and 100-102, `glFinish` + 50 ms after each | the fence's completed value stays 0 (all three import variants); a queue `Wait` on it never passes |
| `glWaitSemaphoreEXT` for a value the fence has not reached (offsets 1, 2, 3, 5 over the completed value; queue or CPU signal afterwards; before or after a second shared GL context exists) | does not hold: a GL fence right after it completes within 50-300 ms (`GL_CONDITION_SATISFIED`) in every case |

One early boot showed a wait holding (300 ms timeout, released after a CPU signal); it ran after
the stuck semaphore signals in the same context and did not reproduce in four later boots, so it
is not counted.

## 3. Second design (also blocked)
Interop-registered textures kept locked for GL (the queue uses the D3D12 resources directly), GL to
queue through a GL fence that a worker thread with a shared GL context waits for and then signals
a plain D3D12 fence from the CPU (the queue waits on it), queue to GL through `glWaitSemaphoreEXT`
on the queue's fence. Its self-test fails at the first check (the wait does not hold), so it falls
back cleanly: `d3d12: GPU-wait hand-overs unavailable (self-test: the GL semaphore wait did not hold
and release (0x911c, 0x911a)); interop lock in their place`, FSR 4 then starts as on master.
Without a working GL-side GPU wait the render thread must still block before GL's commands after
the dispatch, until the queue (which waits for GL's frame so far) is done: the CPU stall moves from
the hand-over to the hand-back instead of going away, so the worker alone buys nothing.

Row flip: unchanged by any of this. Both APIs see the same memory with row 0 first; D3D's row 0 is
the top of the image and a GL framebuffer's the bottom, so the input pass and the output blit keep
flipping (both GPU copies already, 0.04 and 0.23 ms at 4K).

## 4. Options (decision needed)
1. Recommended: keep the interop hand-over (master); report the three driver faults to AMD with
   the probe; re-run the self-test on each new driver and on NVIDIA/Intel GPUs (where the DLSS
   port also needs the bridge). The self-test code is ready to drop in.
2. Ship the self-tested GPU-wait design dormant: it would turn itself on only where a driver
   passes the test. Unproven on any GPU here, so not recommended.
3. A Vulkan intermediary (Vulkan imports the D3D12 fence and resources, GL imports from Vulkan's
   opaque handles, AMD's documented GL interop use): a larger spike; GL-from-Vulkan semaphores are
   binary, so fence values need mapping. Only worth it if FSR 4's +2.2 ms over FSR 3.1 matters
   enough for a multi-day spike.

## 5. Not done (blocked by 2)
FLIP parity, the stall before/after and frame ms (FSR 4 at 67 %, two spots), the present path
check (a local merge of `feat/present-flip` onto the branch was built for it, not run), PR ready.
