#version 410 core

// actor shadow pre-pass: generic vertices are already in camera space (see ActorShadows.h)
layout (location = 0) in vec3 position_in;

uniform mat4 shadow_matrix;  // camera space to the light's clip space

void main() {
  gl_Position = shadow_matrix * vec4(position_in, 1.0);
}
