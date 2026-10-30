#version 410 core

layout(location = 0) out vec4 caster;  // sun map colour: generic casters keep their shadow

void main() {
  caster = vec4(1.0);
}
