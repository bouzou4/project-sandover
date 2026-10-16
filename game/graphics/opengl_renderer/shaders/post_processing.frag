#version 410 core

uniform sampler2D tex_T0;
uniform sampler2DMS tex_ms;  // with ms_samples > 0: the MSAA frame at 1:1, averaged here
uniform int ms_samples;
out vec4 color;
in vec2 tex_coord;

uniform vec4 color_mult;
uniform vec4 color_add;

uniform int tm_mode;  // -1 pass through (RGBA8 target), 0 clamp, 1 filmic, 2 toon
uniform float tm_exposure;
uniform float tm_knee;
uniform float tm_toon_knee;
uniform float tm_toon_range;
uniform vec2 tm_white;  // max channel where the core starts / finishes whitening
uniform vec2 tm_halo;   // max channel where a halo starts / finishes the per-channel clip

// weight toward vanilla's per-channel clip: hot cores, and halos (eco and emitter stacks clip
// per channel in vanilla too: blue to cyan-white, green to lime)
float clip_weight(vec3 c) {
  float mx = max(c.r, max(c.g, c.b));
  float hw = smoothstep(tm_white.x, max(tm_white.y, tm_white.x * 1.0001 + 1e-4), mx);
  return max(hw, smoothstep(tm_halo.x, max(tm_halo.y, tm_halo.x * 1.0001 + 1e-4), mx));
}

// toon: luminance shoulder from the knee to 1.0 at 1 + range (extended Reinhard), hue kept
vec3 tonemap_toon(vec3 c) {
  vec3 hot = min(c, vec3(1.0));
  float hw = clip_weight(c);
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float a = max(1.0 - tm_toon_knee, 1e-4);
  float t = max(l - tm_toon_knee, 0.0) / a;
  float w = max(1.0 + tm_toon_range - tm_toon_knee, 1e-4) / a;
  float lt = l > tm_toon_knee ? min(tm_toon_knee + a * t * (1.0 + t / (w * w)) / (1.0 + t), 1.0) : l;
  c *= l > 1e-6 ? lt / l : 1.0;
  // a channel still above 1: desaturate toward the luminance (at most 25%), then scale by
  // 1 / max channel like the bloom hue clip
  float m = max(c.r, max(c.g, c.b));
  if (m > 1.0) {
    c = mix(vec3(lt), c, max((1.0 - lt) / max(m - lt, 1e-6), 0.75));
    c /= max(1.0, max(c.r, max(c.g, c.b)));
  }
  // hot core or halo: toward the per-channel clip (white for eco, yellow for fire)
  return mix(c, hot, hw);
}

uniform int hdr_out;  // 1: write linear scRGB (1.0 = 80 nits) for the HDR presenter
uniform vec4 hdr_nits;  // paper white, peak, black (nits), knee (0 paper white .. 1 peak)
uniform float hdr_saturation;
uniform float hdr_ref_nits;  // paper white pattern's fixed half: Windows' SDR white, else 200

vec3 tonemap(vec3 c) {
  if (tm_mode < 0) {
    return c;
  }
  if (tm_mode == 0) {
    return clamp(c, 0.0, 1.0);
  }
  c = max(c * tm_exposure, 0.0);
  if (tm_mode == 2) {
    return tonemap_toon(c);
  }
  if (tm_knee >= 0.999) {
    return min(c, 1.0);
  }
  // identity below the knee, exponential shoulder to 1.0 with matching slope above it
  float r = 1.0 - tm_knee;
  vec3 s = tm_knee + r * (1.0 - exp(-max(c - tm_knee, 0.0) / r));
  return mix(c, s, step(tm_knee, c));
}

// Toon curve in nits: identity up to the knee, exponential shoulder to peak, on luminance
// with chroma kept; black lifts the shadows by at most `black` nits and keeps 0 at 0.
vec3 hdr_map(vec3 e) {
  float paper = hdr_nits.x;
  float peak = max(hdr_nits.y, 1.0);
  float black = hdr_nits.z;
  vec3 lin = pow(max(e, 0.0), vec3(2.2));
  const vec3 w = vec3(0.2126, 0.7152, 0.0722);
  float y = dot(lin, w);
  // toon: cores and halos take the per-channel clip's chroma at their own luminance, as SDR
  // does; keeping their hue sent stacked blue eco to deep blue and dimmed it under the peak
  vec3 hot = pow(min(max(e, 0.0), vec3(1.0)), vec3(2.2));
  float hw = tm_mode == 2 ? clip_weight(max(e, 0.0)) : 0.0;
  lin = mix(lin, hot * (y / max(dot(hot, w), 1e-6)), hw);
  lin = max(mix(vec3(y), lin, hdr_saturation), 0.0);
  y = dot(lin, w);
  if (y <= 0.0) {
    return vec3(0.0);
  }
  float x = y * paper;
  float kn = min(mix(paper, peak, clamp(hdr_nits.w, 0.0, 1.0)), 0.999 * peak);
  float r = peak - kn;
  float o = x < kn ? x : kn + r * (1.0 - exp(-(x - kn) / r));
  if (black > 0.0) {
    o += black * (1.0 - exp(-o / black));
  }
  vec3 nits = lin * (o / y);
  float m = max(nits.r, max(nits.g, nits.b));
  if (m > peak) {
    nits *= peak / m;
  }
  return nits / 80.0;
}

