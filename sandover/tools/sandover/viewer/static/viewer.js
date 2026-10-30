// Review viewer: pick any ripped glb, swap texture sets and shading modes, place a light.
// Texture names come from glTF texture/image `name`, which the rip sets to the PNG stem.
import * as THREE from 'three';
import { GLTFLoader } from './vendor/three/loaders/GLTFLoader.js';
import { OrbitControls } from './vendor/three/controls/OrbitControls.js';
import { TransformControls } from './vendor/three/controls/TransformControls.js';
import { mergeVertices } from './vendor/three/utils/BufferGeometryUtils.js';
import { EmissiveBloom } from './bloom.js';

// merc2.frag: alpha is doubled (PS2 128 = opaque), then discarded below 0.128.
const ENGINE_ALPHA_CUTOFF = 0.128;
const LEGACY_ALPHA_CUTOFF = 0.1;
const LAST_MODEL_KEY = 'sandover-viewer:last-model';
// A texture-set entry `<set>+maps` is that set's albedo plus the derived normal/roughness maps.
const MAPS_SUFFIX = '+maps';
const HIGHLIGHT_COLOR = 0xffcf3f;
const HIGHLIGHT_PERIOD = 1.2; // seconds
const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

// envmap/highlight/inspect-flash overlay meshes shadow a real primitive's geometry; every
// traversal that measures or reshapes the model (fitBox, applyNormals) must skip them.
const OVERLAY_SUFFIXES = ['-envmap-pass', '-highlight-pass', '-inspect-flash-pass'];
function isOverlayMeshName(name) {
  return OVERLAY_SUFFIXES.some((s) => name.endsWith(s));
}

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const wrap = $('canvas-wrap');

const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(wrap.clientWidth, wrap.clientHeight);
wrap.appendChild(renderer.domElement);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(50, wrap.clientWidth / wrap.clientHeight, 0.01, 10000);
camera.position.set(2, 2, 2);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
scene.add(new THREE.AmbientLight(0xffffff, 0.4));

const state = {
  set: null,
  mode: 'unlit',
  alpha: 'engine',
  envmap: true,
  envStrength: 0.5,
  eyes: true,
  wireframe: false,
  modelId: null,
  normals: 'file', // 'file': NORMAL if present else smooth; 'smooth'/'flat': always
  highlightStem: null,
  pulseEnabled: !reducedMotion,
  mapStrength: 1.0,
};

// stem -> { tpage, metallic, recipe, flat } from `sandover-materials derive-maps`
let pbrIndex = {};
const loadedPbrMaps = {}; // `${stem}/${kind}` -> Promise<THREE.Texture|null>
let metalEnv = null;

function baseSet() {
  return state.set && state.set.endsWith(MAPS_SUFFIX) ? state.set.slice(0, -MAPS_SUFFIX.length) : state.set;
}
function mapsOn() {
  return state.mode === 'pbr' && !!state.set && state.set.endsWith(MAPS_SUFFIX);
}

let current = null; // { entry, root, records, mixer, fit, clips }
let bloom = null; // EmissiveBloom when ?bloom=1&emissive=<stem>
let loadToken = 0;
let catalogue = [];
const textureIndexCache = {}; // setName -> { texName: relPath }
const loadedTextures = {}; // `${set}:${name}` -> Promise<THREE.Texture>
const textureLoader = new THREE.TextureLoader();
const clock = new THREE.Clock();

// ---- inspect (Shift+click) ----
const raycaster = new THREE.Raycaster();
const pointerNDC = new THREE.Vector2();
const texelCanvasCache = new WeakMap(); // image -> offscreen canvas, for texel sampling
let pointerDownAt = null; // { x, y, shift }, to tell a click from a drag-to-orbit
let inspectFlash = null; // { overlay, timer }: a short tint on the clicked primitive
let inspectTarget = null; // override row the Mark buttons post, or null with the panel closed

// Unlit/baked do the engine's math on raw sRGB bytes, like the PS2/PC renderer: no decode/encode.
function gammaSpace() {
  return state.mode !== 'pbr';
}
function uiColor(value) {
  return new THREE.Color().setStyle(value, gammaSpace() ? THREE.LinearSRGBColorSpace : THREE.SRGBColorSpace);
}
let ground = null;
let axes = null;
function applyBackground() {
  scene.background = uiColor($('bgColor').value);
  if (ground) ground.material.color.copy(uiColor('#555555'));
}

// ---- light + gizmo ----
const pointLight = new THREE.PointLight(0xffffff, 3, 0, 0);
pointLight.add(new THREE.Sprite(new THREE.SpriteMaterial({ map: makeLightIcon(), depthTest: false })));
scene.add(pointLight);
const gizmo = new TransformControls(camera, renderer.domElement);
gizmo.setSize(0.7);
gizmo.attach(pointLight);
const gizmoHelper = gizmo.getHelper();
scene.add(gizmoHelper);
gizmo.addEventListener('dragging-changed', (e) => {
  controls.enabled = !e.value;
});
gizmo.addEventListener('objectChange', syncLightReadout);

function makeLightIcon() {
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const g = c.getContext('2d');
  const grad = g.createRadialGradient(32, 32, 4, 32, 32, 30);
  grad.addColorStop(0, 'rgba(255,245,200,1)');
  grad.addColorStop(0.35, 'rgba(255,220,120,0.9)');
  grad.addColorStop(1, 'rgba(255,200,80,0)');
  g.fillStyle = grad;
  g.fillRect(0, 0, 64, 64);
  const t = new THREE.CanvasTexture(c);
  t.colorSpace = THREE.SRGBColorSpace;
  return t;
}

function resetLight() {
  const fit = current ? current.fit : { center: new THREE.Vector3(), radius: 1 };
  pointLight.position.copy(fit.center).add(new THREE.Vector3(0.6, 0.45, 0.9).multiplyScalar(fit.radius));
  pointLight.children[0].scale.setScalar(fit.radius * 0.08);
  syncLightReadout();
}
// Readouts are metres relative to the model centre; the rip divides game units by 4096 and
// keeps the game's axes (Y up, a character faces +Z), so game units = metres x 4096.
function syncLightReadout() {
  const c = current ? current.fit.center : new THREE.Vector3();
  const rel = pointLight.position.clone().sub(c);
  $('lightX').value = rel.x.toFixed(2);
  $('lightY').value = rel.y.toFixed(2);
  $('lightZ').value = rel.z.toFixed(2);
  const g = pointLight.position.clone().multiplyScalar(4096);
  $('lightGame').textContent = `game units, model space: ${g.x.toFixed(0)}, ${g.y.toFixed(0)}, ${g.z.toFixed(0)}`;
}
function updateLightVisibility() {
  const on = state.mode === 'pbr';
  pointLight.visible = on;
  gizmo.enabled = on;
  gizmoHelper.visible = on;
}

