// Inspect ID target, draw buffer 3 (bound only while inspect mode is on; see InspectRenderer.h).
// x: draw record (0 none), y: provoking vertex id, z/w: texture coordinate as float bits.
layout(location = 3) out uvec4 inspect_id;
uniform uint insp_rec;
flat in uint insp_vid;

void inspect_write(vec2 uv) {
  inspect_id = uvec4(insp_rec, insp_vid, floatBitsToUint(uv.x), floatBitsToUint(uv.y));
}
