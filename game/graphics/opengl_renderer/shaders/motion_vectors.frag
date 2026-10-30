#version 410 core

// Velocity pass (MotionVectors.h): per pixel the nearest sample's object velocity where the merc
// surface is still the visible one, else camera-only velocity from depth.
layout(location = 0) out vec2 vel_out;
layout(location = 1) out vec4 aux_out;
// motion blur input (px): objects move against the world, capped at their on-screen motion, the
// rest 0; at rel_on 2 objects their on-screen motion, the rest the camera's. View distance (m), HUD
layout(location = 2) out vec4 rel_out;

uniform sampler2D depth_ss;
uniform sampler2D obj_ss;
uniform sampler2DMS depth_ms;
uniform sampler2DMS obj_ms;
uniform int samples;   // 1: the _ss samplers
uniform mat4 reproj;   // current (ndc, 1) -> previous clip
uniform mat3 sky;      // current (ndc.xy, 1) -> previous (x, y, w), directions
uniform vec2 size;     // render target pixels
uniform vec2 wk;       // clip w = wk.x / (ndc z - wk.y)
uniform int valid;
uniform float w_tol;   // object kept while |w_obj - w_depth| <= w_tol * w_depth
uniform int aux_on;
uniform int rel_on;  // 1 objects, 2 objects and camera

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  // GEQUAL depth, cleared to 0: the nearest sample has the largest value
  float d = -1.0;
  int si = 0;
  if (samples > 1) {
    for (int i = 0; i < samples; i++) {
      float s = texelFetch(depth_ms, px, i).r;
      if (s > d) {
        d = s;
        si = i;
      }
    }
  } else {
    d = texelFetch(depth_ss, px, 0).r;
  }
  vec2 ndc = gl_FragCoord.xy / size * 2.0 - 1.0;
  vec2 cam;
  float src;
  float w = 0.0;
  if (d <= 0.0) {
    vec3 h = sky * vec3(ndc, 1.0);
    cam = (h.xy / h.z - ndc) * 0.5 * size;
    src = 2.0;
  } else {
    float nz = d * 2.0 - 1.0;
    vec4 h = reproj * vec4(ndc, nz, 1.0);
    cam = (h.xy / h.w - ndc) * 0.5 * size;
    w = wk.x / (nz - wk.y);
    src = 0.0;
  }
  vec2 v = cam;
  vec4 o = samples > 1 ? texelFetch(obj_ms, px, si) : texelFetch(obj_ss, px, 0);
  // merc HUD models write w negative
  float ow = abs(o.z);
  if (ow > 0.0 && d > 0.0 && abs(ow - w / 4096.0) <= w_tol * w / 4096.0) {
    v = o.xy;
    src = 1.0;
  }
  if (valid == 0) {
    v = vec2(0.0);
    cam = vec2(0.0);
    src = 3.0;
  }
  vel_out = v;
  if (rel_on > 0) {
    vec2 r = rel_on == 2 ? v : vec2(0.0);
    if (src == 1.0 && rel_on == 1) {
      vec2 rel = v - cam;
      r = rel * min(1.0, length(v) / max(length(rel), 1e-4));
    }
    rel_out = o.z < 0.0 ? vec4(0.0, 0.0, 0.0, 1.0)
                        : vec4(r, d > 0.0 ? w / 4096.0 : 60000.0, 0.0);
  }
  aux_out = aux_on == 1 ? vec4(cam, w / 4096.0, src) : vec4(0.0);
}
