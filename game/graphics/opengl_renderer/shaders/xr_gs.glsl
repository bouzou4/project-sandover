// XR head tracking for geometry that arrives in GS coordinates (ocean, shadow volumes, sky):
// back to the game camera's view ray, into the drawn eye, projected again (set_xr_gs_uniforms).
#define XR_GS_DEFINED 1
uniform int xr_gs_on;   // 0 off, 1 reproject with the vertex depth, 2 at infinity
uniform mat3 xr_gs_rot;
uniform vec3 xr_gs_rt;  // eye rotation * eye offset, game units
uniform vec3 xr_gs_off; // GS x, y of the view axis and GS depth at infinity
uniform vec3 xr_gs_a;   // GS = off + a * (x / z, y / z, 1 / z) in game camera space
uniform vec2 xr_gs_fov;

// p: GS x and y / 4096, depth / 2^24. Returns the same units in homogeneous form, w = 1 when
// off; the caller multiplies its affine GS-to-clip mapping through by w.
vec4 xr_gs(vec3 p) {
  if (xr_gs_on == 0) {
    return vec4(p, 1.);
  }
  vec3 g = p * vec3(4096., 4096., 16777216.);
  vec3 ray = vec3((g.x - xr_gs_off.x) / xr_gs_a.x, (g.y - xr_gs_off.y) / xr_gs_a.y, 1.);
  float iw = xr_gs_on == 1 ? (g.z - xr_gs_off.z) / xr_gs_a.z : 0.;
  vec3 e = xr_gs_rot * ray - xr_gs_rt * iw;
  vec3 h = vec3(xr_gs_off.x * e.z + xr_gs_a.x * xr_gs_fov.x * e.x,
                xr_gs_off.y * e.z + xr_gs_a.y * xr_gs_fov.y * e.y,
                xr_gs_on == 1 ? xr_gs_off.z * e.z + xr_gs_a.z * iw : g.z * e.z);
  return vec4(h / vec3(4096., 4096., 16777216.), e.z);
}
