#version 410 core

// sky stars (SunDisc.cpp, style 1): Jak 3's sky-hotdot, added over sky samples
layout(location = 0) out vec4 color;

uniform sampler2D dot_tex;  // coverage, peak 1

in float v_lum;

void main() {
  float g = texture(dot_tex, gl_PointCoord).r;
  color = vec4(pow(vec3(0.92, 0.96, 1.0) * (v_lum * g), vec3(1.0 / 2.2)), 0.0);
}
