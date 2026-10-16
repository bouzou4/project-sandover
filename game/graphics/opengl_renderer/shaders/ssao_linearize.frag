#version 410 core

// linearize: resolve the MSAA depth copy to linear view z in metres, keeping the farthest sample
out float csz;

uniform sampler2DMS depth_ms;
uniform sampler2D depth_ss;
uniform int samples;
uniform vec3 lin;  // see make_new_cam_mat
uniform float far_m;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float d = 1.0;
  if (samples > 1) {
    for (int i = 0; i < samples; i++) {
      d = min(d, texelFetch(depth_ms, p, i).r);
    }
  } else {
    d = texelFetch(depth_ss, p, 0).r;
  }
  // depth is cleared to 0 and larger is nearer, so the smallest sample is the farthest
  if (d <= 0.0) {
    csz = far_m;
    return;
  }
  float z = abs(lin.x / (lin.y * (2.0 * d - 1.0) - lin.z)) / 4096.0;
  csz = min(z, far_m);
}
