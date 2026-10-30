/// Share of the game's water alpha this pixel's view ray earns through the water (Beer-Lambert).
float iw_column() {
  if (iw_depth_ok == 0) {
    return 1.0;
  }
  ivec2 p = ivec2(gl_FragCoord.xy);
  float bed = texelFetch(iw_depth, min(p, textureSize(iw_depth, 0) - ivec2(1)), 0).r;
  float surf = abs(iw_lin.x / (iw_lin.y * (2.0 * gl_FragCoord.z - 1.0) - iw_lin.z)) / 4096.0;
  if (bed > 5000.0) {
    return 1.0;
  }
  vec2 xy = (vec2(p) + 0.5) * iw_proj.xy + iw_proj.zw;
  float ray = max(bed - surf, 0.0) * length(vec3(xy, 1.0));
  return 1.0 - exp(-ray / iw_water.x);
}
