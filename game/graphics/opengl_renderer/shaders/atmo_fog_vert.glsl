float atmo_fog(float dist_m, float mid_y_m) {
  float od = atmo.z * max(dist_m - atmo.y, 0.) * exp(-atmo.w * max(mid_y_m, 0.));
  return (255. - fog_min) * (1. - exp(-od));
}