uniform int hdr_pq;       // direct to the swapchain image: encode HDR10 PQ here
uniform float hdr_scale;  // direct: the fade the blackout pass applies otherwise

const mat3 k709_to_2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                               0.0433, 0.0114, 0.8956);

// same encode as the HDR presenter's: linear scRGB (1.0 = 80 nits) to PQ
vec3 pq_encode(vec3 c) {
  vec3 y = pow(clamp(k709_to_2020 * c * (80.0 / 10000.0), 0.0, 1.0), vec3(0.1593017578125));
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

vec3 source() {
  if (ms_samples > 0) {
    ivec2 p = ivec2(tex_coord * vec2(textureSize(tex_ms)));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < ms_samples; i++) {
      sum += texelFetch(tex_ms, p, i).rgb;
    }
    return sum / float(ms_samples);
  }
  return texture(tex_T0, tex_coord).rgb;
}

uniform int hdr_cal;  // calibration pattern: 0 none, 1 peak, 2 paper white, 3 black

// Pattern at this pixel (a = 1 inside): linear scRGB with hdr, else SDR (the peak pattern scaled
// so 10000 nits is white). Centred low on the frame, under the menu rows; sizes in frame heights.
vec4 cal_pattern(bool hdr) {
  vec2 sz = ms_samples > 0 ? vec2(textureSize(tex_ms)) : vec2(textureSize(tex_T0, 0));
  float aspect = sz.x / max(sz.y, 1.0);
  vec2 q = vec2((tex_coord.x - 0.5) * aspect, tex_coord.y - 0.125);
  const float s = 0.09;
  const float border = 0.008;
  vec2 half_size = hdr_cal == 3 ? vec2(2.0 * s, 0.5 * s) : vec2(s);
  vec2 d = abs(q) - half_size;
  if (max(d.x, d.y) > border) {
    return vec4(0.0);
  }
  if (max(d.x, d.y) > 0.0) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  if (hdr_cal == 1) {
    // full-signal field, inner diamond at the chosen peak: it vanishes once the peak reaches
    // the level where the display clips
    float nits = abs(q.x) + abs(q.y) < 0.55 * s ? hdr_nits.y : 10000.0;
    return vec4(vec3(hdr ? nits / 80.0 : pow(nits / 10000.0, 1.0 / 2.2)), 1.0);
  }
  if (hdr_cal == 2) {
    // left: game white and mid-grey through the game's mapping; right: the same at the desktop's
    // fixed white, so the seam vanishes when paper white matches it
    float e = max(abs(q.x), abs(q.y)) < 0.45 * s ? 0.5 : 1.0;
    if (q.x < 0.0) {
      return vec4(hdr ? hdr_map(vec3(e)) : vec3(e), 1.0);
    }
    float o = min(hdr_ref_nits * pow(e, 2.2), max(hdr_nits.y, 1.0));
    o += hdr_nits.z > 0.0 ? hdr_nits.z * (1.0 - exp(-o / hdr_nits.z)) : 0.0;
    return vec4(vec3(hdr ? o / 80.0 : pow(min(o / max(hdr_nits.x, 1.0), 1.0), 1.0 / 2.2)), 1.0);
  }
  // eight near-black steps, 2/255 apart in the game's encoding; the first is true black
  float k = clamp(floor((q.x / (4.0 * s) + 0.5) * 8.0), 0.0, 7.0);
  float e = k * 2.0 / 255.0;
  return vec4(hdr ? hdr_map(vec3(e)) : vec3(e), 1.0);
}

void main() {
  vec4 cal = hdr_cal > 0 ? cal_pattern(hdr_out == 1) : vec4(0.0);
  if (hdr_out == 1) {
    vec3 e = source() * color_mult.rgb * color_mult.a + color_add.rgb;
    vec3 c = (cal.a > 0.0 ? cal.rgb : hdr_map(e)) * hdr_scale;
    color = vec4(hdr_pq == 1 ? pq_encode(c) : c, 1.0);
    return;
  }
  color = vec4(tonemap(source()) * color_mult.rgb * color_mult.a, 1.0) + color_add;
  if (cal.a > 0.0) {
    color = vec4(cal.rgb, 1.0);
  }
}
