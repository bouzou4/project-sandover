#version 410 core

// XR 2D layer: the canvas drawn on black and on white (FP16, so additive draws do not clip on
// white) gives premultiplied colour (black) and coverage 1 - (white - black).
in vec2 uv;
out vec4 color;

uniform sampler2D on_black;
uniform sampler2D on_white;
uniform int encode;  // 1: the eyes' SDR tonemap, as the 2D had inside the eye frame

TONEMAP_SDR_GLSL

void main() {
  vec3 b = texture(on_black, uv).rgb;
  vec3 w = texture(on_white, uv).rgb;
  float a = clamp(1.0 - dot(w - b, vec3(1.0 / 3.0)), 0.0, 1.0);
  if (encode == 1) {
    // covered pixels: tonemap the colour, keep the coverage; pure additive light: tonemap as is
    b = a > 1.0 / 255.0 ? tonemap(b / a) * a : tonemap(b);
  }
  color = vec4(b, a);  // premultiplied: additive draws keep colour at zero coverage
}
