#version 410 core

layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG
CAVE_DARK_FRAG
in vec3 vtx_color;
in vec2 vtx_st;
in float fog;
in float haze_f;
in vec3 emerc_cpos;


uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;

uniform int decal_enable;

uniform int gfx_hack_no_tex;
uniform int merc_emitter;  // emitters keep their shine


void main() {
  ssao_normal = ssao_normal_value();
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);

    color.a = T0.a;
    color.rgb = T0.rgb * vtx_color;
    color *= 2;
  } else {
    color.rgb = vtx_color;
    color.a = 1;
  }
  color.rgb *= merc_emitter == 0 ? cave_dark(emerc_cpos) : 1.0;
  color.rgb *= 1. - haze_f;
  ssao_normal = ssao_normal_fog(ssao_normal, color.a, (1. - clamp(fog_color.a * fog, 0, 1)) * (1. - haze_f));
  inspect_write(vtx_st);
}
