"""GPU check of the lava rules on background surfaces: with lava on, no surface gets darker than
with lava off, the light is finite, and walls show no step at the lava-class texture seam.

Compiles the checkout's pbr_common_frag.glsl and pbr_bg_frag.glsl on the real driver (hidden GL
4.1 context) and runs, per lava level's field (goalc-test dump, as lava_character_check.py):

- `lava_light_add` for shaded classes: points 0.5 m below to 40 m above lava texels, every normal
  direction, lit colours 0..1.5, metallic 0 and 1, roughness 0.05..1, both strengths. Fails if an
  output is non-finite or below its input in any channel.
- the lava-class branch of `pbr_background` (cut verbatim from the file): on walls (up-facing
  weight 0, normal y below 0.6) the result is never darker than the vanilla colour. Up-facing
  lava-class crust (lava floors, normal y from 0.6, fully at 0.9) is darkened by design, by at most
  the crust setting on top of the stone response; that ramp is reported (smallest ratio to vanilla).

Usage: ``uv run --no-sync --with moderngl python scripts/lava_background_check.py <checkout> <dump dir>``
"""
import json
import re
import sys
from pathlib import Path

import moderngl
import numpy as np

D = Path(sys.argv[1]) / "game/graphics/opengl_renderer/shaders"
DUMP = Path(sys.argv[2])
common = (D / "pbr_common_frag.glsl").read_text()
bg = (D / "pbr_bg_frag.glsl").read_text().replace("PBR_COMMON_FRAG\n", "")
bg = re.sub(r"^in ", "", bg, flags=re.M)  # varyings become globals set per fragment
# the lava-class branch, verbatim: from the lava test to the else of the plain classes
a = bg.index("  if (lava_m >= 0.0) {\n    // the crust of lava-class rock")
b = bg.index("  } else if ((lava_on & 2) != 0 && pbr_shaded(c)) {", a)
branch = bg[a:b] + "  }\n"

FS = """#version 410 core
out vec4 out_c;
uniform sampler2D in_p;  // xyz world position (m), w: lit colour scale
uniform sampler2D in_n;  // xyz normal, w: metallic
uniform sampler2D in_c;  // rgb vanilla colour (also the albedo and the lit colour), a roughness
uniform int mode;        // 0 lava_light_add on a shaded class, 1 the lava-class branch
""" + common + bg + """
void main() {
  ivec2 q = ivec2(gl_FragCoord.xy);
  vec4 P = texelFetch(in_p, q, 0);
  vec4 Nm = texelFetch(in_n, q, 0);
  vec4 C = texelFetch(in_c, q, 0);
  pbr_wpos = P.xyz;
  vec3 Ng = normalize(Nm.xyz);
  vec3 Nd = Ng;
  vec3 V = normalize(vec3(0.3, 0.4, 1.0));
  vec3 rgb = C.rgb;
  vec3 albedo = clamp(C.rgb, 0.0, 1.0);
  vec3 lit = C.rgb * P.w;
  float rough = C.a;
  float ef = 1.0;
  vec3 hl;
  if (mode == 0) {
    out_c = vec4(lava_light_add(lit, albedo, Ng, Nd, V, rough, 1, ef), 1.0);
  } else {
    int c = 1;
    vec3 lava_off = rgb;
    float lava_m = lava_molten(albedo);
""" + branch + """
    out_c = vec4(lit, lava_m);
  }
}
"""
VS = """#version 410 core
void main() { vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1); gl_Position = vec4(p, 0, 1); }
"""
ctx = moderngl.create_standalone_context(require=410)
prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
STRENGTHS = {"lava": (2.0, 22.0, 0.15), "strong": (4.5, 22.0, 0.3)}
LIGHT = np.array([0.875, 0.33, 0.0])
rng = np.random.default_rng(2)
bad = 0


def setu(name, v):
    if name in prog:
        prog[name].value = v


