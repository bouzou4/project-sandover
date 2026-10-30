// World sun map, vertex side of the background receivers (WorldShadows.h): the direct-sun share
// of a vertex without sidecar data, estimated from its palette colour.
#ifndef SO_WORLD_SHADOW_VERT
#define SO_WORLD_SHADOW_VERT
uniform int ws_on;
uniform float ws_amb;  // shader colour of a vertex the bake leaves at the level's median ambient
out float shadow_share1;  // the direct share this vertex would have fully lit
uniform int ws_selflit;   // 1 glow prop draw, 2 per-vertex mask (etie, Tie3)

uniform sampler1D ws_glow;  // per palette entry: baked glow in u8 / 255 (PaletteGlow.h), unit 59
uniform int ws_glow_on;

// share of colour c that is the bake's glow (light held in every slot), not sun
float ws_glow_share(int idx, vec3 c) {
  if ((ws_on & 1) == 0 || ws_glow_on == 0) {
    return 0.0;
  }
  return clamp(2.0 * texelFetch(ws_glow, idx, 0).r / max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-3),
               0.0, 1.0);
}

// colour above the ambient floor and the glow: about 0 where the bake keeps the vertex in shadow
float ws_share_est(vec3 c, float glow) {
  return clamp(1.0 - ws_amb / max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-3) - glow, 0.0, 1.0);
}
#endif
