#version 410 core

// bloom bright pass at half resolution: ramp(E) x (colour above threshold), 2x2 box of full-res pixels
out vec4 color;

uniform sampler2D color_tex;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;
uniform int em_samples;
uniform float threshold;
uniform vec2 em_ramp;  // same ramp as the SSAO exclusion
uniform uint perf_opt;

EMISSIVE_MASK_FRAG
void main() {
  ivec2 base = ivec2(gl_FragCoord.xy) * 2;
  ivec2 hi = textureSize(color_tex, 0) - ivec2(1);
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 4; i++) {
    ivec2 p = min(base + ivec2(i & 1, i >> 1), hi);
    float e = smoothstep(em_ramp.x, em_ramp.y, emissive(p));
    // perf-opt bit 18: no colour fetch where the mask is 0
    if (e == 0.0 && (perf_opt & (1u << 18)) != 0u) {
      continue;
    }
    vec3 c = texelFetch(color_tex, p, 0).rgb;
    sum += e * max(c - vec3(threshold), vec3(0.0));
  }
  color = vec4(sum * 0.25, 1.0);
}
