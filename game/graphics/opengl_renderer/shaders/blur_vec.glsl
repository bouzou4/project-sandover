vec2 blur_vec(vec4 r) {
  vec2 v = r.w > 0.5 ? vec2(0.0) : r.xy * vscale;
  float l = length(v);
  return l > rmax ? v * (rmax / l) : v;
}
