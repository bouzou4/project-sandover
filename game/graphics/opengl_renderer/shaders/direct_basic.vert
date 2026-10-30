#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 5) in vec4 gs_scissor_in;

out vec4 fragment_color;
out vec4 gs_scissor;

XR_GS_VERT

void main() {
  // Note: position.y is multiplied by 32 instead of 16 to undo the half-height for interlacing stuff.
  vec4 h = xr_gs(position_in);
  gl_Position = vec4((h.x - 0.5 * h.w) * 16., -(h.y - 0.5 * h.w) * 32 * HEIGHT_SCALE, h.z * 2 - h.w, h.w);
  // scissoring area adjust
  gl_Position.y *= SCISSOR_ADJUST;
  fragment_color = vec4(rgba_in.x, rgba_in.y, rgba_in.z, rgba_in.w * 2.);
  gs_scissor = gs_scissor_in;
}
