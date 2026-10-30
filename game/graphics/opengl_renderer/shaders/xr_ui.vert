#version 410 core

// XR 2D layer (OpenGLRenderer::ui_layer_finish): one quad from gl_VertexID over rect (NDC x0 y0 x1 y1)
uniform vec4 rect;
out vec2 uv;

void main() {
  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
  uv = c;
  gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);
}
