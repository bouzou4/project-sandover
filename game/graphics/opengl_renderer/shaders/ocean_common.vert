#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec3 tex_coord_in;
layout (location = 3) in uvec4 fog_in;  // fog, wave weight (water), depth share

invariant gl_Position;  // the mid ocean's envmap pass depth-tests against its textured pass
centroid out vec4 fragment_color;
centroid out vec3 tex_coord;
centroid out float fog;
centroid out vec2 water_gs;  // GS pixel position, for the ocean shading view ray
centroid out vec3 water_rest;  // rest xz (m) / w and 1 / w, divided per pixel (gl_Position.w is 1)
centroid out float water_edge;  // wave weight, 0 on the mid ocean's outer edge
centroid out float water_band;  // the same before shoaling: texture hex tiling fades with it
noperspective centroid out float water_wz;  // window depth, inside the triangle under MSAA
centroid out vec3 env_tex_coord;  // merged envmap pass (ocean_water only)
centroid out vec4 env_color;

uniform int bucket;

XR_GS_VERT

// ocean shading displacement (Materials.cpp water_setup_ocean)
uniform int water_on;

WATER_WAVES

void main() {
  vec3 pos = position_in;
  water_rest = vec3(0.);
  water_edge = float(fog_in.y) / 255.;
  water_band = water_edge;
  // without tessellation the mesh stays flat; the waves are in the per-pixel normal and foam
  if (water_on != 0 && water_disp_on != 0) {
    water_depth_w = float(fog_in.z) / 255.;
    water_rest = water_rest_of(position_in);
  }
  float x = pos.x + stereo_gs.x * (pos.z - stereo_gs.y);
  vec4 h = xr_gs(vec3(x, pos.y, pos.z));
  gl_Position = vec4((h.x - 0.5 * h.w) * 16., -(h.y - 0.5 * h.w) * 32, h.z * 2 - h.w, h.w);
  // scissoring area adjust
  gl_Position.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  water_wz = h.z / h.w;
  fragment_color = vec4(rgba_in.rgb, rgba_in.a * 2);
  tex_coord = tex_coord_in;
  water_gs = pos.xy * 4096.;
  fog = 255 - fog_in.x;
  env_tex_coord = vec3(0., 0., 1.);
  env_color = vec4(0.);

  if (bucket == 0) {
    fragment_color.rgb *= 2;
  } else if (bucket == 1 || bucket == 3) {
    fragment_color *= 2;
  } else if (bucket == 4) {
    fragment_color.a = 0;
  }
}
