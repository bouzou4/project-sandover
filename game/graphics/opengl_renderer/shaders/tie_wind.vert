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
uniform vec4 cam_trans;
uniform mat4 canopy_inst;  // this instance's world matrix after the wind (canopy shade)
out vec3 canopy_wpos;
WORLD_SHADOW_VERT
uniform float atmo_wscale;  // metres of view depth per unit of w

ATMO_UNIFORMS_VERT
out float fog_tw;  // share of the fog layer that takes the neighbour tint

ATMO_FOG_VERT
ATMO_COMBINE_VERT
PBR_BG_VERT
uniform mat4 pbr_inst;  // this instance's world matrix after the wind (game units)

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

void main() {
  insp_vid = uint(gl_VertexID);
  vec4 transformed = -camera[3];
  transformed -= camera[0] * position_in.x;
  transformed -= camera[1] * position_in.y;
  transformed -= camera[2] * position_in.z;
  float Q = fog_constant / transformed.w;

  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  haze_f = 0.;
  fog_tw = 1.;
  if (atmo.x > 0.) {
    // instanced: no world position here, so use view depth and the camera's height
    float af = atmo_fog(abs(transformed.w) * atmo_wscale, cam_trans.y * (1. / 4096.));
    haze_f = haze.z * (1. - exp(-haze.x * max(abs(transformed.w) * atmo_wscale - haze.y, 0.)));
    fogginess = atmo.x > 1.5 ? atmo_combine(fogginess, af, haze_f, fog_tw) : af;
  }

  // perspective divide!
  transformed.xyz *= Q;
  // offset
  transformed.xyz += hvdf_offset.xyz;
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

  // time of day lookup
  fragment_color = texelFetch(tex_T10, time_of_day_index, 0);
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;
  shadow_share1 = (ws_on & 1) != 0
                      ? ws_share_est(fragment_color.rgb,
                                     ws_glow_share(time_of_day_index, fragment_color.rgb))
                      : 0.0;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  tex_coord = tex_coord_in;
  pbr_vertex(pbr_inst[0].xyz * position_in.x + pbr_inst[1].xyz * position_in.y +
                 pbr_inst[2].xyz * position_in.z + pbr_inst[3].xyz,
             cam_trans.xyz, uvec4(0u));
  canopy_wpos = (canopy_inst * vec4(position_in, 1.0)).xyz;
}