// ---- textures ----
async function fetchJSON(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.json();
}

function loadTexture(url) {
  return new Promise((resolve, reject) => {
    textureLoader.load(url, (t) => {
      t.flipY = false;
      resolve(t);
    }, undefined, reject);
  });
}

function getPbrMap(stem, kind) {
  const key = `${stem}/${kind}`;
  if (!loadedPbrMaps[key]) {
    loadedPbrMaps[key] = loadTexture(`/api/pbr-map/${encodeURIComponent(stem)}/${kind}`)
      .then((t) => { t.colorSpace = THREE.NoColorSpace; return t; })
      .catch(() => null);
  }
  return loadedPbrMaps[key];
}

// Derived maps for one record, or null: {normal, rough, metallic}.
async function resolvePbrMaps(rec, albedo) {
  const entry = rec.texName && pbrIndex[rec.texName];
  if (!entry || !mapsOn() || (rec.eye && state.eyes)) return null;
  const [normal, rough] = await Promise.all([getPbrMap(rec.texName, 'normal'), getPbrMap(rec.texName, 'rough')]);
  for (const t of [normal, rough]) {
    if (t && albedo) {
      t.wrapS = albedo.wrapS;
      t.wrapT = albedo.wrapT;
    }
  }
  return { normal, rough, metallic: entry.metallic || 0 };
}

// Neutral sky/ground gradient, only for metallic surfaces (metal has no diffuse to show).
function getMetalEnv() {
  if (metalEnv) return metalEnv;
  const envScene = new THREE.Scene();
  const geo = new THREE.SphereGeometry(1, 32, 16);
  const cols = [];
  const pos = geo.attributes.position;
  for (let i = 0; i < pos.count; i++) {
    const t = pos.getY(i) * 0.5 + 0.5;
    cols.push(0.25 + 0.6 * t, 0.25 + 0.62 * t, 0.25 + 0.7 * t);
  }
  geo.setAttribute('color', new THREE.Float32BufferAttribute(cols, 3));
  envScene.add(new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.BackSide })));
  const pmrem = new THREE.PMREMGenerator(renderer);
  metalEnv = pmrem.fromScene(envScene, 0.02).texture;
  pmrem.dispose();
  geo.dispose();
  return metalEnv;
}

async function getTexture(setName, texName) {
  const rel = textureIndexCache[setName] && textureIndexCache[setName][texName];
  if (!rel) return null;
  const key = `${setName}:${texName}`;
  if (!loadedTextures[key]) loadedTextures[key] = loadTexture(`/textures/${setName}/${rel}`);
  return loadedTextures[key];
}

function setColorSpace(tex) {
  if (!tex) return;
  const cs = gammaSpace() ? THREE.NoColorSpace : THREE.SRGBColorSpace;
  if (tex.colorSpace !== cs) {
    tex.colorSpace = cs;
    tex.needsUpdate = true;
  }
}

// Engine eyes are drawn at runtime by the eye renderer into the eye texture slot; the rip only
// has the 4x4 programmer_eye placeholder. This stand-in is a white eyeball with a dark pupil.
let eyeTexture = null;
function standInEye() {
  if (eyeTexture) return eyeTexture;
  const c = document.createElement('canvas');
  c.width = c.height = 64;
  const g = c.getContext('2d');
  g.fillStyle = '#f2f2ee';
  g.fillRect(0, 0, 64, 64);
  g.fillStyle = '#1a1410';
  g.beginPath();
  g.arc(32, 32, 11, 0, Math.PI * 2);
  g.fill();
  g.fillStyle = '#ffffff';
  g.beginPath();
  g.arc(28, 28, 3, 0, Math.PI * 2);
  g.fill();
  eyeTexture = new THREE.CanvasTexture(c);
  eyeTexture.flipY = false;
  eyeTexture.name = 'stand-in-eye';
  return eyeTexture;
}

// ---- normals ----
// tfrag/tie/shrub carry no NORMAL (kb/facts/renderer.md #5); three.js then renders faceted
// (flatShading, derivative normals) with no smoothing across shared edges. Compute smooth
// per-vertex normals by merging a position-only proxy (so UV/color seams don't block the
// merge) and scattering the merged normal back onto the original per-vertex layout.
function computeSmoothNormalsByPosition(geometry) {
  const proxy = new THREE.BufferGeometry();
  proxy.setAttribute('position', geometry.attributes.position);
  if (geometry.index) proxy.setIndex(geometry.index);
  const merged = mergeVertices(proxy);
  merged.computeVertexNormals();
  const mergedNormal = merged.attributes.normal;
  const mergedIndex = merged.index;
  const origIndex = geometry.index;
  const posCount = geometry.attributes.position.count;
  const n = origIndex ? origIndex.count : posCount;
  const out = new Float32Array(posCount * 3);
  for (let i = 0; i < n; i++) {
    const origVtx = origIndex ? origIndex.getX(i) : i;
    const mergedVtx = mergedIndex ? mergedIndex.getX(i) : i;
    out[origVtx * 3] = mergedNormal.getX(mergedVtx);
    out[origVtx * 3 + 1] = mergedNormal.getY(mergedVtx);
    out[origVtx * 3 + 2] = mergedNormal.getZ(mergedVtx);
  }
  return new THREE.BufferAttribute(out, 3);
}

// Cached on the geometry so repeated applyMaterials() calls (texture/alpha toggles) don't
// re-run the merge; cleared with the geometry itself on model unload.
function getSmoothNormal(geometry) {
  if (!geometry.userData.smoothNormal) {
    geometry.userData.smoothNormal = computeSmoothNormalsByPosition(geometry);
  }
  return geometry.userData.smoothNormal;
}

// Applies the selected Normals mode to every mesh's geometry ahead of material creation.
// 'flat' leaves geometry alone and forces flatShading on the material instead (below).
function applyNormals() {
  if (!current) return;
  current.root.traverse((obj) => {
    if (!obj.isMesh || isOverlayMeshName(obj.name)) return;
    const geo = obj.geometry;
    if (geo.userData.fileNormal === undefined) geo.userData.fileNormal = geo.attributes.normal || null;
    if (state.normals === 'smooth') {
      geo.setAttribute('normal', getSmoothNormal(geo));
    } else if (state.normals === 'flat') {
      if (geo.userData.fileNormal) geo.setAttribute('normal', geo.userData.fileNormal);
      else geo.deleteAttribute('normal');
    } else { // 'file': use it as-is if present, else fall back to smooth
      geo.setAttribute('normal', geo.userData.fileNormal || getSmoothNormal(geo));
    }
  });
}

