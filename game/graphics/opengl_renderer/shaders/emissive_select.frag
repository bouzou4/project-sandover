#version 410 core

// emissive tile lights, pass 2: write the selected tile into its candidate slot
layout(location = 0) out vec4 power;
layout(location = 1) out vec4 pos;

flat in vec4 v_power;
flat in vec4 v_pos;

void main() {
  power = v_power;
  pos = v_pos;
}
