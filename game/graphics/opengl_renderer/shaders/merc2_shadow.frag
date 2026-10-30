#version 410 core

in vec2 vtx_st;

uniform sampler2D tex_T0;
uniform int ignore_alpha;
uniform float caster_sun;  // 1 - cover: the sun map colour, receivers fade the shadow by it
layout(location = 0) out vec4 caster;

void main() {
  // 128 is opaque: cut texels below about an eighth of that
  if (ignore_alpha == 0 && texture(tex_T0, vtx_st).a < 0.06) {
    discard;
  }
  caster = vec4(caster_sun);
}
