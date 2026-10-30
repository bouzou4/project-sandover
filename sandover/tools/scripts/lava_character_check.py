"""GPU check of the lava light on characters: the checkout's pbr_common_frag.glsl lava functions and
the exact merc2.frag lava block, compiled and run on the real driver (hidden GL 4.1 context), with
each level's field as the engine builds it.

Cases: points 0.5 m below to 3 m above the nearest lava on lava texels, normals in every direction
including zero and denormal ones, base colours 0..1.5, the three strengths (B, C-H1, C-H2) and
vanilla dir2 on/off. Fails on a non-finite output, an output above max(1.15, input) or a negative
change.

Field dumps: ``LAVA_FIELD_FR3=<out/jak1/fr3> LAVA_FIELD_DUMP=<dir> goalc-test
--gtest_filter=LavaField.RealLevels``.

Usage: ``uv run --no-sync --with moderngl python scripts/lava_character_check.py <checkout> <dump dir>``
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
merc = (D / "merc2.frag").read_text()
a = merc.index("    if ((lava_on & 2) != 0 && (pbr_mat & 31) != 10) {")
b = merc.index("\n  }\n", a)
block = merc[a:b] + "\n"

FS = """#version 410 core
out vec4 out_c;
uniform sampler2D in_p;  // xyz world position (game units), w unused
uniform sampler2D in_n;  // xyz normal
uniform sampler2D in_c;  // rgb colour before, a albedo grey
uniform mat4 lava_c2w;
uniform float lava_char;
uniform float lava_vanilla;
uniform float lava_gain;
""" + common + """
void main() {
  ivec2 q = ivec2(gl_FragCoord.xy);
  vec3 shadow_cpos = texelFetch(in_p, q, 0).xyz;
  vec3 shadow_cnrm = texelFetch(in_n, q, 0).xyz;
  vec4 cc = texelFetch(in_c, q, 0);
  vec4 color = vec4(cc.rgb, 1.0);
  vec4 T0_pbr = vec4(vec3(cc.a), 1.0);
""" + block + """
  out_c = color;
}
"""
VS = """#version 410 core
void main() { vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1); gl_Position = vec4(p, 0, 1); }
"""
ctx = moderngl.create_standalone_context(require=410)
prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
STRENGTHS = {"B": (2.0, 22.0), "C-H1": (4.5, 22.0), "C-H2": (4.5, 15.0)}
LIGHT = np.array([0.875, 0.33, 0.0])
bad = 0
for level in ["firecanyon", "lavatube", "village3", "ogre"]:
    meta = json.loads((DUMP / f"{level}.json").read_text())
    w, h = meta["w"], meta["h"]
    raw = np.fromfile(DUMP / f"{level}.field", dtype=np.float32)
    cov, near, roof = raw[:w * h], raw[w * h:2 * w * h], raw[2 * w * h:]
    cov2 = cov.reshape(h, w)
    near2 = near.reshape(h, w)
    # the engine's near texture: roof over y0 capped at nh + 100, nearest height over y0, distance
    nh = np.where(np.isfinite(near2), near2 - meta["y0"], 0.0)
    rgb = np.stack([np.minimum(roof.reshape(h, w) - meta["y0"], nh + 100.0), nh,
                    np.full_like(nh, 0.0)], -1).astype("f4")
    # distance from the field: recompute (dump has no dist): 0 on lava, else 4-neighbour steps
    dist = np.where(cov2 > 0, 0.0, 1000.0).astype("f4")  # the cases sit on lava texels
    rgb[..., 2] = np.minimum(dist, 1000)
    t_cov = ctx.texture((w, h), 1, cov2.astype("f4").tobytes(), dtype="f4")
    t_cov.build_mipmaps()
    t_cov.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
    t_near = ctx.texture((w, h), 3, rgb.tobytes(), dtype="f4")
    t_near.filter = (moderngl.LINEAR, moderngl.LINEAR)
    # cases: points over lava texels, heights -0.5..3 m above their lava
    ys, xs = np.nonzero(cov2 > 0.3)
    rng = np.random.default_rng(1)
    pick = rng.choice(len(xs), size=min(256, len(xs)), replace=False)
    hs = np.linspace(-0.5, 3.0, 16)
    normals = [(0, -1, 0), (0, 1, 0), (1, 0, 0), (0, 0, 1), (0.7, -0.7, 0), (0, 0, 0), (1e-8, 0, 0)]
    cols = [0.0, 0.3, 1.0, 1.5]
    N = len(pick) * len(hs) * len(normals) * len(cols)
    W = 1024
    H = (N + W - 1) // W
    P = np.zeros((H * W, 4), "f4")
    Nn = np.zeros((H * W, 4), "f4")
    C = np.zeros((H * W, 4), "f4")
    k = 0
    for i in pick:
        x = meta["x0"] + (xs[i] + 0.5) * meta["texel"]
        z = meta["z0"] + (ys[i] + 0.5) * meta["texel"]
        y0 = near2[ys[i], xs[i]]
        for hh in hs:
            for n in normals:
                for c in cols:
                    P[k, :3] = np.array([x, y0 + hh, z]) * 4096.0
                    Nn[k, :3] = n
                    C[k] = (c, c * 0.7, c * 0.5, 0.6)
                    k += 1
    tp = ctx.texture((W, H), 4, P.tobytes(), dtype="f4")
    tn = ctx.texture((W, H), 4, Nn.tobytes(), dtype="f4")
    tc = ctx.texture((W, H), 4, C.tobytes(), dtype="f4")
    for t in (tp, tn, tc):
        t.filter = (moderngl.NEAREST, moderngl.NEAREST)
    fbo = ctx.framebuffer(color_attachments=[ctx.texture((W, H), 4, dtype="f4")])
    for name, (gain, reach) in STRENGTHS.items():
        for vanilla in (0.0, 1.0):
            tp.use(0); tn.use(1); tc.use(2); t_cov.use(41); t_near.use(42)
            u = {"in_p": 0, "in_n": 1, "in_c": 2, "lava_tex": 41, "lava_near": 42, "lava_on": 7,
                 "pbr_mat": 5, "lava_char": 0.5, "lava_vanilla": vanilla, "lava_gain": gain,
                 "lava_view": 0,
                 "lava_box": (meta["x0"], meta["z0"], 1 / (w * meta["texel"]), 1 / (h * meta["texel"])),
                 "lava_fld": (meta["texel"], meta["y0"], reach, float(int(np.log2(max(w, h))))),
                 "lava_light": (*(LIGHT * gain), 1.0), "lava_under_k": (0.0, 64.0, 1.15)}
            for kk, v in u.items():
                if kk in prog:
                    prog[kk].value = v
            prog["lava_c2w"].write(np.eye(4, dtype="f4").tobytes())
            fbo.use()
            ctx.clear()
            ctx.vertex_array(prog, []).render(moderngl.TRIANGLE_STRIP, vertices=4)
            out = np.frombuffer(fbo.color_attachments[0].read(), "f4").reshape(-1, 4)[:k, :3]
            fin = np.isfinite(out).all(1)
            gainv = out - C[:k, :3]
            lit = (gainv.max(1) > 0.004).mean()
            mx = np.nanmax(np.where(fin[:, None], out, 0))
            ok = fin.all() and mx <= max(1.15, C[:k, :3].max()) + 1e-3 and (gainv >= -1e-4).all()
            bad += 0 if ok else 1
            print(f"{level:10s} {name:5s} vanilla {int(vanilla)}: {k} cases, non-finite {(~fin).sum()}, "
                  f"max out {mx:.3f}, max added {np.nanmax(gainv):.3f}, lit {lit:.2f} {'OK' if ok else 'FAIL'}")
sys.exit(1 if bad else 0)
