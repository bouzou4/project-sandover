// Emissive bloom preview (?bloom=1&emissive=<stem>&strength=<f>): an approximation of the
// engine's BloomRenderer, driven by a texture-space emissive map instead of the sidecar's E.
import * as THREE from 'three';

// Engine defaults (gfx.h): threshold 0.3, radius 6 half-res px, E ramp 0.1..0.4, strength 1.
export const BLOOM_DEFAULTS = { threshold: 0.3, radius: 6, rampLo: 0.1, rampHi: 0.4 };

const FS_VERT = `
varying vec2 vUv;
void main() { vUv = uv; gl_Position = vec4(position.xy, 0.0, 1.0); }`;

// Same kernel as bloom_blur.frag: sigma = radius / 2.
const BLUR_FRAG = `
uniform sampler2D src;
uniform vec2 stepUv;
varying vec2 vUv;
void main() {
  float sigma = max(float(RADIUS) * 0.5, 0.5);
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int i = -RADIUS; i <= RADIUS; i++) {
    float w = exp(-0.5 * float(i * i) / (sigma * sigma));
    sum += w * texture2D(src, vUv + float(i) * stepUv).rgb;
    wsum += w;
  }
  gl_FragColor = vec4(sum / wsum, 1.0);
}`;

const ADD_FRAG = `
uniform sampler2D src;
varying vec2 vUv;
void main() { gl_FragColor = vec4(texture2D(src, vUv).rgb, 1.0); }`;

