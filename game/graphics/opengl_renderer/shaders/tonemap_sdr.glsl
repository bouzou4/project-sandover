// SDR tonemap of the final pass (post_processing.frag), shared with the XR 2D layer resolve
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

// toon: luminance shoulder from the knee to 1.0 at 1 + range (extended Reinhard), then vanilla's
// per-channel clip
vec3 tonemap_toon(vec3 c) {
  vec3 hot = min(c, vec3(1.0));
  float hw = clip_weight(c);
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float a = max(1.0 - tm_toon_knee, 1e-4);
  float t = max(l - tm_toon_knee, 0.0) / a;
  float w = max(1.0 + tm_toon_range - tm_toon_knee, 1e-4) / a;
  float lt = l > tm_toon_knee ? min(tm_toon_knee + a * t * (1.0 + t / (w * w)) / (1.0 + t), 1.0) : l;
  c *= l > 1e-6 ? lt / l : 1.0;
  // a channel still above 1 clips per channel, as vanilla's RGBA8 target does (sky, sunlit grass)
  c = min(c, vec3(1.0));
  // hot core or halo: toward the per-channel clip of the unshouldered colour
  return mix(c, hot, hw);
}

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
