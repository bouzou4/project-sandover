#version 410 core

layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG

in vec4 fragment_color;
in vec3 tex_coord;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;

void main() {
  ssao_normal = ssao_normal_value();
  color = fragment_color;
  ssao_normal.a = color.a;
  inspect_write(vec2(0.0));  // no texture coordinate in this debug shader
}
