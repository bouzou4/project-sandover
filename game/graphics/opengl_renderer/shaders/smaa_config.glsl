// SMAA 1x settings for the three smaa_* passes. SMAA.hlsl is the reference implementation by
// Jimenez et al. (MIT licence in its header and in third-party/smaa/LICENSE.txt).
uniform vec4 smaa_rt_metrics;  // 1/w, 1/h, w, h
uniform float smaa_threshold;
#define SMAA_RT_METRICS smaa_rt_metrics
#define SMAA_THRESHOLD smaa_threshold
#define SMAA_MAX_SEARCH_STEPS 32
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25
#ifndef SMAA_CUSTOM_SL
#define SMAA_GLSL_4
#endif
SANDOVER_SMAA_HLSL
