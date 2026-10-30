AMD FSR SDK (FidelityFX SDK) v2.3.0, commit 60f4ea81909200d8542eca14dccb2628b763a9a3.
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0

Vendored, unmodified (MIT, LICENSE.txt; the SDK's docs/license.md lists these paths under MIT):
- include/api/include/ffx_api.h, ffx_api_types.h, dx12/ffx_api_dx12.h  (Kits/FidelityFX/api/include)
- include/upscalers/include/ffx_upscale.h                             (Kits/FidelityFX/upscalers/include)

Fetched at configure time by fetch.cmake (pinned commit, SHA-256 per file), not committed:
- amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll (Kits/FidelityFX/signedbin,
  AMD-signed, also MIT). FSR 4 ships only as these signed DLLs; they are used unmodified and
  loaded at run time with LoadLibrary (gk does not import them). The build copies both beside gk;
  the package ships them with LICENSE.txt as AMD-FSR-LICENSE.txt.

Configure with -DSANDOVER_FSR4=OFF to build without the FSR 4 mode (it then reports unavailable).
SANDOVER_FSR4_SDK_DIR (cache or environment) points at a folder that already holds the DLLs, for
offline builds; it is checked against the same hashes.
The engine side is game/graphics/opengl_renderer/Fsr4Upscaler.cpp.
