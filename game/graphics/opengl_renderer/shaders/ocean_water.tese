#version 410 core

// GS attributes are linear in screen space, so barycentric interpolation of the control
// points matches what the GS would draw; then the same displacement as ocean_common.vert.
layout(triangles, equal_spacing, ccw) in;

in vec3 te_pos[];
in vec4 te_rgba[];
in vec3 te_stq[];
in vec3 te_fog_edge[];
in vec3 te_env_stq[];
in vec4 te_env_rgba[];

invariant gl_Position;  // the mid ocean's envmap pass depth-tests against its textured pass
centroid out vec4 fragment_color;
centroid out vec3 tex_coord;
centroid out float fog;
centroid out vec2 water_gs;
centroid out vec3 water_rest;
centroid out float water_edge;
centroid out float water_band;
noperspective centroid out float water_wz;
centroid out vec3 env_tex_coord;  // merged envmap pass: the bucket 4 vertex's stq and colour
centroid out vec4 env_color;

uniform int bucket;

XR_GS_VERT

WATER_WAVES

void main() {
  vec3 b = gl_TessCoord;
  vec3 position_in = b.x * te_pos[0] + b.y * te_pos[1] + b.z * te_pos[2];
  vec4 rgba_in = b.x * te_rgba[0] + b.y * te_rgba[1] + b.z * te_rgba[2];
  vec3 fe = b.x * te_fog_edge[0] + b.y * te_fog_edge[1] + b.z * te_fog_edge[2];
  water_depth_w = fe.z;
  float edge = fe.y;
  water_band = fe.y;
  vec3 pos = position_in;
  water_rest = vec3(0.);
  if (water_disp_on != 0) {
    pos = water_displace(position_in, edge, water_rest);
  }
  water_edge = edge;
  float x = pos.x + stereo_gs.x * (pos.z - stereo_gs.y);
  vec4 h = xr_gs(vec3(x, pos.y, pos.z));
  gl_Position = vec4((h.x - 0.5 * h.w) * 16., -(h.y - 0.5 * h.w) * 32, h.z * 2 - h.w, h.w);
  gl_Position.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  water_wz = h.z / h.w;
  fragment_color = vec4(rgba_in.rgb, rgba_in.a * 2);
  tex_coord = b.x * te_stq[0] + b.y * te_stq[1] + b.z * te_stq[2];
  water_gs = pos.xy * 4096.;
  fog = 255. - fe.x;
  env_tex_coord = b.x * te_env_stq[0] + b.y * te_env_stq[1] + b.z * te_env_stq[2];
  env_color = b.x * te_env_rgba[0] + b.y * te_env_rgba[1] + b.z * te_env_rgba[2];

  if (bucket == 0) {
    fragment_color.rgb *= 2;
  } else if (bucket == 1 || bucket == 3) {
    fragment_color *= 2;
  } else if (bucket == 4) {
    fragment_color.a = 0;
  }
}
