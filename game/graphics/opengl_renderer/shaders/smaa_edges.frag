#version 410 core
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#define SMAA_CUSTOM_SL 1

// The scene is display-encoded (1.0 = SDR white): identity up to 1, a log shoulder for the FP16
// headroom above it, so edge contrast is measured on a perceptual scale.
vec4 smaa_perceptual(vec4 c) {
  vec3 x = max(c.rgb, vec3(0.0));
  return vec4(mix(x, 1.0 + 0.25 * log2(max(x, vec3(1.0))), step(1.0, x)), c.a);
}

// SMAA_GLSL_4 macros, with the edge reads going through smaa_perceptual
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAASample(tex, coord) texture(tex, coord)
#define SMAASamplePoint(tex, coord) smaa_perceptual(textureLod(tex, coord, 0.0))
#define SMAASampleOffset(tex, coord, offset) texture(tex, coord, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define SMAAGather(tex, coord) textureGather(tex, coord)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

SANDOVER_SMAA_CONFIG

uniform sampler2D color_tex;
uniform int smaa_luma;  // 1: luma edges, otherwise colour edges

in vec2 v_uv;
in vec4 v_offset[3];
out vec4 out_edges;

void main() {
  vec2 e = smaa_luma == 1 ? SMAALumaEdgeDetectionPS(v_uv, v_offset, color_tex)
                          : SMAAColorEdgeDetectionPS(v_uv, v_offset, color_tex);
  out_edges = vec4(e, 0.0, 0.0);
}
