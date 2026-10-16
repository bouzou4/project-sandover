// ground mist (Swamp.h; swamp, Misty Island): exponential height fog above the water line along
// each pixel's ray. Shared by the mist pass and the SSAO composite (which fades AO under it).
uniform vec4 proj_info;
uniform float far_m;
uniform mat3 cam_to_world;  // SSAO camera space to world directions
uniform float eye_h;        // camera height above the water line (m)
uniform vec4 mist;          // x density at the water line (1/m), y 1 / scale height, z start (m), w max
uniform int sea;  // 1: rays stop at the water plane (the ocean is not in the depth copy), sky
                  // misted only in a band a few degrees over the horizon

/// Mist opacity over pixel p whose camera-space depth is z (m); v is the world ray direction.
float swamp_mist_alpha(ivec2 p, float z, out vec3 v) {
  vec3 cs = vec3(((vec2(p) + 0.5) * proj_info.xy + proj_info.zw) * z, z);
  vec3 w = cam_to_world * cs;
  float dist = length(w);
  v = w / max(dist, 1e-4);
  bool sky = z >= far_m * 0.999;
  if (sea != 0 && eye_h > 0.0 && v.y < -1e-4 && (sky || eye_h < -v.y * dist)) {
    dist = eye_h / -v.y;
    sky = false;
  }
  // sky: the ray leaves the layer well within this
  dist = sky ? 800.0 : dist;
  float d = max(dist - mist.z, 0.0);
  float y0 = eye_h + v.y * min(mist.z, dist);
  float y1 = eye_h + v.y * dist;
  // mean of exp(-y / H) over the segment, exact for a straight ray
  float e0 = exp(-max(y0, -1.0) * mist.y);
  float e1 = exp(-max(y1, -1.0) * mist.y);
  float k = (y1 - y0) * mist.y;
  float avg = abs(k) > 1e-3 ? (e0 - e1) / k : e0;
  float a = min(1.0 - exp(-mist.x * d * avg), mist.w);
  return a * (sky && sea != 0 ? 1.0 - smoothstep(0.0, 0.12, v.y) : 1.0);
}
