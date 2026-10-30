vec3 decodeNormal(vec4 e) {
  return dot(e.rgb, e.rgb) < 0.01 ? vec3(0.0) : normalize(e.rgb * 2.0 - 1.0);
}