// ---- materials ----
function alphaPatch(rec) {
  const lines = [];
  if (state.mode === 'baked') lines.push('diffuseColor.rgb *= 2.0;');
  if (state.alpha === 'engine') {
    lines.push('#ifdef USE_MAP', 'diffuseColor.a = min(sampledDiffuseColor.a * 2.0, 1.0);', '#endif');
    if (rec.ignoreAlpha) lines.push('diffuseColor.a = 1.0;');
  }
  return lines.join('\n');
}

function makeMaterial(rec, map, pbr = null) {
  setColorSpace(map);
  const common = { map, side: THREE.DoubleSide, wireframe: state.wireframe };
  const mat = state.mode === 'pbr'
    ? new THREE.MeshStandardMaterial({
      ...common, roughness: 0.85, metalness: 0.0, flatShading: state.normals === 'flat',
    })
    : new THREE.MeshBasicMaterial({ ...common, vertexColors: state.mode === 'baked' });
  if (pbr && mat.isMeshStandardMaterial) {
    const k = state.mapStrength;
    if (pbr.normal) {
      mat.normalMap = pbr.normal;
      // glTF (+Y up) maps on derivative tangents with flipY=false need Y negated, as GLTFLoader does.
      mat.normalScale.set(k, -k);
    }
    if (pbr.rough) {
      mat.roughnessMap = pbr.rough;
      mat.roughness = 1.0;
    }
    mat.metalness = pbr.metallic;
    if (pbr.metallic > 0) mat.envMap = getMetalEnv();
  }
  if (state.alpha === 'engine') {
    // Envmapped merc effects set ignore-alpha: their texture alpha is envmap strength.
    mat.alphaTest = rec.ignoreAlpha ? 0 : ENGINE_ALPHA_CUTOFF;
    mat.transparent = rec.blend;
    mat.depthWrite = !rec.blend;
  } else {
    mat.alphaTest = LEGACY_ALPHA_CUTOFF;
  }
  const patch = alphaPatch(rec);
  mat.onBeforeCompile = (shader) => {
    shader.fragmentShader = shader.fragmentShader.replace(
      '#include <color_fragment>',
      `#include <color_fragment>\n${patch}`
    );
  };
  mat.customProgramCacheKey = () => `${state.mode}|${patch}`;
  return mat;
}

// Second pass approximating emerc: additive envmap lookup by view normal, scaled by the
// base texture's alpha (x2), which is what the PS2 used as envmap strength.
function makeEnvMaterial(envTex, baseMap) {
  setColorSpace(envTex);
  setColorSpace(baseMap);
  const mat = new THREE.MeshMatcapMaterial({
    matcap: envTex, map: baseMap, side: THREE.DoubleSide, transparent: true,
    blending: THREE.AdditiveBlending, depthWrite: false, flatShading: state.normals === 'flat',
  });
  const strength = state.envStrength.toFixed(3);
  mat.onBeforeCompile = (shader) => {
    shader.fragmentShader = shader.fragmentShader.replace(
      'vec3 outgoingLight = diffuseColor.rgb * matcapColor.rgb;',
      `float envAmt = 1.0;\n#ifdef USE_MAP\nenvAmt = min(sampledDiffuseColor.a * 2.0, 1.0);\n#endif\n` +
      `vec3 outgoingLight = matcapColor.rgb * envAmt * ${strength};\ndiffuseColor.a = 1.0;`
    );
  };
  mat.customProgramCacheKey = () => `env|${strength}|${state.mode}`;
  return mat;
}

// Shared by the envmap pass and the highlight overlay: a second mesh over the same geometry,
// bound to the same skeleton/morph state, drawn after the base pass.
function makeOverlayMesh(mesh, material, nameSuffix, renderOrder) {
  const overlay = mesh.isSkinnedMesh ? new THREE.SkinnedMesh(mesh.geometry, material) : new THREE.Mesh(mesh.geometry, material);
  if (mesh.isSkinnedMesh) {
    overlay.bindMode = mesh.bindMode;
    overlay.bind(mesh.skeleton, mesh.bindMatrix);
    overlay.boundingSphere = mesh.boundingSphere;
  }
  overlay.morphTargetInfluences = mesh.morphTargetInfluences;
  overlay.morphTargetDictionary = mesh.morphTargetDictionary;
  overlay.frustumCulled = false;
  overlay.renderOrder = renderOrder;
  overlay.name = `${mesh.name}-${nameSuffix}`;
  mesh.add(overlay);
  return overlay;
}

function makeEnvMesh(mesh, material) {
  return makeOverlayMesh(mesh, material, 'envmap-pass', 1);
}

// Additive tint over the primitives whose material or envmap uses the highlighted stem;
// opacity is driven per frame by updateHighlightPulse().
function makeHighlightMesh(mesh) {
  const material = new THREE.MeshBasicMaterial({
    color: HIGHLIGHT_COLOR, transparent: true, opacity: 0, depthWrite: false,
    side: THREE.DoubleSide, blending: THREE.AdditiveBlending,
  });
  return makeOverlayMesh(mesh, material, 'highlight-pass', 2);
}

// One-shot tint on a Shift+click hit, self-removing after HIGHLIGHT_FLASH_MS.
const HIGHLIGHT_FLASH_MS = 700;
function flashPrimitive(mesh) {
  if (inspectFlash) {
    clearTimeout(inspectFlash.timer);
    removeOverlay(inspectFlash.overlay);
  }
  const material = new THREE.MeshBasicMaterial({
    color: HIGHLIGHT_COLOR, transparent: true, opacity: 0.6, depthWrite: false,
    side: THREE.DoubleSide, blending: THREE.AdditiveBlending,
  });
  const overlay = makeOverlayMesh(mesh, material, 'inspect-flash-pass', 3);
  const timer = setTimeout(() => {
    removeOverlay(overlay);
    if (inspectFlash && inspectFlash.overlay === overlay) inspectFlash = null;
  }, HIGHLIGHT_FLASH_MS);
  inspectFlash = { overlay, timer };
}

function removeOverlay(overlay) {
  if (overlay.parent) overlay.parent.remove(overlay);
  overlay.material.dispose();
}

async function resolveMap(name, fallback) {
  if (name && state.set) {
    const swapped = await getTexture(baseSet(), name);
    if (swapped) {
      if (fallback) {
        swapped.wrapS = fallback.wrapS;
        swapped.wrapT = fallback.wrapT;
        swapped.magFilter = fallback.magFilter;
      }
      return { tex: swapped, found: true };
    }
  }
  return { tex: fallback, found: false };
}

