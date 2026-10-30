#version 410 core
SSAO_NORMAL_VERT
INSPECT_VERT

// merc vertex definition
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;

// light control
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;
uniform vec3 light_dir2;
uniform vec4 light_col0;
uniform vec4 light_col1;
uniform vec4 light_col2;
uniform vec4 light_ambient;

// camera control
uniform vec4 hvdf_offset;
uniform vec4 fog_constants;

ATMO_FOG_MERC_VERT
ATMO_COMBINE_VERT
uniform mat4 perspective_matrix;

// stylized material shading (see Materials.h): camera-space position, normal and key light
uniform int pbr_on;
out vec3 pbr_mpos;
out vec3 pbr_mnrm;
out vec4 pbr_key;  // xyz toward light 0, w its share of the light
out vec3 pbr_kcol;

// fur shells (instance i is shell i + 1), off unless fur_on == 1
uniform int fur_on;
uniform vec4 fur_shape;  // shells, length (game units), gravity, curl
uniform vec3 fur_down;   // camera-space gravity direction
out float fur_h;

// output
out vec4 vtx_color;
out vec2 vtx_st;

out float fog;

// actor shadows: camera-space position and normal, the sun part of the light (lights 0 and 1)
out vec3 shadow_cpos;
out vec3 shadow_cnrm;
out vec4 vtx_sun;
out float vtx_light_lum;
out vec3 vtx_base;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

// motion vectors (MotionVectors.h): 0 off, 1 previous bones, 2 camera only (mv_cam); + 4 HUD
uniform int mv_on;
uniform mat4 mv_cam;  // current clip -> previous clip, camera motion only
uniform mat4 mv_prev_persp;
uniform vec4 mv_prev_hvdf;
uniform vec4 mv_prev_fog;
layout (std140) uniform ub_prev_bones {
  MercMatrixData prev_bones[128];
};
out vec4 mv_cur;
out vec4 mv_prev;

// the projection below, with the previous frame's camera
vec4 mv_project(vec4 vp) {
  vec4 t = mv_prev_persp * vp;
  float q = mv_prev_fog.x / t[3];
  t.xyz *= q;
  t.xyz += mv_prev_hvdf.xyz;
  t.xy -= (2048.);
  t.z /= (8388608);
  t.z -= 1;
  t.x /= (256);
  t.y /= -(128);
  t.xyz *= t.w;
  t.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  return t;
}


