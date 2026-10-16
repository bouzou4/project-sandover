#version 410 core
// world sun map casters (WorldShadows.cpp): static world, depth only

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec2 tex_coord_in;
layout (location = 2) in mat4 inst_in;  // wind tie instance to world, per instance (ws_inst_on 2)

uniform mat4 ws_clip;     // world to cascade clip
uniform mat4 ws_inst;     // wind tie instance to world
uniform int ws_inst_on;   // 0 static, 1 ws_inst, 2 inst_in
uniform float ws_uv_scale;

out vec2 uv;

void main() {
  vec3 p = position_in;
  if (ws_inst_on == 1) {
    p = (ws_inst * vec4(position_in, 1.0)).xyz;
  } else if (ws_inst_on == 2) {
    p = (inst_in * vec4(position_in, 1.0)).xyz;
  }
  gl_Position = ws_clip * vec4(p, 1.0);
  uv = tex_coord_in * ws_uv_scale;
}
