#version 410 core

// Ocean shading with tessellation (ShaderId::OCEAN_WATER): the vertices pass through to
// ocean_water.tesc, which splits each triangle by its size on screen, and ocean_water.tese
// displaces the new vertices. The fragment shader is ocean_common.frag.
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec3 tex_coord_in;
layout (location = 3) in uvec4 fog_in;  // fog, wave weight, depth share
layout (location = 4) in vec3 env_stq_in;  // the envmap pass's vertex, when merged (perf bit 14)
layout (location = 5) in vec4 env_rgba_in;

out vec3 tc_pos;
out vec4 tc_rgba;
out vec3 tc_stq;
out vec3 tc_fog_edge;
out vec3 tc_env_stq;
out vec4 tc_env_rgba;
out vec3 tc_world;  // on the ocean plane (game units), for the split levels

WATER_WAVES

void main() {
  tc_pos = position_in;
  tc_rgba = rgba_in;
  tc_stq = tex_coord_in;
  tc_fog_edge = vec3(float(fog_in.x), vec2(fog_in.yz) / 255.);
  tc_env_stq = env_stq_in;
  tc_env_rgba = env_rgba_in;
  water_depth_w = tc_fog_edge.z;
  tc_world = water_plane_point(position_in);
}
