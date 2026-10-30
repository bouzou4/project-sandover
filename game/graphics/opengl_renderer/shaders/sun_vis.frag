#version 410 core

// sun disc visibility (SunDisc.cpp): share of 21 depth taps over the disc that see the sky
// (depth still the clear value 0); blended over the previous frames' value by the pass
out vec4 color;

uniform sampler2DMS depth_ms;
uniform sampler2D depth_ss;
uniform int ms;
uniform vec2 centre;  // render pixels
uniform float radius;
uniform ivec2 size;

float sky(vec2 p) {
  ivec2 q = clamp(ivec2(floor(p)), ivec2(0), size - ivec2(1));
  float d = ms != 0 ? texelFetch(depth_ms, q, 0).r : texelFetch(depth_ss, q, 0).r;
  return d <= 0.0 ? 1.0 : 0.0;
}

void main() {
  float v = sky(centre);
  for (int i = 0; i < 8; i++) {
    float a = 0.7853982 * float(i);
    v += sky(centre + 0.5 * radius * vec2(cos(a), sin(a)));
  }
  for (int i = 0; i < 12; i++) {
    float a = 0.5235988 * (float(i) + 0.5);
    v += sky(centre + 0.95 * radius * vec2(cos(a), sin(a)));
  }
  color = vec4(v / 21.0, 0.0, 0.0, 1.0);
}
