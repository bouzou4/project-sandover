#version 410 core

layout (location = 0) in vec3 position_in;

uniform vec2 stereo_gs;

XR_GS_VERT

void main() {
  // Note: position.y is multiplied by 32 instead of 16 to undo the half-height for interlacing stuff.
  float x = position_in.x + stereo_gs.x * (position_in.z - stereo_gs.y);
  vec4 h = xr_gs(vec3(x, position_in.y, position_in.z));
  gl_Position = vec4((h.x - 0.5 * h.w) * 16., -(h.y - 0.5 * h.w) * 32, h.z * 2 - h.w, h.w);
  // scissoring area adjust
  gl_Position.y *= SCISSOR_ADJUST;
}
