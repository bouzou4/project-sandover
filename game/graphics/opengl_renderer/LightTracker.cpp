#include "LightTracker.h"

#include <algorithm>
#include <cmath>

namespace {
float dist2(const float* a, const float* b) {
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return x * x + y * y + z * z;
}

float luma(const float* c) {
  return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

template <typename S>
void step_visibility(S& s, const LightTracker::Params& p, bool force_hide) {
  if (force_hide) {
    s.visible = false;
  } else if (!s.visible && s.seen >= p.frames_in) {
    s.visible = true;
  } else if (s.visible && s.missed >= p.frames_out) {
    s.visible = false;
  }
  const float d = 1.f / (float)std::max(p.fade_frames, 1);
  s.fade = s.visible ? std::min(1.f, s.fade + d) : std::max(0.f, s.fade - d);
}
}  // namespace

void LightTracker::reset() {
  for (auto& s : m_slots) {
    s = Slot();
  }
  for (auto& w : m_world) {
    w = WSlot();
  }
  m_tiles_out.clear();
  m_world_out.clear();
}

int LightTracker::live_slots() const {
  int n = 0;
  for (const auto& s : m_slots) {
    n += s.used;
  }
  return n;
}

void LightTracker::update(const std::vector<Candidate>& cands,
                          const std::vector<World>& world,
                          float tile_m_per_m,
                          const float cam[3],
                          const Params& p,
                          bool tiles) {
  // world lights: by table row; only a slot whose row is not listed this frame can be reused,
  // the one drawn faintest
  auto drawn = [](const WSlot& w) { return w.fade * luma(w.shown); };
  bool wpresent[kWorldSlots] = {};
  std::vector<int> slot_of(world.size(), -1);
  for (size_t j = 0; j < world.size(); j++) {
    for (int i = 0; i < kWorldSlots; i++) {
      if (m_world[i].row == world[j].row) {
        slot_of[j] = i;
        wpresent[i] = true;
        break;
      }
    }
  }
  for (size_t j = 0; j < world.size(); j++) {
    int k = slot_of[j];
    if (k < 0) {
      int faintest = -1;
      for (int i = 0; i < kWorldSlots && k < 0; i++) {
        if (m_world[i].row < 0) {
          k = i;
        } else if (!wpresent[i] && (faintest < 0 || drawn(m_world[i]) < drawn(m_world[faintest]))) {
          faintest = i;
        }
      }
      k = k >= 0 ? k : faintest;
      if (k < 0) {
        continue;
      }
      m_world[k] = WSlot();
      m_world[k].row = world[j].row;
      if (world[j].ramp) {
        // the power ramp from 0 is its fade in
        m_world[k].visible = true;
        m_world[k].fade = 1.f;
      }
      wpresent[k] = true;
    }
    m_world[k].w = world[j];
    if (!world[j].ramp) {
      std::copy(world[j].power, world[j].power + 3, m_world[k].shown);
    }
    m_world[k].seen++;
    m_world[k].missed = 0;
  }
  m_world_out.clear();
  for (int i = 0; i < kWorldSlots; i++) {
    auto& s = m_world[i];
    if (s.row < 0) {
      continue;
    }
    if (!wpresent[i]) {
      s.missed++;
      s.seen = 0;
    }
    if (s.w.ramp && wpresent[i]) {
      for (int c = 0; c < 3; c++) {
        const float lim = std::max(p.ramp, 0.f) * std::max(s.w.power[c], s.shown[c]);
        s.shown[c] += std::clamp(s.w.power[c] - s.shown[c], -lim, lim);
      }
    }
    if (s.w.snap) {
      if (!wpresent[i]) {
        s = WSlot();
        continue;
      }
      s.visible = true;
      s.fade = 1.f;
    } else if (s.w.ramp && wpresent[i]) {
      // listed again while fading out: fade back up from where it is
      s.visible = true;
      s.fade = std::min(1.f, s.fade + 1.f / (float)std::max(p.fade_frames, 1));
    } else {
      step_visibility(s, p, false);
    }
    if (!s.visible && s.fade <= 0.f) {
      if (s.missed > 0) {
        s = WSlot();
      }
      continue;
    }
    if (s.fade > 0.f) {
      Out o;
      std::copy(s.w.pos, s.w.pos + 3, o.pos);
      for (int c = 0; c < 3; c++) {
        o.power[c] = s.shown[c] * s.fade;
      }
      o.radius = s.w.radius;
      o.arc = s.w.arc;
      o.added = !s.w.suppress;
      o.wide = s.w.wide;
      o.id = s.row;
      m_world_out.push_back(o);
    }
  }
  if (!tiles) {
    return;
  }
  // how much of a point's tile light the world lights take over (0..1)
  const float sin_gate = std::sin(std::min(p.emitter_tiles * tile_m_per_m, 1.f));
  auto toward_emitter = [&](const float* pos, const float* power) {
    if (!p.emitters) {
      return false;
    }
    const float a[3] = {pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]};
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lp = std::sqrt(power[0] * power[0] + power[1] * power[1] + power[2] * power[2]);
    for (const auto& e : *p.emitters) {
      const float b[3] = {e.pos[0] - cam[0], e.pos[1] - cam[1], e.pos[2] - cam[2]};
      const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
      if (la < lb - std::max(2.f, 0.1f * lb) || la > lb + e.radius) {
        continue;
      }
      const float lc = std::sqrt(e.colour[0] * e.colour[0] + e.colour[1] * e.colour[1] +
                                 e.colour[2] * e.colour[2]);
      const float hue =
          (power[0] * e.colour[0] + power[1] * e.colour[1] + power[2] * e.colour[2]) /
          std::max(lp * lc, 1e-9f);
      if (hue < 0.9f) {
        continue;
      }
      const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                          a[0] * b[1] - a[1] * b[0]};
      const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      if (dot > 0.f && std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) <= sin_gate * la * lb) {
        return true;
      }
    }
    return false;
  };
  auto suppressed = [&](const float* pos, const float* power, bool full_only) {
    float s = 0.f;
    if (toward_emitter(pos, power)) {
      return 1.f;
    }
    if (p.suppress <= 0.f) {
      return s;
    }
    for (const auto& w : m_world) {
      if (w.row < 0 || w.fade <= 0.f || (full_only && w.fade < 1.f) || !w.w.suppress) {
        continue;
      }
      const float r = p.suppress * w.w.radius;
      if (dist2(pos, w.w.pos) < r * r) {
        s = std::max(s, w.fade);
      }
    }
    return s;
  };

  // tiles: greedy nearest matching within the gate
  struct Pair {
    float d2;
    int slot, cand;
  };
  std::vector<Pair> pairs;
  const int nc = (int)cands.size();
  for (int j = 0; j < nc; j++) {
    const auto& c = cands[j];
    const float gate =
        std::max(p.gate_min_m, p.gate_tiles * tile_m_per_m * std::sqrt(dist2(c.pos, cam)));
    for (int i = 0; i < kSlots; i++) {
      if (!m_slots[i].used) {
        continue;
      }
      const float d2 = dist2(m_slots[i].pos, c.pos);
      if (d2 < gate * gate) {
        pairs.push_back({d2, i, j});
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d2 < b.d2; });
  int slot_cand[kSlots];
  std::fill(slot_cand, slot_cand + kSlots, -1);
  std::vector<bool> cand_taken(nc, false);
  for (const auto& pr : pairs) {
    if (slot_cand[pr.slot] < 0 && !cand_taken[pr.cand]) {
      slot_cand[pr.slot] = pr.cand;
      cand_taken[pr.cand] = true;
    }
  }
  const float thin_mid = 0.5f * (p.fill_lo + p.fill_hi);
  const float blend = std::clamp(p.blend, 0.01f, 1.f);
  for (int i = 0; i < kSlots; i++) {
    auto& s = m_slots[i];
    if (!s.used) {
      continue;
    }
    const int j = slot_cand[i];
    if (j < 0) {
      s.missed++;
      s.seen = 0;
      continue;
    }
    const auto& c = cands[j];
    if (s.thin && c.fill >= p.fill_hi) {
      s.thin = false;
    } else if (!s.thin && c.fill < p.fill_lo) {
      s.thin = true;
    }
    const float cover = s.thin ? p.thin_cover : 1.f;
    for (int k = 0; k < 3; k++) {
      s.pos[k] += blend * (c.pos[k] - s.pos[k]);
      s.power[k] += blend * (c.power[k] * cover - s.power[k]);
    }
    s.seen++;
    s.missed = 0;
  }
  // new candidates, brightest first; one faint slot is faded out per frame to make room
  bool evicted = false;
  for (int j = 0; j < nc; j++) {
    if (cand_taken[j]) {
      continue;
    }
    const auto& c = cands[j];
    if (suppressed(c.pos, c.power, true) >= 1.f) {
      continue;
    }
    const bool thin = c.fill < thin_mid;
    const float cover = thin ? p.thin_cover : 1.f;
    int k = -1;
    for (int i = 0; i < kSlots && k < 0; i++) {
      if (!m_slots[i].used) {
        k = i;
      }
    }
    if (k < 0) {
      if (!evicted) {
        int weakest = -1;
        for (int i = 0; i < kSlots; i++) {
          if (m_slots[i].visible && !m_slots[i].evict &&
              (weakest < 0 || luma(m_slots[i].power) < luma(m_slots[weakest].power))) {
            weakest = i;
          }
        }
        if (weakest >= 0 && luma(c.power) * cover > 2.f * luma(m_slots[weakest].power)) {
          m_slots[weakest].evict = true;
          evicted = true;
        }
      }
      continue;
    }
    auto& s = m_slots[k];
    s = Slot();
    s.used = true;
    s.thin = thin;
    for (int q = 0; q < 3; q++) {
      s.pos[q] = c.pos[q];
      s.power[q] = c.power[q] * cover;
    }
    s.seen = 1;
  }
  m_tiles_out.clear();
  for (int i = 0; i < kSlots; i++) {
    auto& s = m_slots[i];
    if (!s.used) {
      continue;
    }
    step_visibility(s, p, s.evict);
    if (!s.visible && s.fade <= 0.f) {
      if (s.missed > 0 || s.evict) {
        s = Slot();
      }
      continue;
    }
    const float k = s.fade * (1.f - suppressed(s.pos, s.power, false));
    if (k <= 0.f) {
      continue;
    }
    Out o;
    std::copy(s.pos, s.pos + 3, o.pos);
    for (int c = 0; c < 3; c++) {
      o.power[c] = s.power[c] * k;
    }
    o.radius = 0.f;
    o.arc = false;
    o.id = i;
    m_tiles_out.push_back(o);
  }
}
