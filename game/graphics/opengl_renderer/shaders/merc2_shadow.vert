#version 410 core

// actor shadow pre-pass: merc skinning into a light's clip space (see ActorShadows.h)
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;

uniform mat4 shadow_matrix;  // merc camera space to the light's clip space

out vec2 vtx_st;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

void main() {
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
  }
  gl_Position = shadow_matrix * vec4(vtx_pos.xyz / vtx_pos.w, 1.0);
  vtx_st = st_in;
}
