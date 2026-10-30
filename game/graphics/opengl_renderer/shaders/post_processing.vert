#version 410 core

layout (location = 0) in vec2 position_in;

out vec2 tex_coord;

uniform int direct_flip;  // 1: the target is a D3D image, top row first

void main() {
  gl_Position = vec4(position_in.x, direct_flip == 1 ? -position_in.y : position_in.y, 0, 1.0);
  tex_coord = (position_in + vec2(1.0, 1.0)) * 0.5;
}
