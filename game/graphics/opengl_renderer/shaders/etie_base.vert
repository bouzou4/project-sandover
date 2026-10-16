#version 410 core
SSAO_NORMAL_VERT
INSPECT_VERT

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in int time_of_day_index;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;
uniform usamplerBuffer tex_T11;  // .unbake records, see background/Unbake.h

PBR_BG_VERT

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

// actor shadows: receiver position, global sun share, emissive gate
uniform float shadow_share_global;
WORLD_SHADOW_VERT
out vec3 shadow_pos;
out float shadow_share;
out float shadow_receive;  // 1 - E, as tfrag3
out float emissive;
uniform int em_on;
uniform float em_w[10];
float em_value(uint w) {
  return float((w >> 16u) & 0xffu) / 255.0 * em_w[min(int((w >> 24u) & 0xfu), 9)];
}

// etie stuff
uniform vec4 persp0;
uniform vec4 persp1;
uniform mat4 cam_no_persp;
uniform vec4 cam_trans;

// unified vista fog: x on, y start (m), z density (1/m), w 1/scale height (1/m)
uniform vec4 atmo;
// neighbour-mode haze: x density (1/m), y start (m), z max amount
uniform vec4 haze;
out float haze_f;
out float fog_tw;  // share of the fog layer that takes the neighbour tint

float atmo_fog(float dist_m, float mid_y_m) {
  float od = atmo.z * max(dist_m - atmo.y, 0.) * exp(-atmo.w * max(mid_y_m, 0.));
  return (255. - fog_min) * (1. - exp(-od));
}

// atmo.x 2: fog is max(game, unified + haze); only the excess takes the tint and haze colour
float atmo_combine(float van, float af, inout float hz, out float tw) {
  float v = van / 255., te = 1. - (1. - af / 255.) * (1. - hz), a1 = max(v, af / 255.);
  tw = te > v ? (te - v) / te : 0.;
  hz = te > v ? max(1. - (1. - te) / max(1. - a1, 1e-4), 0.) : 0.;
  return a1 * 255.;
}

void main() {
  insp_vid = uint(gl_VertexID);
  float fog1 = camera[3].w + camera[0].w * position_in.x + camera[1].w * position_in.y + camera[2].w * position_in.z;
  fogginess = 255 - clamp(fog1 + hvdf_offset.w, fog_min, fog_max);
  haze_f = 0.;
  fog_tw = 1.;
  if (atmo.x > 0.) {
    vec3 d = (position_in - cam_trans.xyz) * (1. / 4096.);
    float af = atmo_fog(length(d), 0.5 * (position_in.y + cam_trans.y) * (1. / 4096.));
    haze_f = haze.z * (1. - exp(-haze.x * max(length(d) - haze.y, 0.)));
    fogginess = atmo.x > 1.5 ? atmo_combine(fogginess, af, haze_f, fog_tw) : af;
  }
  vec4 vf17 = cam_no_persp[3];
  vf17 += cam_no_persp[0] * position_in.x;
  vf17 += cam_no_persp[1] * position_in.y;
  vf17 += cam_no_persp[2] * position_in.z;
  vec4 p_proj = vec4(persp1.x * vf17.x, persp1.y * vf17.y, persp1.z, persp1.w);
  p_proj += persp0 * vf17.z;

  float pQ = 1.f / p_proj.w;
  vec4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  ssao_cpos = gl_Position.xyw;
  ssao_vnrm = vec4(0.0);



  if (decal == 1) {
    fragment_color = vec4(1.0, 1.0, 1.0, 1.0);
  } else {
    // time of day lookup
    fragment_color = texelFetch(tex_T10, time_of_day_index, 0);
    // color adjustment
    fragment_color *= 2;
    fragment_color.a *= 2;
  }
  pbr_cau = decal == 1 ? vec3(0.0) : 2.0 * pbr_caustic_env(time_of_day_index);

  tex_coord = tex_coord_in;
  pbr_vertex(position_in, cam_trans.xyz,
             (pbr_on != 0 && pbr_rec != 0) ? texelFetch(tex_T11, gl_VertexID) : uvec4(0u));
  shadow_pos = position_in;
  shadow_share = (ws_on & 1) != 0
                     ? ws_share_est(texelFetch(tex_T10, time_of_day_index, 0).rgb * 2.0)
                     : shadow_share_global;
  shadow_share1 = shadow_share;
  emissive = em_on == 1 ? em_value(texelFetch(tex_T11, gl_VertexID).w) : 0.0;
  shadow_receive = 1.0 - emissive;
}
