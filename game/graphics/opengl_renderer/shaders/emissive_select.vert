#version 410 core

// emissive tile lights, pass 2: one point per tile, placed at its flux rank in the candidate row
// (read back and tracked on the CPU: LightTracker.h)
uniform sampler2D tile_flux;
uniform sampler2D tile_pos;
uniform int max_lights;
uniform float threshold;
uniform float tile_area;  // tile footprint in m^2 at 1 m of depth

flat out vec4 v_power;
flat out vec4 v_pos;

float score(ivec2 t) {
  vec4 f = texelFetch(tile_flux, t, 0);
  float l = dot(f.rgb, vec3(0.299, 0.587, 0.114));
  return l >= threshold && texelFetch(tile_pos, t, 0).w > 0.0 ? l : 0.0;
}

void main() {
  ivec2 size = textureSize(tile_flux, 0);
  int id = gl_VertexID;
  ivec2 t = ivec2(id % size.x, id / size.x);
  float mine = score(t);
  gl_PointSize = 1.0;
  v_power = vec4(0.0);
  v_pos = vec4(0.0);
  gl_Position = vec4(2.0, 2.0, 0.0, 1.0);  // clipped
  if (mine <= 0.0) {
    return;
  }
  int rank = 0;
  int n = size.x * size.y;
  for (int j = 0; j < n && rank < max_lights; j++) {
    float s = score(ivec2(j % size.x, j / size.x));
    if (s > mine || (s == mine && j < id)) {
      rank++;
    }
  }
  if (rank >= max_lights) {
    return;
  }
  vec4 p = texelFetch(tile_pos, t, 0);
  vec4 f = texelFetch(tile_flux, t, 0);
  // emitted power: mean flux over the tile x the tile's area at the light's depth
  v_power = vec4(f.rgb * tile_area * p.z * p.z, mine);
  v_pos = vec4(p.xyz, f.a);  // w: fill, for the thin/compact class
  gl_Position = vec4((float(rank) + 0.5) / float(max_lights) * 2.0 - 1.0, 0.0, 0.0, 1.0);
}
