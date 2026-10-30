#version 410 core

// contact shadows (ContactShadows.h), composite: a depth-aware 3x3 of the accumulated term,
// multiplied into the frame (or a debug view)
out vec4 color;

uniform sampler2D cs_hist;  // r term, g view depth (m)
uniform sampler2D cs_raw;
uniform int cs_view;
uniform vec2 cs_size;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 hi = ivec2(cs_size) - 1;
  if (cs_view == 1 || cs_view == 2 || cs_view == 4) {
    color = vec4(vec3(texelFetch(cs_raw, p, 0).r), 1.0);
    return;
  }
  vec2 c = texelFetch(cs_hist, p, 0).rg;
  float sum = 0.0, wsum = 0.0;
  for (int i = 0; i < 9; i++) {
    vec2 s = texelFetch(cs_hist, clamp(p + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), hi), 0).rg;
    float w = (i == 4 ? 2.0 : 1.0) * max(1.0 - abs(s.g - c.g) / (0.02 * c.g + 0.02), 0.0);
    sum += s.r * w;
    wsum += w;
  }
  float f = 1.0 - clamp(sum / max(wsum, 1e-4), 0.0, 1.0);
  color = vec4(f, f, f, 1.0);
}
