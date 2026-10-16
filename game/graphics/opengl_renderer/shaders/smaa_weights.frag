#version 410 core
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
SANDOVER_SMAA_CONFIG

uniform sampler2D edges_tex;
uniform sampler2D area_tex;
uniform sampler2D search_tex;

in vec2 v_uv;
in vec2 v_pix;
in vec4 v_offset[3];
out vec4 out_weights;

void main() {
  out_weights = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_offset, edges_tex, area_tex,
                                                search_tex, vec4(0.0));
}