async function applyMaterials() {
  if (!current) return;
  renderer.outputColorSpace = gammaSpace() ? THREE.LinearSRGBColorSpace : THREE.SRGBColorSpace;
  applyBackground();
  updateLightVisibility();
  applyNormals();
  const token = loadToken;
  const missing = new Set();
  let nMapped = 0;
  syncMapsControls();
  for (const rec of current.records) {
    let map;
    if (rec.eye && state.eyes) {
      map = standInEye();
    } else {
      const r = await resolveMap(rec.texName, rec.fallback);
      if (!r.found && rec.texName) missing.add(rec.texName);
      map = r.tex;
    }
    const pbr = await resolvePbrMaps(rec, map);
    if (pbr) nMapped++;
    if (token !== loadToken) return;
    disposeMaterial(rec.mesh.material);
    rec.mesh.material = makeMaterial(rec, map, pbr);

    if (rec.envMesh) {
      disposeMaterial(rec.envMesh.material);
      rec.mesh.remove(rec.envMesh);
      rec.envMesh = null;
    }
    if (rec.envTexName && state.envmap && !state.wireframe) {
      const env = await resolveMap(rec.envTexName, rec.envFallback);
      const base = rec.eye && state.eyes ? null : map;
      if (token !== loadToken) return;
      if (env.tex) rec.envMesh = makeEnvMesh(rec.mesh, makeEnvMaterial(env.tex, base));
    }

    if (rec.highlightMesh) {
      disposeMaterial(rec.highlightMesh.material);
      rec.mesh.remove(rec.highlightMesh);
      rec.highlightMesh = null;
    }
    if (state.highlightStem && (rec.texName === state.highlightStem || rec.envTexName === state.highlightStem)) {
      rec.highlightMesh = makeHighlightMesh(rec.mesh);
    }
  }
  current.missing = [...missing].sort();
  renderInfo();
  updateHighlightLegend();
  const mapsNote = mapsOn() ? ` | maps on ${nMapped}/${current.records.length} prims x${state.mapStrength.toFixed(2)}` : '';
  statusEl.textContent = `${current.entry.file} | set=${state.set} mode=${state.mode} alpha=${state.alpha}` + mapsNote + bloomStatus();
  document.body.dataset.ready = '1';
}

function syncMapsControls() {
  const have = Object.keys(pbrIndex).length > 0;
  $('pbrMaps').disabled = !have;
  $('pbrMaps').checked = mapsOn();
  $('mapStrength').disabled = !mapsOn();
  $('pbrMapsHint').textContent = have ? (state.mode === 'pbr' ? '' : 'PBR preview only')
    : 'no derived maps (sandover-materials derive-maps)';
}

function disposeMaterial(mat) {
  if (!mat) return;
  for (const m of Array.isArray(mat) ? mat : [mat]) m.dispose();
}

// ---- model loading ----
const gltfLoader = new GLTFLoader();

function disposeCurrent() {
  if (!current) return;
  if (inspectFlash) { clearTimeout(inspectFlash.timer); inspectFlash = null; }
  closeInspectPanel();
  if (current.mixer) current.mixer.stopAllAction();
  if (bloom) bloom.disposeRecordMaterials(current.records);
  const textures = new Set();
  current.root.traverse((obj) => {
    if (obj.geometry) obj.geometry.dispose();
    for (const m of obj.material ? (Array.isArray(obj.material) ? obj.material : [obj.material]) : []) {
      m.dispose();
    }
    if (obj.isSkinnedMesh && obj.skeleton) obj.skeleton.dispose();
  });
  for (const rec of current.records) {
    if (rec.fallback) textures.add(rec.fallback);
    if (rec.envFallback) textures.add(rec.envFallback);
  }
  for (const t of textures) t.dispose();
  scene.remove(current.root);
  for (const h of [ground, axes]) {
    if (!h) continue;
    scene.remove(h);
    h.geometry.dispose();
    h.material.dispose();
  }
  ground = axes = null;
  current = null;
}

function modelUrl(entry) {
  return entry.id === '@cli' ? '/model.glb' : `/models/${entry.id.split('/').map(encodeURIComponent).join('/')}`;
}

// Bounds over the vertices the index buffers use, skinned and morphed as drawn (merc glbs carry
// the whole level's vertex pool). 1st-99th percentile per axis: clips park unused props far away.
// ``onlyMeshes``, when given, restricts the bounds to that set (the highlighted primitives).
function fitBox(root, onlyMeshes = null) {
  root.updateMatrixWorld(true);
  const xs = [], ys = [], zs = [];
  const v = new THREE.Vector3();
  root.traverse((obj) => {
    if (!obj.isMesh || isOverlayMeshName(obj.name)) return;
    if (onlyMeshes && !onlyMeshes.has(obj)) return;
    const idx = obj.geometry.index;
    const n = idx ? idx.count : obj.geometry.attributes.position.count;
    const seen = new Set();
    for (let i = 0; i < n; i++) {
      const vi = idx ? idx.getX(i) : i;
      if (seen.has(vi)) continue;
      seen.add(vi);
      obj.getVertexPosition(vi, v).applyMatrix4(obj.matrixWorld);
      xs.push(v.x); ys.push(v.y); zs.push(v.z);
    }
  });
  const box = new THREE.Box3(new THREE.Vector3(-1, -1, -1), new THREE.Vector3(1, 1, 1));
  if (xs.length) {
    const q = (arr) => {
      const a = Float64Array.from(arr).sort();
      return [a[Math.floor((a.length - 1) * 0.01)], a[Math.ceil((a.length - 1) * 0.99)]];
    };
    const [x, y, z] = [q(xs), q(ys), q(zs)];
    box.set(new THREE.Vector3(x[0], y[0], z[0]), new THREE.Vector3(x[1], y[1], z[1]));
  }
  const size = box.getSize(new THREE.Vector3());
  return { box, center: box.getCenter(new THREE.Vector3()), radius: Math.max(size.x, size.y, size.z, 0.5) * 0.6 };
}

// Records whose material or envmap uses the highlighted stem, in the current model.
function highlightRecords() {
  if (!current || !state.highlightStem) return [];
  return current.records.filter((r) => r.texName === state.highlightStem || r.envTexName === state.highlightStem);
}

// Bounds of just the highlighted primitives, or null when nothing in this model matches.
function highlightFit() {
  const recs = highlightRecords();
  if (!recs.length) return null;
  return fitBox(current.root, new Set(recs.map((r) => r.mesh)));
}

function frameCamera({ center, radius }) {
  const dist = radius / Math.sin(THREE.MathUtils.degToRad(camera.fov) / 2);
  controls.target.copy(center);
  camera.position.copy(center).add(new THREE.Vector3(0.45, 0.3, 1).normalize().multiplyScalar(dist));
  camera.near = radius / 100;
  camera.far = radius * 200;
  camera.updateProjectionMatrix();
  controls.update();
}

// Frames the highlighted primitives when a highlight is active and matches this model,
// else the whole model (also used as the R shortcut / Reset view button).
function resetView() {
  if (!current) return;
  frameCamera(highlightFit() || emissiveFit() || current.fit);
}

