#version 430 core
// AMD FidelityFX FSR 1 RCAS (shaders/ffx, MIT, unmodified) after EASU, at display resolution.

uniform sampler2D src;
uniform uvec4 con0;
uniform int headroom;  // 1: FP16 target, values above 1.0

// FSR expects 0..1: with the FP16 target the encoded scene (1.0 = SDR white) is scaled into
// that range by a constant, which both filters are nearly invariant to, and back afterwards
const float kHeadroom = 8.0;
vec4 fsr_fwd(vec4 x) {
  return headroom == 1 ? max(x, vec4(0.0)) * (1.0 / kHeadroom) : x;
}
vec3 fsr_inv(vec3 y) {
  return headroom == 1 ? y * kHeadroom : y;
}

#define A_GPU 1
#define A_GLSL 1
SANDOVER_FFX_A
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
  return fsr_fwd(texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
SANDOVER_FFX_FSR1

out vec4 color;

void main() {
  AF3 c;
  FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), con0);
  vec4 e = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
  // a flat black (or white) ring makes RCAS's limiters 0 x inf: keep the centre texel instead
  // (exponent bits, so no fast-math folding of isnan can drop the test)
  if (any(greaterThanEqual(floatBitsToUint(c) & 0x7fffffffu, uvec3(0x7f800000u)))) {
    c = fsr_fwd(e).rgb;
  }
  color = vec4(fsr_inv(c), e.a);
}