for level in ["firecanyon", "lavatube", "village3", "ogre"]:
    meta = json.loads((DUMP / f"{level}.json").read_text())
    w, h = meta["w"], meta["h"]
    raw = np.fromfile(DUMP / f"{level}.field", dtype=np.float32)
    cov2, near2, roof2 = (raw[i * w * h:(i + 1) * w * h].reshape(h, w) for i in range(3))
    nh = np.where(np.isfinite(near2), near2 - meta["y0"], 0.0)
    rgbt = np.stack([np.minimum(roof2 - meta["y0"], nh + 100.0), nh,
                     np.where(cov2 > 0, 0.0, 1000.0)], -1).astype("f4")
    t_cov = ctx.texture((w, h), 1, cov2.astype("f4").tobytes(), dtype="f4")
    t_cov.build_mipmaps()
    t_cov.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
    t_near = ctx.texture((w, h), 3, rgbt.tobytes(), dtype="f4")
    t_near.filter = (moderngl.LINEAR, moderngl.LINEAR)
    ys, xs = np.nonzero(cov2 > 0.3)
    n = 131072
    k = rng.integers(0, len(xs), n)
    x = meta["x0"] + (xs[k] + rng.random(n)) * meta["texel"]
    z = meta["z0"] + (ys[k] + rng.random(n)) * meta["texel"]
    y = near2[ys[k], xs[k]] + rng.uniform(-0.5, 40.0, n)
    nrm = rng.normal(size=(n, 3))
    nrm[: n // 4, 1] *= 0.2  # plenty of walls
    nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    col = rng.uniform(0.0, 1.5, (n, 3))
    col[: n // 8] = rng.uniform(0.0, 1.0, (n // 8, 1)) * np.array([0.9, 0.35, 0.05])  # lava-like
    scale = rng.uniform(0.5, 1.3, n)
    metal = (rng.random(n) < 0.3).astype("f4")
    rough = rng.uniform(0.05, 1.0, n)
    W = 1024
    H = n // W
    P = np.stack([x, y, z, scale], -1).astype("f4")
    N = np.concatenate([nrm, metal[:, None]], -1).astype("f4")
    C = np.concatenate([col, rough[:, None]], -1).astype("f4")
    tp = ctx.texture((W, H), 4, P.tobytes(), dtype="f4")
    tn = ctx.texture((W, H), 4, N.tobytes(), dtype="f4")
    tc = ctx.texture((W, H), 4, C.tobytes(), dtype="f4")
    for t in (tp, tn, tc):
        t.filter = (moderngl.NEAREST, moderngl.NEAREST)
    fbo = ctx.framebuffer(color_attachments=[ctx.texture((W, H), 4, dtype="f4")])
    for name, (gain, reach, crust) in STRENGTHS.items():
        tp.use(0); tn.use(1); tc.use(2); t_cov.use(41); t_near.use(42)
        u = {"in_p": 0, "in_n": 1, "in_c": 2, "lava_tex": 41, "lava_near": 42, "lava_on": 7,
             "lava_view": 0, "pbr_up": (0.0, 1.0, 0.0), "pbr_look": (1.0, 1.0, 1.0, 0.5),
             "pbr_look2": (1.0, 0.0, 1.0, 0.0), "pbr_f0": 0.04, "pbr_draw": (0.0, 0.0),
             "lava_box": (meta["x0"], meta["z0"], 1 / (w * meta["texel"]), 1 / (h * meta["texel"])),
             "lava_fld": (meta["texel"], meta["y0"], reach, float(int(np.log2(max(w, h))))),
             "lava_light": (*(LIGHT * gain), 1.5), "lava_under_k": (0.0, 64.0, 1.15),
             "lava_glow": (0.3, 0.55, 2.5, 1.0), "lava_glow2": (1.6, crust, 1.15)}
        for kk, v in u.items():
            setu(kk, v)
        if "pbr_cls3" in prog:
            prog["pbr_cls3"].value = [(1.0, 0.0, 1.0)] * 12
        if "pbr_gloss" in prog:
            prog["pbr_gloss"].value = [20.0] * 12
        for mode in (0, 1):
            setu("mode", mode)
            fbo.use()
            ctx.clear()
            ctx.vertex_array(prog, []).render(moderngl.TRIANGLE_STRIP, vertices=4)
            out = np.frombuffer(fbo.color_attachments[0].read(), "f4").reshape(-1, 4)
            fin = np.isfinite(out[:, :3]).all(1)
            if mode == 0:
                lit = col * scale[:, None]
                d = out[:, :3] - lit
                ok = fin.all() and (d >= -1e-4).all()
                print(f"{level:10s} {name:6s} light_add: {n} cases, non-finite {(~fin).sum()}, "
                      f"darker {(d < -1e-4).any(1).sum()}, max added {np.nanmax(d):.3f} "
                      f"{'OK' if ok else 'FAIL'}")
            else:
                # the shader takes the face normal toward the eye: walls and floors by |y|
                ny = np.abs(nrm[:, 1])
                wall = (ny < 0.6) & fin
                d = out[wall, :3] - col[wall]
                ok = fin.all() and (d >= -1e-4).all()
                ramp = (ny >= 0.6) & fin & (col.min(1) > 0.05)
                ratio = (out[ramp, :3] / col[ramp]).min() if ramp.any() else 1.0
                print(f"{level:10s} {name:6s} lava class: walls {wall.sum()} cases, darker than "
                      f"vanilla {(d < -1e-4).any(1).sum()}; up-facing {ramp.sum()} cases, smallest "
                      f"ratio to vanilla {ratio:.2f} (by design); non-finite {(~fin).sum()} "
                      f"{'OK' if ok else 'FAIL'}")
            bad += 0 if ok else 1
sys.exit(1 if bad else 0)
