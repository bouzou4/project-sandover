#version 410 core

// sun-map occupancy (ActorShadows::build_occupancy). pass 0: min depth per 8x8 texels;
// pass 1: min of the 6x6 pass-0 texels around a 32-texel cell (3x3 PCF footprint plus rounding)
layout(location = 0) out float min_depth;
uniform sampler2D tex_T12;  // pass 0: sun map, raw (compare off through a sampler object)
uniform sampler2D tex_T15;  // pass 1: the pass-0 grid
uniform int pass;

void main() {
  ivec2 c = ivec2(gl_FragCoord.xy);
  float m = 1.0;
  if (pass == 0) {
    vec2 texel = 1.0 / vec2(textureSize(tex_T12, 0));
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
        // gather at the shared corner of a 2x2 texel block
        vec4 g = textureGather(tex_T12, (vec2(c * 8 + ivec2(x, y) * 2) + 1.0) * texel);
        m = min(m, min(min(g.x, g.y), min(g.z, g.w)));
      }
    }
  } else {
    ivec2 size = textureSize(tex_T15, 0);
    for (int y = -1; y <= 4; y++) {
      for (int x = -1; x <= 4; x++) {
        m = min(m, texelFetch(tex_T15, clamp(c * 4 + ivec2(x, y), ivec2(0), size - 1), 0).r);
      }
    }
  }
  min_depth = m;
}
