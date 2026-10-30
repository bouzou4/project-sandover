#version 410 core

layout(location = 0) out vec4 color;
// emissive mask, draw buffer 2 only while bloom is on (Sprite3::flush_sprites)
layout(location = 2) out vec4 emissive_out;
uniform float em_out;  // 1: additive particle, E += brightest channel; 0: clear (HUD)
uniform float em_table;   // > 0: listed light texture, E = em_table x coverage (max blend)
uniform float em_chroma;  // listed texture: vertex colours less saturated than this write no E
uniform float em_unlit;   // listed texture: share of E that gives no tile light (mask green)
// draw buffer 4 while tile lights are on: 1 / z (metres) of sprites bright enough to emit, max blend
layout(location = 4) out vec4 sz_out;
uniform vec3 sz_lin;  // ssao_linearize.frag lin
uniform float sz_min_e;

flat in vec4 fragment_color;
in vec3 tex_coord;
flat in uvec2 tex_info;

uniform sampler2D tex_T0;
uniform float alpha_min;
uniform float alpha_max;
in float atmo_f;
in float haze_f;
in float van_f;   // the game's fog (fog floor only)
in float mist_f;  // the swamp mist
in float mist_g;  // its forward glow toward the sun, as swamp_mist.frag
uniform vec3 sw_mist_col;
uniform vec3 sw_sun_col;  // hue, max component 1
uniform vec3 haze_color;
uniform vec4 atmo_color;  // rgb fog colour, a: 1 for additive draws (fade instead of tint)

void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy);
  if (tex_info.y == 0) {
    T0.w = 1.0;
  }
  color = fragment_color * T0;
  if (atmo_color.a > 0.5) {
    color.rgb *= 1. - atmo_f;
    color.rgb *= 1. - haze_f;
  } else {
    // blended sprites fog like the surfaces they sit on: at least the game's fog, then the mist
    color.rgb = mix(color.rgb, atmo_color.rgb, max(atmo_f, van_f));
    color.rgb = mix(color.rgb, haze_color, haze_f);
    color.rgb = mix(color.rgb, sw_mist_col * (1. + 0.6 * mist_g * sw_sun_col), mist_f);
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // 0.4: large additive smoke stays under the bloom ramp, bright cores exceed it
  float e = 0.4 * em_out * max(color.r, max(color.g, color.b));
  if (em_table > 0.0) {
    vec3 v = fragment_color.rgb;
    float hi = max(v.r, max(v.g, v.b));
    float chroma = hi > 0.0 ? (hi - min(v.r, min(v.g, v.b))) / hi : 0.0;
    // full E wherever the sprite adds at least a quarter of white
    float cov = clamp(4.0 * max(color.r, max(color.g, color.b)) * color.a, 0.0, 1.0);
    e = chroma >= em_chroma ? em_table * cov : 0.0;
  }
  emissive_out = vec4(e, em_table > 0.0 ? e * em_unlit : 0.0, 0.0, color.a);
  float iz = 4096.0 * abs(sz_lin.y * (2.0 * gl_FragCoord.z - 1.0) - sz_lin.z) / abs(sz_lin.x);
  float lit = em_table > 0.0 ? e - e * em_unlit : e;
  sz_out = vec4(lit > 0.0 && lit >= sz_min_e && sz_lin.x != 0.0 ? iz : 0.0);
}
