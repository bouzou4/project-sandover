AMD FidelityFX SDK 1.1.4 (MIT, LICENSE.txt), GPU headers only, unmodified:
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4/sdk/include/FidelityFX/gpu

- ffx_*.h: core (ffx_core.h and the files it includes for each target)
- fsr3upscaler/: the FSR 3.1 upscaler passes (3.1.4), without the HLSL and Vulkan GLSL callbacks
- spd/ffx_spd.h, spd/ffx_spd_resources.h: the downsampler the luma pyramid uses
- fsr1/ffx_fsr1.h: RCAS, used by the FSR 3.1 sharpening pass

The OpenGL callbacks and pass entry points are the engine's backend:
game/graphics/opengl_renderer/shaders/fsr3/ and Fsr3Upscaler.cpp.
