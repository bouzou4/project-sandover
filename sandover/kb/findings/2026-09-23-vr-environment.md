---
title: PC VR environment audit for the PSVR2 stereo phase
date: 2026-09-23
status: verified
sha: N/A (system/environment survey, not engine code)
---
> Editorial note (2026-09-23, post-ADR-0004): the VR scope decision dropped eye tracking,
> gaze and PSVR2Toolkit entirely (stereo 3D via OpenXR + SteamVR only). The gaze-specific
> findings below are kept as a record of the environment audit; treat them as refuted/moot,
> not as a plan item. The SteamVR/OpenXR runtime-contention and GPU driver-branch findings
> still stand.

## Answer (≤5 lines)
Steam, SteamVR and the PlayStation VR2 App are installed and fully staged. The active OpenXR runtime is SteamVR. The PSVR2 PC adapter's own driver and controllers are recognized by Windows (idle/disconnected right now, which is expected when the headset isn't plugged in). PSVR2Toolkit is present, installed as a drop-in replacement inside the official PSVR2 SteamVR driver folder, exposing gaze through its own IPC/C API — **not** through OpenXR — confirmed against the upstream source tree, not memory. Two other VR runtimes (a Meta/Oculus runtime and Virtual Desktop) are installed and both register OpenXR runtimes/layers, which is a real interference risk for the stereo phase.

## Evidence (path:line@sha / URL, short excerpts)

| # | Check | Status | Result |
|---|---|---|---|
| 1 | Steam + SteamVR installed | verified | Steam install found via registry; SteamVR app manifest present, state flags indicate fully installed; SteamVR library `libraryfolders.vdf` lists 6 Steam library folders across several drives. |
| 1 | SteamVR version | verified | SteamVR `bin/version.txt` timestamp decodes to 2026-09-15. Build ID present in the app manifest. |
| 2 | PlayStation VR2 App (Steam app 2580190) installed | verified | App manifest present, state flags = fully installed. Its own `version.txt` reports version **3.2.0**. |
| 3 | PSVR2 PC adapter driver / device | verified | `Get-PnpDevice` shows a full set of "PS VR2 …" USB composite/data/status/audio nodes and two "PlayStation VR2 Sense Controller" entries. Data/status/audio nodes show Sony Interactive Entertainment Inc. as driver provider, driver version 1.0.1.4. Most nodes report status "Unknown" (i.e., not currently connected/powered — consistent with the headset being unplugged, not a driver problem); the two Sense controllers connected over Bluetooth report "OK". |
| 4 | OpenXR active runtime | verified | `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` points at the SteamVR OpenXR runtime JSON, which declares itself as the SteamVR runtime. |
| 4 | OpenXR AvailableRuntimes | verified | Three runtime JSONs are registered as available (not necessarily active): the SteamVR runtime, a Meta/Oculus runtime, and a Virtual Desktop runtime. |
| 4 | OpenXR implicit API layers | verified | One implicit layer is registered: an "oculus compatibility" shim shipped by the Virtual Desktop installation. |
| 5 | PSVR2Toolkit installed? | verified | Not a standalone install — it is layered directly into the PlayStation VR2 App's own SteamVR driver folder: the original Sony driver binary is preserved alongside a replacement of the same name, plus a cross-process IPC DLL and PSVR2Toolkit's own C-API DLL. A per-session marker file (written by its SteamVR driver hook when SteamVR last ran) points at that same folder, last updated 3 days before this check — i.e., not currently loaded, but functional and previously exercised. |
| 5 | PSVR2Toolkit version | inferred | No embedded version metadata on the installed binaries. Install timestamps on the DLLs sit 2 days after the "v1.0.0 Experimental #2" GitHub release and well after the last non-prerelease "v0.2.1"; most likely Experimental #2, not confirmed. |
| 5 | PSVR2Toolkit gaze API = custom IPC, not OpenXR | verified | Confirmed two independent ways: (a) the upstream repo's source tree (`projects/`) contains `libcrossipc`, `psvr2_toolkit_capi`, `psvr2_toolkit_capi_loader`, `psvr2_toolkit_legacy_ipc_server`, and `psvr2_openvr_driver_ex` — no OpenXR extension implementation anywhere in that tree; (b) locally, prior project code that already integrates PSVR2Toolkit gaze documents loading `psvr2_toolkit_capi.dll` directly via `LoadLibraryEx` plus a `%TEMP%` path marker file, describing this explicitly as "the PSVR2Toolkit C API" rather than an OpenXR interaction profile. The GitHub README itself only says "our own API library" without naming OpenXR, consistent with this. |
| 5 | PSVR2Toolkit license note | verified | Upstream README states eye-tracking data "must not be used in commercial environments and/or for commercial purposes"; project is stated as intended for non-commercial use only. |
| 6 | Other VR software present | verified | A Meta/Oculus runtime component and both a Virtual Desktop service and a Virtual Desktop streamer are installed (visible both as registered uninstallable applications and as the "Virtual Desktop Monitor" / "Meta Virtual Monitor" display adapters, plus associated virtual audio and virtual gamepad devices). No OpenComposite or OpenXR Toolkit installation was found. |
| 7 | GPU driver / branch | verified | The installed NVIDIA driver reports as 32.0.16.1656 in the Windows driver store, which NVIDIA's own tool reports as version 616.56 — i.e., the **616.x** branch, not 566.x as previously assumed. This should be corrected in any doc that referenced 566.x. |
| 7 | SteamVR GPU selection | inferred | Only one discrete GPU is present on the system, so SteamVR has no alternative GPU to select; not independently verified against a SteamVR log. |

## Implications for the design
- **No installs needed** to reach the stereo spike: SteamVR and the PlayStation VR2 App are already in place from prior work. The stereo spike itself needs no headset connection (per the original specification), so the current "idle/disconnected" PnP state is not a blocker.
- Moot: gaze integration is out of scope per ADR-0004 (no eye tracking, no PSVR2Toolkit). The stack's
  PSVR2Toolkit install and its custom-IPC gaze API are recorded here only as historical evidence.
- **Runtime contention risk for OpenXR-based work:** with a Meta/Oculus runtime and Virtual Desktop both registering OpenXR runtimes and Virtual Desktop additionally registering an implicit API layer, any OpenXR-based capture or spike should explicitly re-check `ActiveRuntime` before running, since a Virtual Desktop or Oculus/Meta session could silently switch it away from SteamVR.
- **GPU driver branch was previously misrecorded** as 566.x; it is actually the 616.x branch. Any compatibility notes keyed to a driver branch should use 616.x.
- Moot: PSVR2Toolkit's exact installed version no longer matters, since gaze is out of scope per ADR-0004.

## Follow-ups
- Moot: no further PSVR2Toolkit follow-up is needed; gaze is out of scope per ADR-0004.
- If/when an OpenXR-based spike is run, capture the `ActiveRuntime` value at that moment as a sanity check, since it can be changed by other installed VR software.
- No admin-level or destructive action is needed before the stereo spike; this check found no blockers to stereo (stereo spike, no headset needed).