// Bounds of the primitives using the bloom preview's texture, or null.
function emissiveFit() {
  if (!bloom) return null;
  const meshes = current.records.filter((r) => r.texName === bloom.stem).map((r) => r.mesh);
  return meshes.length ? fitBox(current.root, new Set(meshes)) : null;
}

function bloomStatus() {
  if (!bloom) return '';
  const n = current ? current.records.filter((r) => r.texName === bloom.stem).length : 0;
  const map = bloom.mapStatus === 'missing' ? ', no emissive map' : '';
  return ` | bloom ${bloom.stem} x${bloom.strength.toFixed(3)} (${n} prim${n === 1 ? '' : 's'}${map}; approximation)`;
}

// Everything that must not reach the bloom's bright pass: helpers and overlay meshes.
function bloomHidden() {
  const out = [gizmoHelper, pointLight];
  if (ground) out.push(ground);
  if (axes) out.push(axes);
  for (const r of current.records) {
    if (r.envMesh) out.push(r.envMesh);
    if (r.highlightMesh) out.push(r.highlightMesh);
  }
  if (inspectFlash) out.push(inspectFlash.overlay);
  return out;
}

// The reviewer's ladder posts strength changes so the iframe needn't reload.
window.addEventListener('message', (e) => {
  if (!bloom || !/^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(e.origin)) return;
  const d = e.data;
  if (!d || d.type !== 'sandover-bloom-strength') return;
  bloom.strength = d.strength;
  if (current) statusEl.textContent = statusEl.textContent.replace(/ \| bloom .*$/, '') + bloomStatus();
});

function updateHighlightPulse() {
  if (!current || !state.highlightStem) return;
  const opacity = state.pulseEnabled
    ? 0.18 + 0.34 * (0.5 + 0.5 * Math.sin((clock.getElapsedTime() % HIGHLIGHT_PERIOD) / HIGHLIGHT_PERIOD * Math.PI * 2))
    : 0.32; // static tint: pulse stopped, or prefers-reduced-motion
  for (const r of current.records) {
    if (r.highlightMesh) r.highlightMesh.material.opacity = opacity;
  }
}

function updateHighlightLegend() {
  const el = $('highlightLegend');
  if (!state.highlightStem) {
    el.hidden = true;
    return;
  }
  el.hidden = false;
  const n = highlightRecords().length;
  $('highlightLabel').textContent = `highlighted: ${state.highlightStem}` +
    (n ? ` (${n} primitive${n === 1 ? '' : 's'})` : ' (not used by this model)');
  $('toggleHighlightPulse').textContent = state.pulseEnabled ? 'Stop pulse' : 'Resume pulse';
}

async function loadModel(entry) {
  const token = ++loadToken;
  delete document.body.dataset.ready;
  statusEl.textContent = `loading ${entry.id} ...`;
  $('modelSearch').value = entry.id;
  let gltf;
  try {
    gltf = await gltfLoader.loadAsync(modelUrl(entry));
  } catch (err) {
    if (token === loadToken) statusEl.textContent = `load error: ${err.message || err}`;
    console.error(err);
    return;
  }
  if (token !== loadToken) return;
  disposeCurrent();
  state.modelId = entry.id;
  try {
    localStorage.setItem(LAST_MODEL_KEY, entry.id);
  } catch (e) { /* storage unavailable */ }

  const root = gltf.scene;
  const records = [];
  let triangles = 0;
  const meshNames = new Set();
  root.traverse((obj) => {
    if (!obj.isMesh) return;
    // Merc glbs carry the whole level's vertex pool; unreferenced verts have out-of-range
    // JOINTS_0, which crashes three.js's skinned bounds (culling and sort). Never compute them.
    obj.frustumCulled = false;
    if (obj.isSkinnedMesh) obj.boundingSphere = new THREE.Sphere();
    const src = obj.material;
    const map = src && src.map;
    const spec = src && src.specularColorMap;
    const texName = map ? map.name : null;
    // The glb's scene-root child this primitive descends from, i.e. the tfrag/tie/shrub
    // "tree" name the sidecar keys on (sandover.unbake.sidecar); null off a level chunk.
    let treeNode = obj;
    while (treeNode.parent && treeNode.parent !== root) treeNode = treeNode.parent;
    records.push({
      mesh: obj,
      texName,
      fallback: map || null,
      envTexName: spec ? spec.name : null,
      envFallback: spec || null,
      ignoreAlpha: !!spec && !src.transparent,
      blend: !!(src && src.transparent),
      eye: !!texName && texName.startsWith('programmer_eye'),
      treeName: treeNode.parent === root ? treeNode.name : null,
      envMesh: null,
      highlightMesh: null,
    });
    meshNames.add(obj.parent && obj.parent.type === 'Group' ? obj.parent.uuid : obj.uuid);
    const idx = obj.geometry.index;
    triangles += (idx ? idx.count : obj.geometry.attributes.position.count) / 3;
  });

  scene.add(root);
  const mixer = gltf.animations.length ? new THREE.AnimationMixer(root) : null;
  const roots = new Set();
  root.traverse((o) => {
    if (o.isBone && o.parent && o.parent.isBone && !(o.parent.parent && o.parent.parent.isBone)) roots.add(o);
  });
  current = {
    entry, root, records, mixer, fit: null, clips: gltf.animations, triangles, meshCount: meshNames.size, missing: [],
    roots: [...roots].map((bone) => ({ bone, position: bone.position.clone(), quaternion: bone.quaternion.clone() })),
  };

  const pose = $('pose');
  pose.innerHTML = '';
  pose.add(new Option('Bind pose', ''));
  for (const clip of gltf.animations) pose.add(new Option(clip.name || '(clip)', clip.name));
  pose.value = '';
  applyPose();
  if (gltf.animations.length) {
    // Default to the first clip unless it throws parts far from the bind-pose bounds.
    const bindRadius = current.fit.radius;
    pose.value = gltf.animations[0].name;
    applyPose();
    if (current.fit.radius > bindRadius * 1.5) {
      pose.value = '';
      applyPose();
    }
  }
  resetView();
  resetLight();
  await applyMaterials();
}

// Refit after a pose change: ground, axes and skinned bounding spheres follow the posed mesh.
function refit() {
  const fit = fitBox(current.root);
  current.fit = fit;
  current.root.traverse((obj) => {
    if (obj.isSkinnedMesh) obj.boundingSphere.set(fit.center.clone(), fit.radius);
  });
  for (const h of [ground, axes]) {
    if (!h) continue;
    scene.remove(h);
    h.geometry.dispose();
    h.material.dispose();
  }
  ground = null;
  if (current.entry.kind !== 'background') {
    const g = fit.radius * 6;
    ground = new THREE.Mesh(new THREE.PlaneGeometry(g, g), new THREE.MeshBasicMaterial({ side: THREE.DoubleSide }));
    ground.rotation.x = -Math.PI / 2;
    ground.position.y = fit.box.min.y;
    scene.add(ground);
    applyBackground();
  }
  axes = new THREE.AxesHelper(fit.radius * 0.5);
  axes.position.set(fit.center.x, fit.box.min.y, fit.center.z);
  axes.visible = $('axes').checked;
  scene.add(axes);
}

