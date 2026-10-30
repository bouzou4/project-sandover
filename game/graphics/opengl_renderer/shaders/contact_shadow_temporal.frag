#version 410 core

// contact shadows (ContactShadows.h), temporal pass: the raw term blended into its reprojected
// history (ScreenHistory.h); rgba = term, view depth (m), frames accumulated, 1
out vec4 color;

uniform sampler2D cs_raw;
uniform sampler2D csz;
uniform mat3 cs_kinv;
uniform vec3 cs_cam;
uniform vec2 cs_size;
uniform float cs_far;
uniform float cs_max_n;  // frames the history may hold (the blend floor is 1 / (n + 1))

SCREEN_HISTORY_GLSL

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 hi = ivec2(cs_size) - 1;
  float z = texelFetch(csz, p, 0).r;
  float cur = texelFetch(cs_raw, p, 0).r;
  if (z >= 0.99 * cs_far) {
    color = vec4(0.0, z, 0.0, 1.0);
    return;
  }
  float mn = cur, mx = cur;
  for (int i = 0; i < 9; i++) {
    if (i == 4) {
      continue;
    }
    float v = texelFetch(cs_raw, clamp(p + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), hi), 0).r;
    mn = min(mn, v);
    mx = max(mx, v);
  }
  float w = z * 4096.0;
  vec3 P = cs_kinv * vec3(((vec2(p) + 0.5) / cs_size * 2.0 - 1.0) * w, w);
  vec2 uv;
  float d_then;
  float n = 0.0;
  float h = cur;
  if (sh_reproject(P + cs_cam, uv, d_then)) {
    vec4 H = texture(sh_prev, uv);
    // disocclusion: the history's depth there is not this surface's depth then
    if (abs(H.g - d_then) < 0.02 * d_then + 0.03) {
      n = H.b;
      float hp = clamp(H.r, mn - 0.02, mx + 0.02);
      h = mix(hp, cur, 1.0 / (min(n, cs_max_n) + 1.0));
    }
  }
  color = vec4(h, z, min(n + 1.0, cs_max_n), 1.0);
}
