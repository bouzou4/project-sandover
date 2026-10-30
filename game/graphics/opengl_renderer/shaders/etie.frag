#version 410 core

layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
in float atmo_fade;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;
in float haze_f;
uniform vec3 haze_color;

uniform int gfx_hack_no_tex;

in vec3 shadow_pos;
in float shadow_share;
in float shadow_share1;
in float shadow_receive;

ACTOR_SHADOW_FRAG


void main() {
  ssao_normal = ssao_normal_value();
  ws_begin(shadow_pos);
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  // alpha test ahead of the shadow taps (perf opt); the test below stays for the opt-off path
  if (shadow_skip != 0 && (color.a < alpha_min || color.a > alpha_max)) {
    discard;
  }

  // the additive reflection darkens with the base; debug views come from the base pass
  if (shadow_on == 1) {
    float occ = shadow_sun(shadow_pos, 0.0003);
    bool ws = (ws_on & 1) != 0;
    // the map acts only through the removed share, 0 at share 0
    bool ws_now = ws && (ws_gate == 0 || shadow_receive * shadow_strength != 0.0 && occ < 1.0 &&
                                             (shadow_share > 0.0 || shadow_share1 < 0.0));
    occ = ws_now ? max(occ, 1.0 - ws_world_vis(shadow_pos)) : occ;
    vec3 canopy = shadow_canopy_factor(shadow_pos, occ);
    float k = shadow_receive * shadow_strength *
              (ws ? ws_removed(shadow_share, shadow_share1, occ) : shadow_share * occ);
    color.rgb *= shadow_view == 0 ? shadow_keep(k, k, shadow_receive * shadow_marker(shadow_pos),
                                              shadow_share, color.rgb)
                                : vec3(0.0);
    color.rgb *= shadow_view == 0 ? mix(vec3(1.0), canopy, shadow_receive) : vec3(1.0);
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
  color.rgb *= 1. - clamp(atmo_fade * fog_color.a, 0., 1.);
  color.rgb *= 1. - haze_f;
  ssao_normal = ssao_normal_fog(ssao_normal, color.a, (1. - clamp(fogginess * fog_color.a, 0, 1)) *
                                (1. - clamp(atmo_fade * fog_color.a, 0., 1.)) * (1. - haze_f));
  inspect_write(tex_coord.xy);
}
