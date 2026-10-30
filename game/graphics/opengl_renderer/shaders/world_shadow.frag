#version 410 core
// world sun map casters: alpha-tested foliage keeps its cut-out

uniform sampler2D tex_T0;
uniform float ws_alpha_ref;  // < 0: no test; alpha 1 = the 128 opaque convention

in vec2 uv;

void main() {
  if (ws_alpha_ref >= 0.0 && texture(tex_T0, uv).a * 2.0 < ws_alpha_ref) {
    discard;
  }
}
