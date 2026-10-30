#version 410 core

// world map bounds (WorldShadows::build_bounds), per 8x8-texel cell of one layer, uv in the layer.
// pass 0, from the depths: g = least-squares depth slope, m = min(d - g.x), max depth.
// pass 1, over the (2k+1)^2 cells around: M = min(d - g.x) for this cell's g, max depth.
// Out: (M or m, g.x, g.y, max depth).
layout(location = 0) out vec4 bounds;
uniform sampler2DArray ws_depth;  // pass 0: the map, raw
uniform sampler2DArray ws_cells;  // pass 1: the pass-0 grid
uniform int pass;
uniform int layer;
uniform int k;          // pass 1: cells each side
uniform float size;     // map texels per side

void main() {
  ivec2 c = ivec2(gl_FragCoord.xy);
  if (pass == 0) {
    float d[64];
    float sum = 0.0, sx = 0.0, sy = 0.0, mx = 0.0;
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 4; i++) {
        ivec2 t = c * 8 + ivec2(i, j) * 2;
        vec4 g = textureGather(ws_depth, vec3((vec2(t) + 1.0) / size, float(layer)));
        // gather order: (0,1) (1,1) (1,0) (0,0)
        int b = (j * 2) * 8 + i * 2;
        d[b] = g.w;
        d[b + 1] = g.z;
        d[b + 8] = g.x;
        d[b + 9] = g.y;
      }
    }
    for (int n = 0; n < 64; n++) {
      float x = float(n % 8) - 3.5, y = float(n / 8) - 3.5;
      sum += d[n];
      sx += d[n] * x;
      sy += d[n] * y;
      mx = max(mx, d[n]);
    }
    // sum of squared offsets on an 8x8 grid: 8 * 42 texels^2; slope in depth per uv
    vec2 g = vec2(sx, sy) / 336.0 * size;
    float m = 1e30;
    for (int n = 0; n < 64; n++) {
      vec2 x = (vec2(c * 8 + ivec2(n % 8, n / 8)) + 0.5) / size;
      m = min(m, d[n] - dot(g, x));
    }
    bounds = vec4(m, g, mx);
  } else {
    ivec2 n = textureSize(ws_cells, 0).xy;
    vec2 g = texelFetch(ws_cells, ivec3(c, layer), 0).yz;
    float lo = 1e30, mx = 0.0;
    float h = 4.0 / size;
    for (int y = -k; y <= k; y++) {
      for (int x = -k; x <= k; x++) {
        ivec2 q = c + ivec2(x, y);
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, n))) {
          // taps past the edge clamp: no bound
          lo = -1e30;
          mx = 1.0;
          continue;
        }
        vec4 b = texelFetch(ws_cells, ivec3(q, layer), 0);
        vec2 dg = b.yz - g;
        vec2 cc = (vec2(q * 8) + 4.0) / size;
        // d - g.x = (d - g_q.x) + (g_q - g).x, the last term bounded over the cell
        lo = min(lo, b.x + dot(dg, cc) - (abs(dg.x) + abs(dg.y)) * h);
        mx = max(mx, b.w);
      }
    }
    bounds = vec4(lo, g, mx);
  }
}
