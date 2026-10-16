#version 410 core

// sun disc quad (SunDisc.cpp) at the far plane: window depth 0, the clear value
uniform vec4 quad;  // centre and half size, NDC

void main() {
  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
  gl_Position = vec4(quad.xy + c * quad.zw, -1.0, 1.0);
}
