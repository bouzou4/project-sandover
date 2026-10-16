#version 410 core

out vec4 color;

uniform sampler2D resolved;
uniform sampler2DMS ms;
uniform int samples;

// resolved value minus: sample 0, min, max, mean of the samples
void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  float r = texelFetch(resolved, px, 0).r;
  float s0 = texelFetch(ms, px, 0).r;
  float lo = s0;
  float hi = s0;
  float sum = s0;
  for (int i = 1; i < samples; i++) {
    float s = texelFetch(ms, px, i).r;
    lo = min(lo, s);
    hi = max(hi, s);
    sum += s;
  }
  color = vec4(r - s0, r - lo, r - hi, r - sum / float(samples));
}