export class EmissiveBloom {
  constructor(renderer, { stem, strength = 1, engineAlpha = true } = {}) {
    this.renderer = renderer;
    this.stem = stem;
    this.engineAlpha = engineAlpha;
    this.uniforms = {
      emMap: { value: null },
      emStrength: { value: strength },
      emThreshold: { value: BLOOM_DEFAULTS.threshold },
      emRamp: { value: new THREE.Vector2(BLOOM_DEFAULTS.rampLo, BLOOM_DEFAULTS.rampHi) },
    };
    this.mapStatus = 'loading';
    this.targets = null;
    this.fsCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    this.blurMat = new THREE.ShaderMaterial({
      uniforms: { src: { value: null }, stepUv: { value: new THREE.Vector2() } },
      defines: { RADIUS: BLOOM_DEFAULTS.radius },
      vertexShader: FS_VERT, fragmentShader: BLUR_FRAG, depthTest: false, depthWrite: false,
    });
    this.addMat = new THREE.ShaderMaterial({
      uniforms: { src: { value: null } }, vertexShader: FS_VERT, fragmentShader: ADD_FRAG,
      blending: THREE.AdditiveBlending, transparent: true, depthTest: false, depthWrite: false,
    });
    this.fsQuad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), this.blurMat);
    this.fsQuad.frustumCulled = false;
    this.fsScene = new THREE.Scene();
    this.fsScene.add(this.fsQuad);
  }

  get strength() { return this.uniforms.emStrength.value; }
  set strength(v) { this.uniforms.emStrength.value = Math.max(0, Number(v) || 0); }

  async loadMap(url) {
    try {
      const tex = await new THREE.TextureLoader().loadAsync(url);
      tex.flipY = false; // glTF UV convention, like the model's own maps
      tex.colorSpace = THREE.NoColorSpace;
      tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
      tex.needsUpdate = true;
      this.uniforms.emMap.value = tex;
      this.mapStatus = 'ok';
    } catch (e) {
      this.mapStatus = 'missing';
    }
  }

  // Bright-pass material for a primitive using the reviewed texture, else a black occluder.
  materialFor(rec, base) {
    const map = base && base.map ? base.map : null;
    const alphaTest = base ? base.alphaTest : 0;
    if (rec.texName !== this.stem || !map) {
      return new THREE.MeshBasicMaterial({ color: 0x000000, map, alphaTest, side: THREE.DoubleSide });
    }
    const mat = new THREE.MeshBasicMaterial({ map, alphaTest, side: THREE.DoubleSide });
    const alpha = this.engineAlpha ? 'diffuseColor.a = min(diffuseColor.a * 2.0, 1.0);' : '';
    mat.onBeforeCompile = (shader) => {
      Object.assign(shader.uniforms, this.uniforms);
      shader.fragmentShader = 'uniform sampler2D emMap;\nuniform float emStrength;\nuniform float emThreshold;\n' +
        'uniform vec2 emRamp;\n' + shader.fragmentShader.replace('#include <color_fragment>', `#include <color_fragment>
${alpha}
float emE = smoothstep(emRamp.x, emRamp.y, texture2D(emMap, vMapUv).r);
diffuseColor.rgb = emE * max(diffuseColor.rgb * emStrength - vec3(emThreshold), vec3(0.0));`);
    };
    mat.customProgramCacheKey = () => `bloom-em|${alpha}`;
    return mat;
  }

  ensureTargets() {
    const size = this.renderer.getDrawingBufferSize(new THREE.Vector2());
    const w = Math.max(1, Math.floor(size.x / 2)), h = Math.max(1, Math.floor(size.y / 2));
    if (this.targets && this.targets.w === w && this.targets.h === h) return this.targets;
    if (this.targets) { this.targets.a.dispose(); this.targets.b.dispose(); }
    const opts = { type: THREE.HalfFloatType, depthBuffer: true };
    this.targets = { w, h, a: new THREE.WebGLRenderTarget(w, h, opts), b: new THREE.WebGLRenderTarget(w, h, opts) };
    return this.targets;
  }

  // Renders the scene with `records` swapped to bright-pass materials into a half-res target,
  // blurs it, draws the normal frame via `drawFrame`, then adds the blur on top.
  render(scene, camera, records, hidden, drawFrame) {
    const r = this.renderer;
    if (!this.uniforms.emMap.value) { drawFrame(); return; }
    const t = this.ensureTargets();
    const saved = records.map((rec) => rec.mesh.material);
    records.forEach((rec, i) => {
      if (!rec.bloomMat || rec.bloomBase !== saved[i]) {
        if (rec.bloomMat) rec.bloomMat.dispose();
        rec.bloomMat = this.materialFor(rec, saved[i]);
        rec.bloomBase = saved[i];
      }
      rec.mesh.material = rec.bloomMat;
    });
    const vis = hidden.map((o) => o.visible);
    hidden.forEach((o) => { o.visible = false; });
    const bg = scene.background;
    scene.background = null;
    const clearColor = r.getClearColor(new THREE.Color());
    const clearAlpha = r.getClearAlpha();
    r.setClearColor(0x000000, 0);
    r.setRenderTarget(t.a);
    r.clear();
    r.render(scene, camera);
    scene.background = bg;
    hidden.forEach((o, i) => { o.visible = vis[i]; });
    records.forEach((rec, i) => { rec.mesh.material = saved[i]; });

    this.fsQuad.material = this.blurMat;
    this.blurMat.uniforms.src.value = t.a.texture;
    this.blurMat.uniforms.stepUv.value.set(1 / t.w, 0);
    r.setRenderTarget(t.b);
    r.render(this.fsScene, this.fsCam);
    this.blurMat.uniforms.src.value = t.b.texture;
    this.blurMat.uniforms.stepUv.value.set(0, 1 / t.h);
    r.setRenderTarget(t.a);
    r.render(this.fsScene, this.fsCam);

    r.setRenderTarget(null);
    r.setClearColor(clearColor, clearAlpha);
    drawFrame();
    this.fsQuad.material = this.addMat;
    this.addMat.uniforms.src.value = t.a.texture;
    const autoClear = r.autoClear;
    r.autoClear = false;
    r.render(this.fsScene, this.fsCam);
    r.autoClear = autoClear;
  }

  disposeRecordMaterials(records) {
    for (const rec of records) {
      if (rec.bloomMat) rec.bloomMat.dispose();
      rec.bloomMat = null;
      rec.bloomBase = null;
    }
  }
}
