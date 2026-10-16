#version 410 core
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
SANDOVER_SMAA_CONFIG

uniform sampler2D color_tex;
uniform sampler2D blend_tex;
uniform sampler2D edges_tex;
uniform int smaa_debug;  // 1: edges (red left, green top), 2: blend weights, over the dimmed frame

in vec2 v_uv;
in vec4 v_offset;
out vec4 out_color;

void main() {
  vec4 c = SMAANeighborhoodBlendingPS(v_uv, v_offset, color_tex, blend_tex);
  if (smaa_debug == 1) {
    vec2 e = textureLod(edges_tex, v_uv, 0.0).rg;
    c.rgb = e.r + e.g > 0.0 ? vec3(e, 0.0) : c.rgb * 0.25;
  } else if (smaa_debug == 2) {
    vec4 w = textureLod(blend_tex, v_uv, 0.0);
    c.rgb = w.x + w.y + w.z + w.w > 0.0 ? vec3(w.x + w.y, w.z + w.w, 0.0) * 2.0 : c.rgb * 0.25;
  }
  out_color = c;
}
