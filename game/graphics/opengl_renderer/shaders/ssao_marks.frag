#version 410 core

// late stencil marks (StencilMarks.h): passes the depth test EQUAL where the source's depth copy
// is still the depth; with a copy from before the source, only where the source changed it
uniform int ms;
uniform int has_before;
uniform sampler2D after_ss;
uniform sampler2D before_ss;
uniform sampler2DMS after_ms;
uniform sampler2DMS before_ms;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float a, b = -1.0;
  if (ms != 0) {
    a = texelFetch(after_ms, p, gl_SampleID).r;
    if (has_before != 0) {
      b = texelFetch(before_ms, p, gl_SampleID).r;
    }
  } else {
    a = texelFetch(after_ss, p, 0).r;
    if (has_before != 0) {
      b = texelFetch(before_ss, p, 0).r;
    }
  }
  if (a == b) {
    discard;
  }
  gl_FragDepth = a;
}
