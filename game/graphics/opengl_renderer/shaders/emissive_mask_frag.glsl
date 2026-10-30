float emissive(ivec2 p) {
  if (em_samples <= 1) {
    return texelFetch(em_ss, p, 0).r;
  }
  float e = 0.0;
  for (int i = 0; i < em_samples; i++) {
    e += texelFetch(em_ms, p, i).r;
  }
  return e / float(em_samples);
}
