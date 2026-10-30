float em_value(uint w) {
  return float((w >> 16u) & 0xffu) / 255.0 * em_w[min(int((w >> 24u) & 0xfu), 9)];
}
