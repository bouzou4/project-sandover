#version 410 core

out vec4 color;

uniform sampler2D depth_tex;
uniform sampler2DMS depth_tex_ms;
uniform int use_ms;
// x: depth offset, y: perspective w scale, z: depth scale (see make_new_cam_mat)
uniform vec3 lin;
uniform float far_dist;
uniform int raw;

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  float d = use_ms == 1 ? texelFetch(depth_tex_ms, px, 0).r : texelFetch(depth_tex, px, 0).r;
  if (d <= 0.0) {
    // cleared depth: nothing opaque drawn here
    color = vec4(0.15, 0.1, 0.3, 1.0);
    return;
  }
  if (raw == 1) {
    color = vec4(vec3(d), 1.0);
    return;
  }
  float z = abs(lin.x / (lin.y * (2.0 * d - 1.0) - lin.z));
  float g = sqrt(clamp(z / far_dist, 0.0, 1.0));
  color = vec4(vec3(1.0 - g), 1.0);
}
