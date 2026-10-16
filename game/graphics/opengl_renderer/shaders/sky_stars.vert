#version 410 core

// sky stars (SunDisc.cpp, style 1): one point per star at the far plane (window depth 0)
layout(location = 0) in vec4 star;  // direction (world, unit), luminance in paper whites

uniform mat3 star_rot;   // the sky's turn by the hour
uniform mat3 world_cs;   // world to SSAO camera space
uniform vec4 proj_info;  // pixel to SSAO camera space per unit z
uniform vec2 size;       // render pixels
uniform float point_px;
uniform float gain;
uniform vec3 haze;       // log(1 - fog at the fog end), layer height, fog end (m)

out float v_lum;

void main() {
  vec3 w = star_rot * star.xyz;
  vec3 c = world_cs * w;
  float s = max(w.y, 0.0);
  float path = haze.y / sqrt(s * s + (haze.y / haze.z) * (haze.y / haze.z));
  v_lum = star.w * gain * smoothstep(-0.052, 0.035, w.y) * exp(haze.x * path / haze.z);
  gl_PointSize = point_px;
  if (c.z <= 0.01 || v_lum <= 1e-4) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }
  vec2 px = (c.xy / c.z - proj_info.zw) / proj_info.xy;
  gl_Position = vec4(px / size * 2.0 - 1.0, -1.0, 1.0);
}
