#version 410 core

// bloom composite, blended GL_ONE, GL_SRC_ALPHA with alpha 1: dst' = dst + add, add >= 0
out vec4 color;

uniform sampler2D src;
uniform sampler2D frame;  // the frame before bloom (resolved)
uniform float strength;
uniform int hue_clip;
uniform vec2 white;  // max channel where the hue-kept sum starts / finishes turning to min(sum, 1)
uniform uint perf_opt;

void main() {
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0) * 2);
  vec3 b = strength * texture(src, uv).rgb;
  // perf-opt bit 18: adding 0 leaves the frame as it is; skip the MSAA read-modify-write
  if (b == vec3(0.0) && (perf_opt & (1u << 18)) != 0u) {
    discard;
  }
  if (hue_clip != 0) {
    vec3 base = min(texelFetch(frame, ivec2(gl_FragCoord.xy), 0).rgb, vec3(1.0));
    vec3 sum = base + b;
    float m = max(sum.r, max(sum.g, sum.b));
    // over 1: add only what the hue-scaled sum has above the frame; never darken a channel
    if (m > 1.0) {
      vec3 t = mix(sum / m, min(sum, vec3(1.0)), smoothstep(white.x, max(white.y, white.x * 1.0001 + 1e-4), m));
      b = max(t - base, vec3(0.0));
    }
  }
  color = vec4(b, 1.0);
}
