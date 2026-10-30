#version 410 core
SSAO_NORMAL_VERT
INSPECT_VERT

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in int time_of_day_index;

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;

ATMO_UNIFORMS_VERT
out float fog_tw;  // share of the fog layer that takes the neighbour tint

ATMO_FOG_VERT
ATMO_COMBINE_VERT
// moving sun (Jak 1): per-vertex fit from the .unbake sidecar, see background/Unbake.h
uniform usamplerBuffer tex_T11;
uniform int ub_mask;      // palette slots to evaluate
uniform vec3 ub_w[8];     // palette blend weight per slot and channel
uniform vec3 ub_dir[8];   // fitted sun per slot
uniform vec3 ub_lgt[8];
uniform vec3 ub_amb[8];
uniform vec3 ub_tdir[8];  // target sun per slot
uniform vec3 ub_tlgt[8];
uniform vec3 ub_tamb[8];
uniform ivec2 ub_tv[8];   // visibility slots the target interpolates between
uniform float ub_tvf[8];
uniform float ub_sw[8];   // 1: the slot's direct light is the one the shadow maps cast
uniform float ub_scale;   // strength / 127.5: u8 steps to shader colour
uniform int ub_view;

PBR_BG_VERT

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

// emissive mask E from the .unbake sidecar (record.w bits 16-27), weighted per source
uniform int em_on;
uniform float em_w[10];
out float emissive;

EM_VALUE_VERT
// actor shadows: receiver position and sun share (global estimate without sidecar data)
uniform float shadow_share_global;
WORLD_SHADOW_VERT
out vec3 shadow_pos;
out float shadow_share;
out float shadow_receive;  // 1 - emissive: lit surfaces never get an actor shadow

vec3 oct_decode(vec2 e) {
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) {
    n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  }
  return normalize(n);
}

float vis(uint bits, int slot) {
  return float((bits >> uint(4 * slot)) & 0xfu) / 15.0;
}

void main() {
  insp_vid = uint(gl_VertexID);
  // old system:
  // - load vf12
  // - itof0 vf12
  // - multiply with camera matrix (add trans)
  // - let Q = fogx / vf12.w
  // - xyz *= Q
  // - xyzw += hvdf_offset
  // - clip w.
  // - ftoi4 vf12
  // use in gs.
  // gs is 12.4 fixed point, set up with 2048.0 as the center.

  // the itof0 is done in the preprocessing step.  now we have floats.


  // Step 3, the camera transform
  vec3 vert = position_in - cam_trans.xyz;
  vec4 transformed = -pc_camera[3];
  transformed.w = 0;
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;

  // do fog!
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  haze_f = 0.;
  fog_tw = 1.;
  if (atmo.x > 0.) {
    float af = atmo_fog(length(vert) * (1. / 4096.), 0.5 * (position_in.y + cam_trans.y) * (1. / 4096.));
    haze_f = haze.z * (1. - exp(-haze.x * max(length(vert) * (1. / 4096.) - haze.y, 0.)));
    fogginess = atmo.x > 1.5 ? atmo_combine(fogginess, af, haze_f, fog_tw) : af;
  }

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  ssao_cpos = gl_Position.xyw;
  ssao_vnrm = vec4(0.0);
  if (ssao_nrm == 2) {
    uvec4 ssao_rec = texelFetch(tex_T11, gl_VertexID);
    if ((ssao_rec.w & 0x100u) != 0u) {
      float s = SCISSOR_ADJUST * HEIGHT_SCALE;
      ssao_vnrm = vec4(ssao_clip_normal(vec3(pc_camera[0].x, pc_camera[0].y * s, pc_camera[0].w),
                                        vec3(pc_camera[1].x, pc_camera[1].y * s, pc_camera[1].w),
                                        vec3(pc_camera[2].x, pc_camera[2].y * s, pc_camera[2].w),
                                        ssao_oct_decode(ssao_rec.y)),
                       1.0);
    }
  }

  // time of day lookup
  fragment_color = texelFetch(tex_T10, time_of_day_index, 0);
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;

  uvec4 rec = texelFetch(tex_T11, gl_VertexID);
  float q = float(rec.w & 0xffu) / 255.0;
  float a = float(rec.x & 0xffffu) / 16384.0;
  float k = float(rec.x >> 16u) / 16384.0;
  vec3 n = oct_decode(vec2(float(rec.y & 0xffffu), float(rec.y >> 16u)) / 32767.5 - 1.0);
  vec3 delta = vec3(0.0);
  vec3 model = vec3(0.0);
  vec3 sun_part = vec3(0.0);
  vec3 lit_part = vec3(0.0);  // sun_part with every slot's visibility at 1
  vec3 shown = vec3(0.0);
  for (int s = 0; s < 8; s++) {
    if ((ub_mask & (1 << s)) == 0) {
      continue;
    }
    vec3 m = a * ub_amb[s] + (vis(rec.z, s) * k * max(0.0, dot(n, ub_dir[s]))) * ub_lgt[s];
    float vt = mix(vis(rec.z, ub_tv[s].x), vis(rec.z, ub_tv[s].y), ub_tvf[s]);
    vec3 t = a * ub_tamb[s] + (vt * k * max(0.0, dot(n, ub_tdir[s]))) * ub_tlgt[s];
    delta += ub_w[s] * (t - m);
    model += ub_w[s] * m;
    sun_part += (ub_w[s] * ub_sw[s]) * ((vt * k * max(0.0, dot(n, ub_tdir[s]))) * ub_tlgt[s]);
    lit_part += (ub_w[s] * ub_sw[s]) * ((k * max(0.0, dot(n, ub_tdir[s]))) * ub_tlgt[s]);
    shown += ub_w[s] * t;
  }
  if (ub_view == 0) {
    fragment_color.rgb += (q * ub_scale) * delta;
  } else if (ub_view == 1) {
    fragment_color.rgb = model / 127.5;
  } else if (ub_view == 2) {
    fragment_color.rgb = vec3(q);
  } else if (ub_view == 3) {
    fragment_color.rgb = n * 0.5 + 0.5;
  } else {
    fragment_color.rgb = vec3(0.5) + (4.0 * q * ub_scale) * delta;
  }

  vec3 ws_col = fragment_color.rgb;
  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }
  pbr_cau = decal == 1 || ub_view != 0 ? vec3(0.0) : 2.0 * pbr_caustic_env(time_of_day_index);
  
  tex_coord = tex_coord_in;
  emissive = em_on == 1 ? em_value(rec.w) : 0.0;
  shadow_pos = position_in;
  vec3 luma = vec3(0.299, 0.587, 0.114);
  float fitted_share = clamp(dot(sun_part, luma) / max(dot(shown, luma), 1e-3), 0.0, 1.0);
  float fitted_lit = clamp(dot(lit_part, luma) / max(dot(shown, luma), 1e-3), 0.0, 4.0);
  float glow = ws_glow_share(time_of_day_index, ws_col);  // 0 with the map off
  float est = (ws_on & 1) != 0 ? ws_share_est(ws_col, glow) : shadow_share_global;
  shadow_share = mix(est, fitted_share * (1.0 - glow), q);
  shadow_share1 = mix(est, fitted_lit * (1.0 - glow), q);
  shadow_receive = 1.0 - emissive;
  pbr_vertex(position_in, cam_trans.xyz,
             (pbr_on != 0 && pbr_rec != 0) ? texelFetch(tex_T11, gl_VertexID) : uvec4(0u));
}