/*
The inputs are in registers 8, 10, 12, 25, and the outputs are 9, 11, 13, 26. The output is written over the input.

```
mula.xyzw ACC, vf15, vf08
maddz.xyzw vf09, vf16, vf08
mula.xyzw ACC, vf15, vf10
maddz.xyzw vf11, vf16, vf10
mula.xyzw ACC, vf15, vf12
maddz.xyzw vf13, vf16, vf12
addax.xyzw vf20, vf00
madda.xyzw ACC, vf27, vf25
maddz.xyzw vf26, vf28, vf25
```
*/
MERC_CLIP_AXIS_VERT
void main() {
  insp_vid = uint(gl_VertexID);
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  vec3 rotated_nrm = bones[mats[0]].R * normal_in * weights_in[0];

  // game may send garbage bones if the weight is 0, don't let NaNs sneak in.
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
    rotated_nrm += bones[mats[1]].R * normal_in * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
    rotated_nrm += bones[mats[2]].R * normal_in * weights_in[2];
  }

  rotated_nrm = normalize(rotated_nrm);
  fur_h = 0.0;
  if (fur_on == 1) {
    fur_h = float(gl_InstanceID + 1) / fur_shape.x;
    vec3 t1 = normalize(abs(rotated_nrm.y) < 0.9 ? cross(rotated_nrm, vec3(0, 1, 0))
                                                  : cross(rotated_nrm, vec3(1, 0, 0)));
    vec3 t2 = cross(rotated_nrm, t1);
    float a = fract(sin(dot(position_in, vec3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2832;
    vec3 off = rotated_nrm * fur_h + fur_down * (fur_shape.z * fur_h * fur_h) +
               (cos(a) * t1 + sin(a) * t2) * (fur_shape.w * fur_h * fur_h);
    vtx_pos.xyz += off * fur_shape.y * vtx_pos.w;
  }

  vec4 transformed = perspective_matrix * vtx_pos;

  vec3 light_intensity = light_dir0_fade.xyz * rotated_nrm.x + light_dir1_fade_en.xyz * rotated_nrm.y + light_dir2 * rotated_nrm.z;
  light_intensity = max(light_intensity, vec3(0, 0, 0));

  vec4 light_color = light_ambient
                   + light_intensity.x * light_col0
                   + light_intensity.y * light_col1
                   + light_intensity.z * light_col2;



  float Q = fog_constants.x / transformed[3];
  fog = 255 - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);
  haze_f = 0.;
  if (atmo.x > 0.) {
    float af = atmo_fog(vtx_pos.xyz / vtx_pos.w, fog_constants.y);
    haze_f = haze.z * (1. - exp(-haze.x * max(length(vtx_pos.xyz / vtx_pos.w) * (1. / 4096.) - haze.y, 0.)));
    float tw;
    fog = atmo.x > 1.5 ? atmo_combine(fog, af, haze_f, tw) : af;
  }

  transformed.xyz *= Q;
  transformed.xyz += hvdf_offset.xyz;
  transformed.xy -= (2048.);
  transformed.z /= (8388608);
  transformed.z -= 1;
  transformed.x /= (256);
  transformed.y /= -(128);
  transformed.xyz *= transformed.w;
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  ssao_cpos = gl_Position.xyw;
  mv_cur = gl_Position;
  mv_prev = gl_Position;
  if ((mv_on & 3) == 1) {
    vec4 pv = -prev_bones[mats[0]].X * p * weights_in[0];
    if (weights_in[1] > 0) {
      pv += -prev_bones[mats[1]].X * p * weights_in[1];
    }
    if (weights_in[2] > 0) {
      pv += -prev_bones[mats[2]].X * p * weights_in[2];
    }
    if (fur_on == 1) {
      // the same shell offset, scaled to the previous w
      vec4 base = -bones[mats[0]].X * p * weights_in[0];
      if (weights_in[1] > 0) {
        base += -bones[mats[1]].X * p * weights_in[1];
      }
      if (weights_in[2] > 0) {
        base += -bones[mats[2]].X * p * weights_in[2];
      }
      pv.xyz += (vtx_pos.xyz - base.xyz) * (pv.w / vtx_pos.w);
    }
    mv_prev = mv_project(pv);
  } else if ((mv_on & 3) == 2) {
    mv_prev = mv_cam * gl_Position;
  }
  ssao_vnrm = vec4(0.0);
  if (ssao_nrm == 2) {
    mat4 xb = bones[mats[0]].X * weights_in[0];
    if (weights_in[1] > 0) {
      xb += bones[mats[1]].X * weights_in[1];
    }
    if (weights_in[2] > 0) {
      xb += bones[mats[2]].X * weights_in[2];
    }
    mat4 pxb = perspective_matrix * xb;
    ssao_vnrm = vec4(ssao_clip_normal(merc_clip_axis(pxb[0]), merc_clip_axis(pxb[1]),
                                      merc_clip_axis(pxb[2]), normal_in),
                     1.0);
  }


  vtx_color = rgba * light_color;
  vtx_st = st_in;
  shadow_cpos = vtx_pos.xyz / vtx_pos.w;
  shadow_cnrm = rotated_nrm;
  vtx_sun = rgba * (light_intensity.x * light_col0 + light_intensity.y * light_col1);
  vtx_light_lum = dot(light_color.rgb, vec3(0.299, 0.587, 0.114));
  vtx_base = rgba.rgb;
  pbr_mpos = vec3(0.0);
  pbr_mnrm = vec3(0.0, 0.0, 1.0);
  pbr_key = vec4(0.0);
  pbr_kcol = vec3(0.0);
  if (pbr_on != 0) {
    pbr_mpos = vtx_pos.xyz;
    pbr_mnrm = rotated_nrm;
    float kl = dot(light_col0.rgb, vec3(0.299, 0.587, 0.114));
    float al = dot(light_ambient.rgb, vec3(0.299, 0.587, 0.114));
    pbr_key = vec4(light_dir0_fade.x, light_dir1_fade_en.x, light_dir2.x, kl / max(kl + al, 1e-3));
    pbr_kcol = light_col0.rgb / max(1.0, max(light_col0.r, max(light_col0.g, light_col0.b)));
  }
}
