#version 410 core

// separable Gaussian, sigma = radius / 2
out vec4 color;

uniform sampler2D src;
uniform vec2 step;
uniform int radius;

void main() {
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0));
  float sigma = max(float(radius) * 0.5, 0.5);
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int i = -radius; i <= radius; i++) {
    float w = exp(-0.5 * float(i * i) / (sigma * sigma));
    sum += w * texture(src, uv + float(i) * step).rgb;
    wsum += w;
  }
  color = vec4(sum / wsum, 1.0);
}
