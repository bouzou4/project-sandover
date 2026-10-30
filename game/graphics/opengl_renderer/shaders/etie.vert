#version 410 core
SSAO_NORMAL_VERT
INSPECT_VERT

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in int time_of_day_index;
layout (location = 3) in vec3 normal;
layout (location = 4) in vec4 proto_tint;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform vec4 envmap_tod_tint;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

// actor shadows: receiver position, global sun share, emissive gate
uniform float shadow_share_global;
WORLD_SHADOW_VERT
uniform usamplerBuffer ws_selflit_vtx;  // per vertex 1 under a self-lit base texture (Tie3)
out vec3 shadow_pos;
out float shadow_share;
out float shadow_receive;  // 1 - E, as tfrag3
uniform int em_on;
uniform float em_w[10];
EM_VALUE_VERT
uniform usamplerBuffer tex_T11;

// etie stuff
uniform vec4 persp0;
uniform vec4 persp1;
uniform mat4 cam_no_persp;
uniform vec4 cam_trans;

ATMO_UNIFORMS_VERT
// additive pass: fades to black with the fog instead of mixing toward the fog colour
out float atmo_fade;

ATMO_FOG_VERT
ATMO_COMBINE_VERT
void main() {
  insp_vid = uint(gl_VertexID);
  fogginess = 0;
  atmo_fade = 0.;
  haze_f = 0.;
  if (atmo.x > 0.) {
    vec3 d = (position_in - cam_trans.xyz) * (1. / 4096.);
    atmo_fade = atmo_fog(length(d), 0.5 * (position_in.y + cam_trans.y) * (1. / 4096.));
    haze_f = haze.z * (1. - exp(-haze.x * max(length(d) - haze.y, 0.)));
    if (atmo.x > 1.5) {
      // the base pass's fog (etie_base.vert), so the envmap fades with it
      float fog1 = camera[3].w + camera[0].w * position_in.x + camera[1].w * position_in.y +
                   camera[2].w * position_in.z;
      float tw;
      atmo_fade = atmo_combine(255. - clamp(fog1 + hvdf_offset.w, fog_min, fog_max), atmo_fade,
                               haze_f, tw);
    }
  }

  // rotate the normal
  vec3 nrm_vf23 = cam_no_persp[0].xyz * normal.x
                + cam_no_persp[1].xyz * normal.y
                + cam_no_persp[2].xyz * normal.z;

  // transform the point
  vec4 vf17 = cam_no_persp[3];
  vf17 += cam_no_persp[0] * position_in.x;
  vf17 += cam_no_persp[1] * position_in.y;
  vf17 += cam_no_persp[2] * position_in.z;


  // This is the ETIE math.
  // It seems right only if the nrm_vf23 is normalized first
  // (and in this case it's identical the emerc math below)
  // There is no reason to prefer the emerc version over etie - they are identical.
  // likely on PS2, their normal transformation matrix was scaled correctly.
  // ours isn't. (yes, the camera matrix is a pure rotation and that doesn't matter, but the
  // instance matrix used to de-instance the mesh needs the correction, and we don't have it)
  {
    nrm_vf23 = nrm_vf23;
    // nrm.z -= 1
    //subw.z vf23, vf23, vf00
    nrm_vf23.z -= 1.f;

    // dot = nrm.xyz * pt.xyz
    //mul.xyz vf13, vf17, vf23
    //esum.xyzw P, vf13
    //mfp.x vf13, P
    float nrm_dot = dot(vf17.xyz, nrm_vf23);

    // rfl = pt.xzy * nrm.z
    //mulz.xyz vf14, vf17, vf23
    vec3 rfl_vf14 = vf17.xyz * nrm_vf23.z;

    //;; Q_envmap = vf02.w / norm(rfl.xyz)
    //esadd.xyz P, vf14
    //mfp.x vf30, P
    //rsqrt Q, vf02.w, vf30.x
    float Q_envmap = -0.5 / length(rfl_vf14);

    //
    //;; nrm.xy *= dot.x
    //mulx.xy vf23, vf23, vf13
    nrm_vf23.xy *= nrm_dot;

    //
    //;; nrm.xy += rfl.xy
    //add.xy vf23, vf23, vf14
    nrm_vf23.xy += rfl_vf14.xy;
    //
    //;; nrm.z = 1.0
    //addw.z vf23, vf00, vf00
    nrm_vf23.z = 1.0;
    //
    //;; nrm.xy *= Q_envmap
    //mul.xy vf23, vf23, Q
    nrm_vf23.xy *= Q_envmap;
    //

    //
    //;; nrm.xy += vf03.w
    //addw.xy vf23, vf23, vf03
    nrm_vf23.xy += 0.5;
    tex_coord = nrm_vf23;
  }

  //;; perspective transform
  //mula.xy ACC, vf10, vf17  ;; acc build 1
  //mulaw.zw ACC, vf10, vf00 ;; acc build 2
  vec4 p_proj = vec4(persp1.x * vf17.x, persp1.y * vf17.y, persp1.z, persp1.w);

  //maddz.xyzw vf18, vf09, vf17 ;; acc star
  p_proj += persp0 * vf17.z;

  //;; perspective divide
  //div Q, vf00.w, vf18.w
  float pQ = 1.f / p_proj.w;

  // EMERC version of the math
//  {
//    // emerc hack
//    vec3 vf10 = normalize(r_nrm);
//    //subw.z vf10, vf10, vf00 ;; subtract 1 from z
//    vf10.z -= 1;
//    //addw.z vf09, vf00, vf09 ;; xyww the unperspected thing
//    vec4 vf09 = vf17;
//    //mul.xyz vf15, vf09, vf10 ;;
//    vec3 vf15 = vf09.xyz * vf10.xyz;
//    //adday.xyzw vf15, vf15
//    //maddz.x vf15, vf21, vf15
//    float vf15_x = vf15.x + vf15.y + vf15.z;
//    //div Q, vf15.x, vf10.z
//    float qq = vf15_x / vf10.z;
//    //mulaw.xyzw ACC, vf09, vf00
//    vec3 ACC = vf09.xyz;
//    //madd.xyzw vf10, vf10, Q
//    vf10 = ACC + vf10 * qq;
//    //eleng.xyz P, vf10
//    float P = length(vf10.xyz);
//    //mfp.w vf10, P
//    //div Q, vf23.z, vf10.w
//    float qqq = 0.5 / P;
//    //addaz.xyzw vf00, vf23
//    ACC = vec3(0.5, 0.5, 0.5);
//    //madd.xyzw vf10, vf10, Q
//    vf10 = ACC + vf10 * qqq;
//    tex_coord = vf10.xyz;
//  }


  vec4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  ssao_cpos = gl_Position.xyw;
  ssao_vnrm = ssao_etie_vnrm(cam_no_persp, persp0, persp1, SCISSOR_ADJUST * HEIGHT_SCALE,
                             ssao_nrm == 2 ? texelFetch(tex_T11, gl_VertexID) : uvec4(0u));


  fragment_color = proto_tint * envmap_tod_tint;
  shadow_pos = position_in;
  shadow_share = shadow_share_global;
  shadow_share1 = shadow_share;
  shadow_receive = em_on == 1 ? 1.0 - em_value(texelFetch(tex_T11, gl_VertexID).w) : 1.0;
  // the second pass draws by envmap texture: the self-lit flag comes per vertex
  bool selflit = ws_selflit == 2 && texelFetch(ws_selflit_vtx, gl_VertexID).r != 0u;
  shadow_receive = (ws_on & 1) != 0 && selflit ? 0.0 : shadow_receive;
}
