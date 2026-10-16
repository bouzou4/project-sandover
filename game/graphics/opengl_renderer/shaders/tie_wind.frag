#version 410 core

layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG
PBR_BG_FRAG
// highlight material only: none (Tie3.cpp masks r and g)
layout(location = 2) out vec4 emissive_out;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;
vec4 pbr_albedo_grad(vec2 uv, vec2 dx, vec2 dy) { return textureGrad(tex_T0, uv, dx, dy); }

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;
in float haze_f;
in float fog_tw;
in vec3 canopy_wpos;
in float shadow_share1;  // direct-sun share estimated from the palette (world_shadow_vert.glsl)
uniform float shadow_strength;
CANOPY_FRAG
WORLD_SHADOW_FRAG
uniform int canopy_emitter;  // additive, subtractive or emissive-table draws keep their light
uniform vec3 fog_tint;  // neighbour tint - 1
uniform vec3 haze_color;

uniform int gfx_hack_no_tex;


void main() {
  ssao_normal = ssao_normal_value();
  ws_begin(canopy_wpos);
  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  float canopy_occ = 0.0;
  if (canopy_emitter == 0) {
    // the map's only use: the removed share below
    canopy_occ = ws_gate == 0 || shadow_strength * shadow_share1 != 0.0
                     ? 1.0 - ws_world_vis(canopy_wpos) : 0.0;
    color.rgb *= shadow_canopy_factor(canopy_wpos, canopy_occ);
    color.rgb *= max(1.0 - shadow_strength * shadow_share1 * canopy_occ, 0.0);  // 0 without the map
  }
  if (pbr_on != 0) {
    color.rgb = pbr_background(color.rgb, T0.rgb, fragment_color.rgb, tex_coord.xy);
  }
  color.rgb = mix(color.rgb, fog_color.rgb * (1. + fog_tw * fog_tint), clamp(fogginess * fog_color.a, 0, 1));
  color.rgb = mix(color.rgb, haze_color, haze_f);
  ssao_normal = ssao_normal_fog(ssao_normal, color.a, (1. - clamp(fogginess * fog_color.a, 0, 1)) * (1. - haze_f));
  inspect_write(tex_coord.xy);
  emissive_out = vec4(0.0, 0.0, 0.0, 1.0);
}
