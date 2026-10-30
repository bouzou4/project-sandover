#version 410 core

layout(vertices = 3) out;

in vec3 tc_pos[];
in vec4 tc_rgba[];
in vec3 tc_stq[];
in vec3 tc_fog_edge[];
in vec3 tc_world[];
in vec3 tc_env_stq[];
in vec4 tc_env_rgba[];

out vec3 te_pos[];
out vec4 te_rgba[];
out vec3 te_stq[];
out vec3 te_fog_edge[];
out vec3 te_env_stq[];
out vec4 te_env_rgba[];
out float te_shoal[];  // the corner's shoal weight; the evaluation stage interpolates it

uniform vec4 water_tess;  // GS pixels per segment, world segment (m), max level, no split past (m)
uniform vec2 water_dmax;  // largest horizontal and vertical wave displacement (m)

XR_GS_VERT

WATER_WAVES

// Depends only on the edge, so both triangles of an edge split alike. Near the camera, where
// the near and mid ocean meet edge to edge, every edge is cut into equal world segments: both
// grids are 24 m / 2^n, so the cuts on a shared boundary coincide and leave no crack.
float water_level(int a, int b) {
  float m = length(tc_world[a].xz - tc_world[b].xz) / 4096.;
  float d = min(length(tc_world[a].xz - water_eye.xz), length(tc_world[b].xz - water_eye.xz));
  d /= 4096.;
  if (d > water_tess.w) {
    return 1.;
  }
  if (d < 250.) {
    return clamp(round(m / water_tess.y), 1., 64.);
  }
  float px = length((tc_pos[a].xy - tc_pos[b].xy) * 4096.);
  return clamp(ceil(px / water_tess.x), 1., water_tess.z);
}

// NaN from a control point without a world point would cull the patch
float water_level_ok(int a, int b) {
  float l = water_level(a, b);
  return l >= 1. && l <= 64. ? l : 1.;
}

// Clip outcodes of the corners' rest points moved to every corner of the displacement box: a
// patch whose points all lie past one clip plane draws nothing, displaced or not (perf bit 15)
bool water_patch_outside() {
  int all_out = 15;
  for (int i = 0; i < 3; i++) {
    water_depth_w = tc_fog_edge[i].z;
    vec3 P = water_plane_point(tc_pos[i]);
    if (water_plane_missed || !water_finite3(P)) {
      return false;
    }
    for (int k = 0; k < 8; k++) {
      vec3 o = vec3((k & 1) != 0 ? water_dmax.x : -water_dmax.x,
                    (k & 2) != 0 ? water_dmax.y : -water_dmax.y,
                    (k & 4) != 0 ? water_dmax.x : -water_dmax.x);
      vec4 c = -(water_cam * vec4(P + o * 4096., 1.));
      if (c.w <= 0.) {
        return false;
      }
      vec3 g = c.xyz * (water_hvdf.w / c.w) + water_hvdf.xyz;
      float x = g.x / 4096. + stereo_gs.x * (g.z / 16777216. - stereo_gs.y);
      vec4 h = xr_gs(vec3(x, g.y / 4096., g.z / 16777216.));
      if (!(h.w > 0.)) {
        return false;
      }
      vec2 cp = vec2((h.x - 0.5 * h.w) * 16., -(h.y - 0.5 * h.w) * 32. * SCISSOR_ADJUST * HEIGHT_SCALE);
      float w = h.w * 1.001;
      int out_code = (cp.x > w ? 1 : 0) | (cp.x < -w ? 2 : 0) | (cp.y > w ? 4 : 0) |
                     (cp.y < -w ? 8 : 0);
      all_out &= out_code;
      if (all_out == 0) {
        return false;
      }
    }
  }
  return true;
}

void main() {
  te_pos[gl_InvocationID] = tc_pos[gl_InvocationID];
  te_rgba[gl_InvocationID] = tc_rgba[gl_InvocationID];
  te_stq[gl_InvocationID] = tc_stq[gl_InvocationID];
  te_fog_edge[gl_InvocationID] = tc_fog_edge[gl_InvocationID];
  te_env_stq[gl_InvocationID] = tc_env_stq[gl_InvocationID];
  te_env_rgba[gl_InvocationID] = tc_env_rgba[gl_InvocationID];
  te_shoal[gl_InvocationID] = 1.;
  if (water_disp_on != 0) {
    water_depth_w = tc_fog_edge[gl_InvocationID].z;
    vec3 P = water_plane_point(tc_pos[gl_InvocationID]);
    bool ok = !water_plane_missed && water_finite3(P) && (-(water_cam * vec4(P, 1.))).w > 0.;
    te_shoal[gl_InvocationID] = ok ? water_shoal_of(P) : 1.;
  }
  if (gl_InvocationID == 0) {
    if ((perf_opt & (1u << 15)) != 0u && water_disp_on != 0 && water_patch_outside()) {
      gl_TessLevelOuter[0] = 0.;
      gl_TessLevelOuter[1] = 0.;
      gl_TessLevelOuter[2] = 0.;
      gl_TessLevelInner[0] = 0.;
      return;
    }
    float l0 = water_level_ok(1, 2);
    float l1 = water_level_ok(2, 0);
    float l2 = water_level_ok(0, 1);
    gl_TessLevelOuter[0] = l0;
    gl_TessLevelOuter[1] = l1;
    gl_TessLevelOuter[2] = l2;
    gl_TessLevelInner[0] = max(l0, max(l1, l2));
  }
}
