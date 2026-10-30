#version 410 core

// Temporal accumulation of a per-frame-noisy scalar (TemporalAccum.h): the previous result,
// reprojected with the frame camera or the merc object velocity, blended with this frame's value.
// out: r the accumulated value (negative: excluded, passed through), g the texel's clip w (m).
out vec2 result;

uniform sampler2D signal;   // this frame, accumulation size; negative = excluded
uniform sampler2D history;  // previous result (r value, g clip w), same size
uniform int have_history;
uniform sampler2D depth_ss;
uniform sampler2DMS depth_ms;
uniform int samples;        // 1: the _ss samplers
uniform sampler2D obj_ss;
uniform sampler2DMS obj_ms;
uniform int obj_on;
uniform sampler2D dyn_ss;
uniform sampler2DMS dyn_ms;
uniform int dyn_on;         // mask: 0.5 = dynamic (characters), 1 = excluded
uniform mat4 reproj;        // current (ndc, 1) -> previous clip, both without jitter
uniform vec4 jitter;        // NDC offset of the projection: xy this frame, zw the history's
uniform vec2 wk;            // clip w = wk.x / (ndc z - wk.y)
uniform vec2 full_size;     // render target pixels
uniform int half_res;       // 1: texel = the full-res pixel the SSAO mip 1 kept
uniform float blend;        // history weight on static texels
uniform float blend_dyn;    // history weight on dynamic texels
uniform float w_tol;        // history accepted while its w is within this share of the expected
uniform int nb_clamp;       // 1: history clamped to this frame's 3x3 signal range
uniform float nb_gamma;     // > 0: that range narrowed to mean +- nb_gamma std devs

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 c = half_res == 1 ? p * 2 + ivec2(p.y & 1, p.x & 1) : p;
  c = clamp(c, ivec2(0), ivec2(full_size) - ivec2(1));
  float cur = texelFetch(signal, p, 0).r;
  if (isnan(cur)) {
    cur = 1.0;  // no AO; a NaN in the history would spread until a reset
  }
  // nearest sample, as the velocity pass (GEQUAL depth, cleared to 0)
  float d = -1.0;
  int si = 0;
  if (samples > 1) {
    for (int i = 0; i < samples; i++) {
      float s = texelFetch(depth_ms, c, i).r;
      if (s > d) {
        d = s;
        si = i;
      }
    }
  } else {
    d = texelFetch(depth_ss, c, 0).r;
  }
  if (d <= 0.0) {
    result = vec2(cur, 0.0);
    return;
  }
  float nz = d * 2.0 - 1.0;
  float w = wk.x / (nz - wk.y) / 4096.0;  // metres: RG16F saturates near 16 m in game units
  if (cur < 0.0 || have_history == 0) {
    result = vec2(cur, w);
    return;
  }
  vec2 ndc = (vec2(c) + 0.5) / full_size * 2.0 - 1.0;
  vec4 h = reproj * vec4(ndc - jitter.xy, nz, 1.0);
  vec2 prev = (h.xy / h.w + jitter.zw) * 0.5 * full_size + 0.5 * full_size;
  float expect_w = h.w * w;  // h = previous clip / this frame's clip w
  float a = blend;
  float tol = w_tol;
  if (obj_on == 1) {
    vec4 o = samples > 1 ? texelFetch(obj_ms, c, si) : texelFetch(obj_ss, c, 0);
    if (o.z > 0.0 && abs(o.z - w) <= 0.01 * w) {
      prev = vec2(c) + 0.5 + o.xy + (jitter.zw - jitter.xy) * 0.5 * full_size;
      expect_w = w;
      tol = 2.0 * w_tol;
      a = blend_dyn;
    }
  }
  if (dyn_on == 1 && a == blend) {
    float m = samples > 1 ? texelFetch(dyn_ms, c, si).r : texelFetch(dyn_ss, c, 0).r;
    if (m > 0.25 && m < 0.75) {
      a = blend_dyn;  // a character without object velocity: camera-only, so trust less
    }
  }
  // history texels whose surface matches (no blend across a depth edge): bilinear at full res;
  // at half res a tent around each texel's kept pixel (radius covers the widest gap, 1.58 px)
  ivec2 hsize = textureSize(history, 0);
  float sum = 0.0, wsum = 0.0, wall = 0.0;
  ivec2 b;
  vec2 f = vec2(0.0);
  int n;
  if (half_res == 1) {
    b = ivec2(floor((prev - 3.1) * 0.5)) + 1;
    n = 3;
  } else {
    vec2 hs = prev - 0.5;
    b = ivec2(floor(hs));
    f = hs - vec2(b);
    n = 2;
  }
  for (int i = 0; i < n * n; i++) {
    ivec2 o = ivec2(i % n, i / n);
    ivec2 t = b + o;
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, hsize))) {
      continue;
    }
    float bw;
    if (half_res == 1) {
      vec2 kc = vec2(t * 2 + ivec2(t.y & 1, t.x & 1)) + 0.5;
      bw = max(1.0 - length(prev - kc) / 1.6, 0.0);
      bw *= bw;
    } else {
      bw = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
    }
    if (bw <= 0.0) {
      continue;
    }
    wall += bw;
    vec2 hv = texelFetch(history, t, 0).rg;
    if (isnan(hv.r) || hv.r < 0.0 || hv.g <= 0.0 || abs(hv.g - expect_w) > tol * expect_w) {
      continue;
    }
    sum += hv.r * bw;
    wsum += bw;
  }
  // clip to this frame's AO around the texel: occlusion a moved occluder cast does not linger
  float lo = -1e9, hi = 1e9;
  if (nb_clamp == 1) {
    lo = cur;
    hi = cur;
    float m1 = 0.0, m2 = 0.0, cnt = 0.0;
    ivec2 smax = textureSize(signal, 0) - ivec2(1);
    for (int i = 0; i < 9; i++) {
      float s = texelFetch(signal, clamp(p + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), smax), 0).r;
      if (s >= 0.0) {
        lo = min(lo, s);
        hi = max(hi, s);
        m1 += s;
        m2 += s * s;
        cnt += 1.0;
      }
    }
    if (nb_gamma > 0.0 && cnt > 0.0) {
      float mu = m1 / cnt;
      float sd = sqrt(max(m2 / cnt - mu * mu, 0.0));
      lo = max(lo, mu - nb_gamma * sd);
      hi = min(hi, mu + nb_gamma * sd);
      // never past this frame's own value: thin dark texels keep their occlusion
      lo = min(lo, cur);
      hi = max(hi, cur);
    }
  }
  float v = wsum > 0.5 * wall ? mix(cur, clamp(sum / wsum, lo, hi), a) : cur;
  result = vec2(v, w);
}
