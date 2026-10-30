NVIDIA DLSS SDK (NGX), github.com/NVIDIA/DLSS, tag v310.9.1. Not vendored.

The SDK's licence (NVIDIA RTX SDKs License, LICENSE.txt in the SDK) allows distributing its files
only as incorporated into the built application, so the headers, the import library and the
runtime are fetched at configure time by fetch.cmake (pinned tag, SHA-256 per file) into the build
tree, or taken from SANDOVER_DLSS_SDK_DIR. The build links nvsdk_ngx_d.lib into gk (a loader; gk
does not import nvngx_dlss.dll) and copies nvngx_dlss.dll next to it for local runs. The release
zip does not ship the DLL: players place their own nvngx_dlss.dll next to gk.exe, and without it
DLSS reports unavailable.

Files used: include/nvsdk_ngx*.h (ngx, defs, params, helpers, helpers_d3d, helpers_cuda),
lib/Windows_x86_64/x64/nvsdk_ngx_d.lib (dynamic C runtime), lib/Windows_x86_64/rel/nvngx_dlss.dll.

Configure with -DSANDOVER_DLSS=OFF to build without the DLSS mode (it then reports unavailable).
The engine side is game/graphics/opengl_renderer/DlssUpscaler.cpp.
