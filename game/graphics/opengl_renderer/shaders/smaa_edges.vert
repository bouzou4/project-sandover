#version 410 core
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
SANDOVER_SMAA_CONFIG

out vec2 v_uv;
out vec4 v_offset[3];

void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  v_uv = p;
  vec4 o[3];
  SMAAEdgeDetectionVS(p, o);
  v_offset = o;
}
