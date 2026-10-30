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
uniform usamplerBuffer tex_T11;  // .unbake records, see background/Unbake.h

ATMO_UNIFORMS_VERT
out float fog_tw;  // share of the fog layer that takes the neighbour tint

ATMO_FOG_VERT
ATMO_COMBINE_VERT
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
    vec3 d = (position_in - cam_trans.xyz) * (1. / 4096.);
    float af = atmo_fog(length(d), 0.5 * (position_in.y + cam_trans.y) * (1. / 4096.));
    haze_f = haze.z * (1. - exp(-haze.x * max(length(d) - haze.y, 0.)));
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
  vec3 ws_col = fragment_color.rgb;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }
  pbr_cau = decal == 1 ? vec3(0.0) : 2.0 * pbr_caustic_env(time_of_day_index);
  
  tex_coord = tex_coord_in;
  emissive = em_on == 1 ? em_value(texelFetch(tex_T11, gl_VertexID).w) : 0.0;
  shadow_pos = position_in;
  shadow_share = (ws_on & 1) != 0 ? ws_share_est(ws_col, ws_glow_share(time_of_day_index, ws_col))
                                 : shadow_share_global;
  shadow_share1 = shadow_share;
  shadow_receive = 1.0 - emissive;
  pbr_vertex(position_in, cam_trans.xyz,
             (pbr_on != 0 && pbr_rec != 0) ? texelFetch(tex_T11, gl_VertexID) : uvec4(0u));
}