// Ripped clips carry root motion on the joint under the top-level joints ("main"); the game
// keeps the actor in place, so hold its horizontal translation at the clip's first frame.
function pinRoots() {
  for (const r of current.roots) {
    r.bone.position.x = r.position.x;
    r.bone.position.z = r.position.z;
  }
}

function applyPose() {
  if (!current) return;
  if (current.mixer) {
    current.mixer.stopAllAction();
    const clip = current.clips.find((c) => c.name === $('pose').value);
    if (clip) {
      current.mixer.clipAction(clip).play();
      current.mixer.setTime(0);
      for (const r of current.roots) r.position.copy(r.bone.position);
    } else {
      current.root.traverse((o) => { if (o.isSkinnedMesh) o.skeleton.pose(); });
    }
  }
  refit();
}

function renderInfo() {
  const el = $('modelInfo');
  if (!current) {
    el.textContent = 'No model loaded.';
    return;
  }
  const r = current.records;
  const env = r.filter((x) => x.envTexName).length;
  const eyes = r.filter((x) => x.eye).length;
  const e = current.entry;
  const lines = [
    `<b>${escapeHtml(e.id)}</b>`,
    `${current.meshCount} mesh(es), <b>${r.length}</b> primitives, ${Math.round(current.triangles)} triangles`,
    `${e.skinned ? 'skinned' : 'static'}, ${(e.size / 1048576).toFixed(1)} MiB, ${current.clips.length} clip(s)`,
    `${env} envmapped primitive(s)${eyes ? `, ${eyes} eye placeholder(s)` : ''}`,
  ];
  el.innerHTML = lines.join('<br>');
  const t = $('textureInfo');
  t.innerHTML = current.missing.length
    ? `<span class="warn">${current.missing.length} not in set "${escapeHtml(state.set)}" (embedded copy used):</span> ${current.missing.map(escapeHtml).join(', ')}`
    : `All ${new Set(r.map((x) => x.texName).filter(Boolean)).size} textures found in "${escapeHtml(state.set)}".`;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

// ---- inspect (click-to-inspect + emissive marking) ----
// Shift+click raycasts to a primitive, samples its material/texel/E and lets a human mark
// it emissive/not-emissive; see kb/howto/viewer.md "Inspect panel".

function recordForMesh(mesh) {
  return current ? current.records.find((r) => r.mesh === mesh) : null;
}

// Nearest of the hit triangle's three vertices, in world space (skinned/morphed as drawn).
function nearestFaceVertex(mesh, face, point) {
  const v = new THREE.Vector3();
  let best = face.a, bestD = Infinity;
  for (const vi of [face.a, face.b, face.c]) {
    const d = mesh.getVertexPosition(vi, v).applyMatrix4(mesh.matrixWorld).distanceToSquared(point);
    if (d < bestD) { bestD = d; best = vi; }
  }
  return best;
}

// Texel colour at a UV: draws the texture's image to an offscreen canvas once (cached per
// image) and reads it back. flipY is false on every texture here (glTF convention), so v
// indexes the image's rows directly, no Y flip. Alpha is doubled (PS2 128 = opaque), as
// everywhere else in this app.
function sampleTexel(tex, uv) {
  if (!tex || !tex.image || !uv || !tex.image.width) return null;
  const img = tex.image;
  let canvas = texelCanvasCache.get(img);
  if (!canvas) {
    canvas = document.createElement('canvas');
    canvas.width = img.width;
    canvas.height = img.height;
    canvas.getContext('2d').drawImage(img, 0, 0);
    texelCanvasCache.set(img, canvas);
  }
  const x = Math.min(canvas.width - 1, Math.max(0, Math.floor(uv.x * canvas.width)));
  const y = Math.min(canvas.height - 1, Math.max(0, Math.floor(uv.y * canvas.height)));
  const [r, g, b, a] = canvas.getContext('2d').getImageData(x, y, 1, 1).data;
  return { r, g, b, a: Math.min(255, a * 2) };
}

function closeInspectPanel() {
  $('inspectPanel').hidden = true;
  inspectTarget = null;
}

function renderInspectLoading(stem) {
  const panel = $('inspectPanel');
  panel.hidden = false;
  $('inspectName').textContent = stem || '(no texture)';
  $('inspectBody').innerHTML = 'looking up material and E&hellip;';
  $('inspectSwatch').style.background = 'transparent';
  $('inspectStatus').textContent = '';
}

function renderInspectPanel({ stem, material, e, texel, point, uv, level, drawId }) {
  inspectTarget = {
    combo_id: material.combo_id || '', tpage: material.tpage || '', name: stem || '',
    level: level || '', draw_id: drawId || '',
  };
  const eText = e && e.e != null ? e.e.toFixed(3) : (e && e.reason) || 'no sidecar';
  const lines = [
    `tpage: ${material.tpage ? escapeHtml(material.tpage) : '-'}`,
    `combo_id: ${material.combo_id ? escapeHtml(material.combo_id) : '-'}`,
    `class: ${material.class ? escapeHtml(material.class) : 'unclassified'}` +
      (material.tags ? ` (${escapeHtml(material.tags)})` : ''),
    `confidence: ${material.confidence || '-'}`,
    `E: ${eText}`,
    `UV: ${uv ? `${uv.x.toFixed(3)}, ${uv.y.toFixed(3)}` : '-'}`,
    `world: ${point.x.toFixed(2)}, ${point.y.toFixed(2)}, ${point.z.toFixed(2)}`,
  ];
  $('inspectName').textContent = stem || '(no texture)';
  $('inspectBody').innerHTML = lines.join('<br>');
  const sw = $('inspectSwatch');
  if (texel) {
    sw.style.background = `rgba(${texel.r}, ${texel.g}, ${texel.b}, ${(texel.a / 255).toFixed(3)})`;
    sw.title = `texel rgba(${texel.r}, ${texel.g}, ${texel.b}, ${texel.a})`;
  } else {
    sw.style.background = 'transparent';
    sw.title = 'no texel (no texture bound)';
  }
  $('inspectStatus').textContent = '';
}

async function inspectHit(hit) {
  const mesh = hit.object;
  const rec = recordForMesh(mesh);
  if (!rec) return;
  flashPrimitive(mesh);
  const stem = rec.texName;
  renderInspectLoading(stem);
  const vi = hit.face ? nearestFaceVertex(mesh, hit.face, hit.point) : null;
  const texel = sampleTexel(mesh.material && mesh.material.map, hit.uv);
  const materialReq = stem
    ? fetchJSON(`/api/material/${encodeURIComponent(stem)}`).catch(() => ({}))
    : Promise.resolve({});
  const eReq = (rec.treeName && vi != null)
    ? fetchJSON(`/api/vertex-e?model=${encodeURIComponent(current.entry.id)}` +
        `&tree=${encodeURIComponent(rec.treeName)}&vertex=${vi}`).catch(() => ({ e: null, reason: 'lookup failed' }))
    : Promise.resolve({ e: null, reason: 'no sidecar' });
  const [material, e] = await Promise.all([materialReq, eReq]);
  if (!current) return; // model swapped while the lookups were in flight
  renderInspectPanel({
    stem, material, e, texel, point: hit.point, uv: hit.uv,
    level: current.entry.level, drawId: mesh.name || '',
  });
}

async function markVerdict(verdict) {
  if (!inspectTarget) return;
  $('inspectStatus').textContent = 'saving...';
  try {
    const res = await fetch('/api/mark', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...inspectTarget, verdict, note: '' }),
    });
    $('inspectStatus').textContent = res.ok ? `saved: ${verdict || 'cleared'}` : `save failed: ${res.status}`;
  } catch (e) {
    $('inspectStatus').textContent = 'save failed (network)';
  }
}

