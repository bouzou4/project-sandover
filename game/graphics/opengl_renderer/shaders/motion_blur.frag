#version 410 core

// Motion blur reconstruction (MotionBlur.h): McGuire et al. 2012 with the sample direction split
// between the neighbourhood's and the pixel's own velocity (Jimenez 2014). Writes RGB over the
// scene; pixels with no blur nearby are discarded, so they keep every sample bit for bit.
out vec4 color;

uniform sampler2D scene;  // single-sample copy of the scene, bilinear
uniform sampler2D rel;    // relative velocity px, view distance m, HUD flag
uniform sampler2D nmax;   // neighbourhood max blur vector per tile, px
uniform vec2 size;
uniform int tile;
uniform float vscale;
uniform float rmax;
uniform int taps;         // even
uniform float frame;      // dither phase
uniform float soft_z;     // depth classification width, fraction of the distance
uniform int view;         // 1 object velocity, 2 neighbourhood max tiles

BLUR_VEC_GLSL
float cone(float d, float l) {
  return clamp(1.0 - d / max(l, 1e-3), 0.0, 1.0);
}

float cylinder(float d, float l) {
  return 1.0 - smoothstep(0.95 * l, 1.05 * l + 1e-3, d);
}

// 1 when b is in front of a, 0 when it is behind by more than the soft width
float in_front(float za, float zb) {
  return clamp(1.0 - (zb - za) / (soft_z * min(za, zb)), 0.0, 1.0);
}

vec3 heat(float x) {
  return clamp(vec3(1.5 * x, 1.5 - abs(2.0 * x - 1.0) * 1.5, 1.5 * (1.0 - x)), 0.0, 1.0);
}

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  vec2 uv = gl_FragCoord.xy / size;
  vec4 rc = texelFetch(rel, px, 0);
  vec2 vn = texelFetch(nmax, px / tile, 0).xy;
  float ln = length(vn);
  if (view != 0) {
    vec3 c = texture(scene, uv).rgb;
    float m = view == 1 ? length(blur_vec(rc)) : ln;
    vec3 h = rc.w > 0.5 ? vec3(1.0, 0.0, 1.0) : (m < 0.5 ? c * 0.35 : heat(m / rmax));
    color = vec4(view == 2 && rc.w < 0.5 && m >= 0.5 ? mix(c, h, 0.6) : h, 1.0);
    return;
  }
  if (ln < 0.5 || rc.w > 0.5) {
    discard;
  }
  vec2 vc = blur_vec(rc);
  float lc = length(vc);
  float zc = rc.z;
  // the pixel's own direction where it moves, scaled to the neighbourhood's reach
  vec2 valt = lc > 0.5 ? vc * (ln / lc) : vn;

  vec3 c0 = texture(scene, uv).rgb;
  float wsum = 1.0 / max(lc, 0.5);
  vec3 sum = c0 * wsum;
  // interleaved gradient noise, a new phase each frame for the temporal resolve
  float j = fract(52.9829189 * fract(dot(gl_FragCoord.xy + frame * 5.588238,
                                         vec2(0.06711056, 0.00583715)))) - 0.5;
  for (int i = 0; i < taps; i++) {
    // mirrored pairs; every other pair along the pixel's own direction
    int k = i / 2;
    float t = (float(k) + 0.5 + j) / float(taps / 2);
    t = (i & 1) == 0 ? t : -t;
    vec2 dir = (k & 1) == 0 ? vn : valt;
    vec2 off = dir * t;
    vec2 sp = gl_FragCoord.xy + off;
    ivec2 sy = clamp(ivec2(sp), ivec2(0), ivec2(size) - 1);
    vec4 ry = texelFetch(rel, sy, 0);
    vec2 vy = blur_vec(ry);
    float ly = length(vy);
    float zy = ry.w > 0.5 ? 1e-3 : ry.z;
    float d = length(off);
    float f = in_front(zc, zy);
    float b = in_front(zy, zc);
    float a = f * cone(d, ly) + b * cone(d, lc) + 2.0 * cylinder(d, ly) * cylinder(d, lc);
    // HUD models are never gathered
    a *= ry.w > 0.5 ? 0.0 : 1.0;
    sum += texture(scene, sp / size).rgb * a;
    wsum += a;
  }
  color = vec4(sum / wsum, 1.0);
}
