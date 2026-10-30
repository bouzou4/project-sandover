#version 410 core

// VistaHiZ: farthest (smallest, GEQUAL) depth of each tile of the render depth
out vec4 color;

uniform sampler2DMS depth_ms;
uniform sampler2D depth_ss;
uniform int samples;  // 0: single-sampled
uniform ivec2 size;
uniform int tile;

void main() {
  ivec2 base = ivec2(gl_FragCoord.xy) * tile;
  ivec2 hi = min(base + ivec2(tile), size) - ivec2(1);
  float d = 1.0;
  for (int y = base.y; y <= hi.y; y++) {
    for (int x = base.x; x <= hi.x; x++) {
      ivec2 q = ivec2(x, y);
      if (samples == 0) {
        d = min(d, texelFetch(depth_ss, q, 0).r);
      } else {
        for (int s = 0; s < samples; s++) {
          d = min(d, texelFetch(depth_ms, q, s).r);
        }
      }
    }
  }
  color = vec4(d, 0.0, 0.0, 1.0);
}