function onCanvasPointerDown(ev) {
  pointerDownAt = { x: ev.clientX, y: ev.clientY, shift: ev.shiftKey };
}

function onCanvasPointerUp(ev) {
  const down = pointerDownAt;
  pointerDownAt = null;
  if (!down || !down.shift || !ev.shiftKey || !current) return;
  if (Math.hypot(ev.clientX - down.x, ev.clientY - down.y) > 4) return; // was a drag, not a click
  const rect = renderer.domElement.getBoundingClientRect();
  pointerNDC.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
  pointerNDC.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
  raycaster.setFromCamera(pointerNDC, camera);
  const hits = raycaster.intersectObjects(current.records.map((r) => r.mesh), false);
  if (hits.length) inspectHit(hits[0]);
}

// ---- model picker ----
const search = $('modelSearch');
const list = $('modelList');
let activeIndex = -1;
let visible = [];

function filteredModels() {
  const kind = $('kindFilter').value;
  const terms = search.value.toLowerCase().split(/\s+/).filter(Boolean);
  return catalogue.filter((m) => (kind === 'all' || m.kind === kind) && terms.every((t) => m.id.toLowerCase().includes(t)));
}

function renderList() {
  visible = filteredModels();
  list.innerHTML = '';
  if (!visible.length) {
    list.innerHTML = '<li class="empty">No matching model.</li>';
    return;
  }
  let level = null;
  visible.forEach((m, i) => {
    if (m.level !== level) {
      level = m.level;
      const g = document.createElement('li');
      g.className = 'group';
      g.textContent = level || '(root)';
      list.appendChild(g);
    }
    const li = document.createElement('li');
    li.className = 'opt' + (i === activeIndex ? ' active' : '');
    li.setAttribute('role', 'option');
    li.dataset.index = i;
    li.innerHTML = `<span class="name">${escapeHtml(m.file.replace(/\.glb$/, ''))}</span>` +
      `<span class="hint">${m.primitives}</span><span class="badge ${m.kind}">${m.kind}</span>`;
    li.addEventListener('mousedown', (ev) => {
      ev.preventDefault();
      choose(i);
    });
    list.appendChild(li);
  });
}

function openList() {
  list.hidden = false;
  search.setAttribute('aria-expanded', 'true');
  renderList();
}
function closeList() {
  list.hidden = true;
  search.setAttribute('aria-expanded', 'false');
  activeIndex = -1;
}
function choose(i) {
  const m = visible[i];
  closeList();
  search.blur();
  if (m) loadModel(m);
}

search.addEventListener('focus', () => {
  search.select();
  openList();
});
search.addEventListener('input', () => {
  activeIndex = 0;
  openList();
});
search.addEventListener('blur', () => {
  closeList();
  if (current) search.value = current.entry.id;
});
search.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const n = visible.length;
    if (!n) return;
    activeIndex = (activeIndex + (e.key === 'ArrowDown' ? 1 : n - 1)) % n;
    renderList();
    const el = list.querySelector('.opt.active');
    if (el) el.scrollIntoView({ block: 'nearest' });
  } else if (e.key === 'Enter') {
    e.preventDefault();
    choose(Math.max(activeIndex, 0));
  } else if (e.key === 'Escape') {
    search.blur();
  }
});
$('kindFilter').addEventListener('change', () => {
  if (!list.hidden) renderList();
});

// ---- capture ----
function snapshotMeta(note) {
  return {
    model: state.modelId,
    textureSet: state.set,
    shading: state.mode,
    alpha: state.alpha,
    camera: {
      position: camera.position.toArray(),
      target: controls.target.toArray(),
      fov: camera.fov,
    },
    light: {
      position: pointLight.position.toArray(),
      intensity: pointLight.intensity,
      color: `#${pointLight.color.getHexString()}`,
    },
    viewport: [renderer.domElement.width, renderer.domElement.height],
    note,
  };
}

function takeScreenshot() {
  renderer.domElement.toBlob(async (blob) => {
    const res = await fetch('/screenshot', { method: 'POST', body: blob });
    statusEl.textContent = res.ok ? `saved ${(await res.json()).saved}` : `screenshot failed: ${res.status}`;
  }, 'image/png');
}

async function saveReport(note) {
  const png = renderer.domElement.toDataURL('image/png').split(',', 2)[1];
  const res = await fetch('/report', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ png, meta: snapshotMeta(note) }),
  });
  statusEl.textContent = res.ok ? `report ${(await res.json()).saved} saved` : `report failed: ${res.status}`;
  refreshReports();
}

async function refreshReports() {
  const ul = $('reports');
  let items = [];
  try {
    items = await fetchJSON('/reports.json');
  } catch (e) {
    ul.innerHTML = '<li class="warn">could not list reports</li>';
    return;
  }
  ul.innerHTML = items.length ? '' : '<li class="hint">None yet.</li>';
  for (const r of items) {
    const li = document.createElement('li');
    li.innerHTML = `<a href="/reports/${encodeURIComponent(r.id)}/shot.png" target="_blank" rel="noopener">${escapeHtml(r.id)}</a> ` +
      `<span class="hint">${escapeHtml(r.model || '')}</span><br>${escapeHtml(r.note || '')}`;
    ul.appendChild(li);
  }
}

const dialog = $('reportDialog');
function openReportDialog() {
  $('reportNote').value = '';
  dialog.showModal();
}
dialog.addEventListener('close', () => {
  if (dialog.returnValue === 'save') saveReport($('reportNote').value);
});

