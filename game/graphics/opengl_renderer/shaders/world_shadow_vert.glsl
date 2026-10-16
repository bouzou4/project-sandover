// World sun map, vertex side of the background receivers (WorldShadows.h): the direct-sun share
// of a vertex without sidecar data, estimated from its palette colour.
#ifndef SO_WORLD_SHADOW_VERT
#define SO_WORLD_SHADOW_VERT
uniform int ws_on;
uniform float ws_amb;  // shader colour of a vertex the bake leaves at the level's median ambient
out float shadow_share1;  // the direct share this vertex would have fully lit

// colour above the ambient floor: about 0 where the bake keeps the vertex in shadow
float ws_share_est(vec3 c) {
  return clamp(1.0 - ws_amb / max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-3), 0.0, 1.0);
}
#endif
