#version 410 core

layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG
// lava E (r, g; lava draws while the glow is on) and the highlight material (b, a; while the
// tile lights use it); Shrub.cpp masks what a draw does not write
layout(location = 2) out vec4 emissive_out;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
PBR_BG_FRAG
uniform sampler2D tex_T0;
vec4 pbr_albedo_grad(vec2 uv, vec2 dx, vec2 dy) { return textureGrad(tex_T0, uv, dx, dy); }

uniform float alpha_min;
uniform float alpha_max;
uniform vec4 fog_color;
in float haze_f;
in float fog_tw;
uniform vec3 fog_tint;  // neighbour tint - 1
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
  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = texture(tex_T0, tex_coord.xy);
    color = (fragment_color + vec4(pbr_caustic_lit(fragment_color.rgb, T0.rgb), 0.0)) * T0;
  } else {
    color = fragment_color;
  }

  // alpha test ahead of the shadow taps (perf opt); the test below stays for the opt-off path
  if (shadow_skip != 0 && (color.a < alpha_min || color.a > alpha_max)) {
    discard;
  }

  if (shadow_on == 1) {
    float occ = shadow_sun(shadow_pos, 0.0003);
    bool ws = (ws_on & 1) != 0;
    // the map acts only through the removed share (0 at share 0) and the PBR sun term
    bool ws_now = ws && (ws_gate == 0 || shadow_view != 0 ||
                         shadow_receive * shadow_strength != 0.0 && occ < 1.0 &&
                             (shadow_share > 0.0 || shadow_share1 < 0.0 || pbr_sun_occ_read()));
    occ = ws_now ? max(occ, 1.0 - ws_world_vis(shadow_pos)) : occ;
    vec3 canopy = shadow_canopy_factor(shadow_pos, occ);
    // world map: only the direct share the bake still holds goes (WorldShadows.h)
    float ks = ws ? ws_removed(shadow_share, shadow_share1, occ) : shadow_share * occ;
    // the PBR sun term (glints, highlights) sees the leaves as well as the actor shadow
    float sun_x = occ * shadow_receive * shadow_strength;
    pbr_sun_occ = sun_x + shadow_receive * (1.0 - shadow_canopy_sun_vis) * (1.0 - sun_x);
    float lum = dot(fragment_color.rgb, vec3(0.299, 0.587, 0.114));
    float k = shadow_receive * shadow_strength *
              (ks + shadow_lamps_removed(shadow_pos, lum));
    if (shadow_view == 1) {
      color.rgb = vec3(1.0 - occ);
    } else if (shadow_view == 2) {
      color.rgb = vec3(shadow_share);
    } else if (shadow_view == 3) {
      color.rgb = (shadow_mtx_sun * vec4(shadow_pos, 1.0)).xyz * 0.5 + 0.5;
    } else if (shadow_view == 4) {
      color.rgb = shadow_local_debug_bg(shadow_pos, color.rgb);
    } else {
      color.rgb *= shadow_keep(k, shadow_receive * shadow_strength * ks,
                               shadow_receive * shadow_marker(shadow_pos), shadow_share, color.rgb);
      color.rgb *= mix(vec3(1.0), canopy, shadow_receive);  // emitters keep their light
    }
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  if (pbr_on != 0) {
    pbr_fog_keep = (1.0 - clamp(fogginess * fog_color.a, 0.0, 1.0)) * (1.0 - haze_f);
    color.rgb = pbr_background(color.rgb, T0.rgb, fragment_color.rgb, tex_coord.xy);
  }
  color.xyz = mix(color.xyz, fog_color.rgb * (1. + fog_tw * fog_tint), clamp(fogginess * fog_color.a, 0, 1));
  color.xyz = mix(color.xyz, haze_color, haze_f);
  ssao_normal = ssao_normal_fog(ssao_normal, color.a, (1. - clamp(fogginess * fog_color.a, 0, 1)) * (1. - haze_f));
  inspect_write(tex_coord.xy);
  emissive_out = vec4(lava_e, lava_e, pbr_hl);
}
