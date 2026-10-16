#version 430 core
// AMD FidelityFX FSR 1 EASU (shaders/ffx, MIT, unmodified) on the display-encoded scene.

uniform sampler2D src;
uniform uvec4 con0;
uniform uvec4 con1;
uniform uvec4 con2;
uniform uvec4 con3;
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
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return fsr_fwd(textureGather(src, p, 0)); }
AF4 FsrEasuGF(AF2 p) { return fsr_fwd(textureGather(src, p, 1)); }
AF4 FsrEasuBF(AF2 p) { return fsr_fwd(textureGather(src, p, 2)); }
SANDOVER_FFX_FSR1

uniform vec2 out_size;
out vec4 color;

void main() {
  AF3 c;
  FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
  // destination alpha is read by PS2 blending in the text buckets drawn after this
  color = vec4(fsr_inv(c), textureLod(src, gl_FragCoord.xy / out_size, 0.0).a);
}