// ---- wiring ----
function bindSelect(id, key) {
  $(id).addEventListener('change', (e) => {
    state[key] = e.target.value;
    applyMaterials();
  });
}
bindSelect('shadeMode', 'mode');
bindSelect('alphaMode', 'alpha');
bindSelect('normalsMode', 'normals');
$('textureSet').addEventListener('change', (e) => {
  state.set = e.target.value;
  if (state.set.endsWith(MAPS_SUFFIX) && state.mode !== 'pbr') {
    state.mode = 'pbr';
    $('shadeMode').value = 'pbr';
  }
  applyMaterials();
});
$('pbrMaps').addEventListener('change', (e) => {
  const want = `${baseSet()}${e.target.checked ? MAPS_SUFFIX : ''}`;
  if ([...$('textureSet').options].some((o) => o.value === want)) state.set = want;
  $('textureSet').value = state.set;
  if (e.target.checked && state.mode !== 'pbr') {
    state.mode = 'pbr';
    $('shadeMode').value = 'pbr';
  }
  applyMaterials();
});
$('mapStrength').addEventListener('input', (e) => {
  state.mapStrength = parseFloat(e.target.value);
  $('mapStrengthOut').textContent = state.mapStrength.toFixed(2);
});
$('mapStrength').addEventListener('change', () => applyMaterials());
$('envmap').addEventListener('change', (e) => { state.envmap = e.target.checked; applyMaterials(); });
$('envStrength').addEventListener('change', (e) => { state.envStrength = parseFloat(e.target.value); applyMaterials(); });
$('eyes').addEventListener('change', (e) => { state.eyes = e.target.checked; applyMaterials(); });
$('wireframe').addEventListener('change', (e) => { state.wireframe = e.target.checked; applyMaterials(); });
$('bgColor').addEventListener('input', applyBackground);
$('pose').addEventListener('change', applyPose);
$('axes').addEventListener('change', (e) => { if (axes) axes.visible = e.target.checked; });
$('resetView').addEventListener('click', resetView);
$('resetLight').addEventListener('click', resetLight);
$('screenshot').addEventListener('click', takeScreenshot);
$('flagBug').addEventListener('click', openReportDialog);
$('toggleHighlightPulse').addEventListener('click', () => {
  state.pulseEnabled = !state.pulseEnabled;
  updateHighlightLegend();
});
for (const axis of ['X', 'Y', 'Z']) {
  $(`light${axis}`).addEventListener('change', (e) => {
    const v = parseFloat(e.target.value);
    const c = current ? current.fit.center : new THREE.Vector3();
    const a = axis.toLowerCase();
    if (Number.isFinite(v)) pointLight.position[a] = c[a] + v;
    syncLightReadout();
  });
}
$('lightIntensity').addEventListener('input', (e) => {
  pointLight.intensity = parseFloat(e.target.value);
  $('lightIntensityOut').textContent = pointLight.intensity.toFixed(1);
});
$('lightColor').addEventListener('input', (e) => pointLight.color.set(e.target.value));

renderer.domElement.addEventListener('pointerdown', onCanvasPointerDown);
renderer.domElement.addEventListener('pointerup', onCanvasPointerUp);
$('markEmissive').addEventListener('click', () => markVerdict('emissive'));
$('markNotEmissive').addEventListener('click', () => markVerdict('not-emissive'));
$('markClear').addEventListener('click', () => markVerdict(''));
$('inspectClose').addEventListener('click', closeInspectPanel);

window.addEventListener('keydown', (e) => {
  const tag = e.target.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || e.ctrlKey || e.metaKey || e.altKey) return;
  if (dialog.open) return;
  const k = e.key.toLowerCase();
  if (k === 'r') resetView();
  else if (k === 'p') takeScreenshot();
  else if (k === 'b') openReportDialog();
  else if (k === 'e' && inspectTarget) markVerdict('emissive');
  else if (k === 'n' && inspectTarget) markVerdict('not-emissive');
});

window.addEventListener('resize', () => {
  camera.aspect = wrap.clientWidth / wrap.clientHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(wrap.clientWidth, wrap.clientHeight);
});

function animate() {
  requestAnimationFrame(animate);
  const dt = clock.getDelta();
  if (current && current.mixer) {
    current.mixer.update(dt);
    pinRoots();
  }
  updateHighlightPulse();
  controls.update();
  if (bloom && current) bloom.render(scene, camera, current.records, bloomHidden(), () => renderer.render(scene, camera));
  else renderer.render(scene, camera);
}

async function init() {
  applyBackground();
  updateLightVisibility();
  animate();
  const params = new URLSearchParams(window.location.search);
  const sets = await fetchJSON('/textures.json');
  try {
    pbrIndex = await fetchJSON('/api/pbr-index');
  } catch (e) { pbrIndex = {}; }
  const sel = $('textureSet');
  for (const name of sets) {
    sel.add(new Option(name, name));
    textureIndexCache[name] = await fetchJSON(`/textures/${encodeURIComponent(name)}`);
  }
  if (Object.keys(pbrIndex).length) {
    for (const name of sets) sel.add(new Option(`${name} + derived maps`, `${name}${MAPS_SUFFIX}`));
  }
  state.set = sets[0] || null;
  const wantSet = params.get('set');
  if (wantSet && [...sel.options].some((o) => o.value === wantSet)) state.set = wantSet;
  if (['unlit', 'baked', 'pbr'].includes(params.get('mode'))) state.mode = params.get('mode');
  $('shadeMode').value = state.mode;
  sel.value = state.set || '';
  syncMapsControls();

  state.highlightStem = params.get('highlight') || null;
  if (params.get('bloom') === '1' && params.get('emissive')) {
    const stem = params.get('emissive');
    bloom = new EmissiveBloom(renderer, { stem, strength: parseFloat(params.get('strength')) || 1 });
    await bloom.loadMap(`/api/emissive-map/${encodeURIComponent(stem)}`);
  }

  const data = await fetchJSON('/models.json');
  catalogue = data.models;
  refreshReports();
  let stored = null;
  try {
    stored = localStorage.getItem(LAST_MODEL_KEY);
  } catch (e) { /* storage unavailable */ }
  const byId = (id) => catalogue.find((m) => m.id === id);
  const wanted = params.get('model');
  const entry = (wanted && byId(wanted)) || byId(stored) || byId(data.default) ||
    catalogue.find((m) => m.kind === 'skinned') || catalogue[0];
  if (entry) await loadModel(entry);
  else statusEl.textContent = 'no models found';
}

window.viewer = {
  state, camera, controls, pointLight, gizmo, get current() { return current; }, get bloom() { return bloom; },
  loadModel, applyMaterials, resetView, fitBox, inspectHit, get inspectTarget() { return inspectTarget; },
};
init();
