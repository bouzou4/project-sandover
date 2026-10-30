#include "VistaRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace {
const std::vector<tfrag3::TFragmentTreeKind> kVistaTfragKinds = {tfrag3::TFragmentTreeKind::NORMAL};
const std::vector<tfrag3::TFragmentTreeKind> kVistaAlphaKinds = {
    tfrag3::TFragmentTreeKind::TRANS, tfrag3::TFragmentTreeKind::DIRT,
    tfrag3::TFragmentTreeKind::ICE, tfrag3::TFragmentTreeKind::WATER};
// the neighbours' own stand-ins of other places (inventory naming)
constexpr const char* kNeighbourStandins = "lores,medres";

const VistaNeighbourFog* neighbour_fog(const std::string& level) {
  const auto& gs = Gfx::g_global_settings;
  if (!gs.vista_neighbours) {
    return nullptr;
  }
  for (int i = 0; i < gs.vista_nb_fog_count; i++) {
    if (level == gs.vista_nb_fog[i].level) {
      return &gs.vista_nb_fog[i];
    }
  }
  return nullptr;
}
}  // namespace

VistaRenderer::LevelRenderers& VistaRenderer::renderers_for(const std::string& level,
                                                            SharedRenderState* render_state) {
  auto& r = m_levels[level];
  if (!r.tfrag) {
    r.tfrag = std::make_unique<TFragment>("vista-tfrag-" + level, -1, kVistaTfragKinds, false, -1,
                                          m_anim_slot_array);
    r.alpha = std::make_unique<TFragment>("vista-alpha-" + level, -1, kVistaAlphaKinds, false, -1,
                                          m_anim_slot_array);
    r.tie = std::make_unique<Tie3>("vista-tie-" + level, -1, -1, m_anim_slot_array);
    r.shrub = std::make_unique<Shrub>("vista-shrub-" + level, -1);
    r.tfrag->init_shaders(render_state->shaders);
    r.alpha->init_shaders(render_state->shaders);
    r.tie->init_shaders(render_state->shaders);
    r.shrub->init_shaders(render_state->shaders);
  }
  return r;
}

float VistaRenderer::level_distance(const std::string& level,
                                    SharedRenderState* render_state,
                                    const math::Vector4f& cam) {
  const LevelData* data = render_state->loader->get_tfrag3_level(level);
  if (!data || !data->level) {
    return -1.f;
  }
  auto& b = m_bounds[level];
  if (b.load_id != data->load_id) {
    b.load_id = data->load_id;
    b.lo.fill(std::numeric_limits<float>::max());
    b.hi.fill(-std::numeric_limits<float>::max());
    auto add = [&](const tfrag3::BVH& bvh) {
      for (const auto& n : bvh.vis_nodes) {
        for (int i = 0; i < 3; i++) {
          b.lo[i] = std::min(b.lo[i], n.bsphere[i] - n.bsphere.w());
          b.hi[i] = std::max(b.hi[i], n.bsphere[i] + n.bsphere.w());
        }
      }
    };
    for (const auto& t : data->level->tfrag_trees[0]) {
      add(t.bvh);
    }
    for (const auto& t : data->level->tie_trees[0]) {
      add(t.bvh);
    }
    for (const auto& t : data->level->shrub_trees) {
      for (const auto& v : t.unpacked.vertices) {
        const math::Vector3f p(v.x, v.y, v.z);
        b.lo = b.lo.min(p);
        b.hi = b.hi.max(p);
      }
    }
  }
  float d2 = 0.f;
  for (int i = 0; i < 3; i++) {
    const float e = std::max({b.lo[i] - cam[i], cam[i] - b.hi[i], 0.f});
    d2 += e * e;
  }
  return std::sqrt(d2);
}

bool VistaRenderer::levels_overlap(const std::string& a, const std::string& b) const {
  const auto ia = m_bounds.find(a), ib = m_bounds.find(b);
  if (ia == m_bounds.end() || ib == m_bounds.end() || ia->second.load_id == UINT64_MAX ||
      ib->second.load_id == UINT64_MAX) {
    return true;
  }
  constexpr float kMargin = 4096.f;  // 1 m
  for (int i = 0; i < 3; i++) {
    if (ia->second.lo[i] > ib->second.hi[i] + kMargin ||
        ib->second.lo[i] > ia->second.hi[i] + kMargin) {
      return false;
    }
  }
  return true;
}

bool VistaRenderer::level_in_view(const std::string& level, const math::Vector4f* planes) const {
  const auto it = m_bounds.find(level);
  if (it == m_bounds.end() || it->second.load_id == UINT64_MAX) {
    return true;
  }
  const auto& b = it->second;
  const math::Vector3f c = (b.lo + b.hi) * 0.5f;
  const float r = (b.hi - b.lo).length() * 0.5f;
  return sphere_in_view_ref(math::Vector4f(c.x(), c.y(), c.z(), r), planes);
}

void VistaRenderer::restore_frame_lights(SharedRenderState* render_state) {
  const bool reordered =
      m_hiz_skipped || Gfx::perf_opt(PERF_OPT_VISTA + 0) || Gfx::perf_opt(PERF_OPT_VISTA + 2);
  if (m_lights_level < 0 || !reordered) {
    return;
  }
  const LevelData* data = render_state->loader->get_tfrag3_level(m_lights_name);
  if (data) {
    note_background_lights(render_state, data->unbake.get(), m_lights_settings.camera);
  }
}

float VistaRenderer::neighbour_hour(const SharedRenderState* render_state) const {
  const auto& gs = Gfx::g_global_settings;
  if (gs.vista_hour_override >= 0.f) {
    return gs.vista_hour_override;
  }
  if (gs.vista_tod_addr && render_state->ee_main_memory) {
    float h;
    memcpy(&h, (const u8*)render_state->ee_main_memory + gs.vista_tod_addr, sizeof(float));
    if (h >= 0.f && h < 48.f) {
      return h;
    }
  }
  return -1.f;
}

void VistaRenderer::render(SharedRenderState* render_state,
                           ScopedProfilerNode& prof,
                           const TfragPcPortData* cam0,
                           const TfragPcPortData* cam1) {
  const auto& gs = Gfx::g_global_settings;
  render_state->vista_drawn = false;
  m_drawn.clear();
  m_offscreen.clear();
  m_hiz_skipped = false;
  const TfragPcPortData* cam = cam0 ? cam0 : cam1;
  if (!cam) {
    m_status = "no camera packet this frame";
    return;
  }
  std::vector<std::string> viewer;
  for (auto* c : {cam0, cam1}) {
    if (c) {
      viewer.emplace_back(c->level_name);
    }
  }
  auto drawn_by_game = [&](const std::string& l) {
    return std::find(viewer.begin(), viewer.end(), l) != viewer.end();
  };

  if (gs.vista_neighbours) {
    const std::string who = neighbours::viewer();
    if (!neighbours::is_outdoor(who)) {
      render_state->vista_hide_filter.clear();
      render_state->vista_medres_hidden.clear();
      m_status = who.empty() ? "no viewer level from the game, neighbours off"
                             : fmt::format("{} is not an outdoor vista level, neighbours off", who);
      log_status(render_state, "VISTA: " + m_status);
      return;
    }
  }

  std::vector<std::string> todo;
  if (gs.vista_neighbours) {
    std::vector<std::string> game_levels, adj;
    neighbours::get_resident(&game_levels, &adj);
    const math::Vector3f cam_m(cam->camera.trans.x() / 4096.f, cam->camera.trans.y() / 4096.f,
                               cam->camera.trans.z() / 4096.f);
    for (auto* list : {&game_levels, &adj}) {
      for (const auto& l : *list) {
        if (!drawn_by_game(l) && neighbours::is_outdoor(l) && !neighbours::kept_out(l, cam_m)) {
          todo.push_back(l);
        }
      }
    }
  } else if (gs.vista_level[0] && !drawn_by_game(gs.vista_level)) {
    todo.emplace_back(gs.vista_level);
  }
  if (todo.empty()) {
    m_status = "nothing to draw: every level is drawn by the game";
    render_state->vista_medres_hidden.clear();
    return;
  }

  TfragRenderSettings base;
  base.camera = gs.vista_fog_mode == VISTA_FOG_OWN
                    ? vista_fog_camera(cam->camera, gs.vista_fog_end_m)
                    : cam->camera;
  base.tree_idx = 0;
  base.occlusion_culling = nullptr;

  const bool own_mood = gs.vista_neighbours && gs.vista_own_mood;
  m_hour = own_mood ? neighbour_hour(render_state) : -1.f;
  if (m_hour >= 0.f) {
    math::Vector<s32, 4> mine[4];
    m_mood_drift = m_mood.itimes_at(cam->level_name, m_hour, mine)
                       ? neighbours::itimes_diff(mine, cam->camera.itimes)
                       : -1;
  }

  if (gs.vista_gpu_timing) {
    m_timer.begin();
  }
  Timer cpu_timer;
  std::string waiting;
  const char* hide =
      gs.vista_neighbours && gs.vista_hide_neighbour_lowres ? kNeighbourStandins : nullptr;
  struct Pending {
    std::string level;
    TfragRenderSettings settings;
    float dist;
  };
  std::vector<Pending> pending;
  for (const auto& level : todo) {
    TfragRenderSettings settings = base;
    if (m_hour >= 0.f && !m_mood.itimes_at(level, m_hour, settings.camera.itimes)) {
      continue;  // no own mood: skip rather than draw with the viewer's weights
    }
    auto& r = renderers_for(level, render_state);
    r.tfrag->set_force_hide_filter(hide);
    if (!r.tfrag->setup_for_level(kVistaTfragKinds, level, render_state)) {
      waiting += " " + level;
      continue;
    }
    pending.push_back({level, settings, level_distance(level, render_state, cam->camera.trans)});
  }
  // the frame lights (ocean sun, merc light) are whatever background drew last: keep that the
  // level last in the unsorted, unskipped order, whatever the draw order
  m_lights_level = pending.empty() ? -1 : 0;
  m_unsorted.clear();
  for (const auto& p : pending) {
    m_unsorted.emplace_back(p.level, p.settings);
  }
  if (!pending.empty()) {
    m_lights_name = pending.back().level;
    m_lights_settings = pending.back().settings;
  }
  // nearest first for early-z; levels whose bounds overlap keep their order, as coincident
  // seam geometry at equal depth goes to the later level (GEQUAL)
  if (Gfx::perf_opt(PERF_OPT_VISTA + 0)) {
    std::vector<Pending> sorted;
    std::vector<bool> placed(pending.size(), false);
    while (sorted.size() < pending.size()) {
      int best = -1;
      for (size_t i = 0; i < pending.size(); i++) {
        bool ready = !placed[i];
        for (size_t j = 0; ready && j < i; j++) {
          ready = placed[j] || !levels_overlap(pending[i].level, pending[j].level);
        }
        if (ready && (best < 0 || pending[i].dist < pending[best].dist)) {
          best = (int)i;
        }
      }
      placed[best] = true;
      sorted.push_back(pending[best]);
    }
    pending = std::move(sorted);
  }
  // exact occlusion by the viewer levels' depth (one eye: the depth is this eye's)
  bool hiz = false;
  {
    perf::Scope pw(perf::Group::VISTA, "vista-part", "hiz-wait");
    hiz = gs.vista_hiz && gs.vista_neighbours && !pending.empty() &&
          (gs.vista_fog_mode == VISTA_FOG_MAIN || gs.vista_fog_mode == VISTA_FOG_UNIFIED) &&
          !render_state->stereo_active && m_hiz.finish(render_state->frame_idx);
  }
  if (hiz) {
    m_hiz.set_camera(base.camera);
    m_hiz.tested = m_hiz.culled = 0;
  }
  m_hiz.invert = hiz && (gs.vista_probe & 2);
  if (m_hiz.invert) {
    leak_query_begin();
  }
  GLint scissor_box[4];
  const GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
  if (gs.vista_probe & 1) {
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
  }
  render_state->vista_hiz = hiz ? &m_hiz : nullptr;
  u64 tris = 0, draws = 0;
  for (const auto& [level, settings, dist] : pending) {
    auto& r = renderers_for(level, render_state);
    // a level wholly outside the frustum or behind the depth: skip its per-tree setup, stay in
    // m_drawn for the hides
    const auto bit = m_bounds.find(level);
    const bool hidden = hiz && !m_hiz.invert && bit != m_bounds.end() && bit->second.load_id != UINT64_MAX &&
                        m_hiz.box_hidden(bit->second.lo, bit->second.hi);
    m_hiz_skipped |= hidden;
    if (hidden ||
        (Gfx::perf_opt(PERF_OPT_VISTA + 2) && !level_in_view(level, settings.camera.planes))) {
      m_drawn.emplace_back(level, settings);
      m_offscreen.push_back(level);
      continue;
    }
    ProfilerNode* node = prof.make_child(level);
    {
      ScopedProfilerNode p(node);
      perf::Scope ps(perf::Group::VISTA, "vista", level);
      render_state->vista_nb_fog = neighbour_fog(level);
      {
        perf::Scope pk(perf::Group::VISTA, "vista-part", "tfrag");
        r.tfrag->render_matching_trees(r.tfrag->lod(), kVistaTfragKinds, settings, render_state,
                                       p);
      }
      {
        perf::Scope pk(perf::Group::VISTA, "vista-part", "tie");
        r.tie->render_vista(level, hide, settings, render_state, p);
      }
      if (gs.vista_draw_shrub && r.shrub->setup_for_level(level, render_state)) {
        perf::Scope pk(perf::Group::VISTA, "vista-part", "shrub");
        r.shrub->render_all_trees(settings, render_state, p);
      }
      render_state->vista_nb_fog = nullptr;
    }
    tris += node->stats().triangles;
    draws += node->stats().draw_calls;
    m_drawn.emplace_back(level, settings);
  }
  render_state->vista_hiz = nullptr;
  if (m_hiz.invert) {
    glEndQuery(GL_SAMPLES_PASSED);
  }
  if (gs.vista_probe & 1) {
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    scissor_was ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
  }
  if (gs.vista_gpu_timing) {
    m_timer.end();
  }
  restore_frame_lights(render_state);
  if (perf::on() && hiz) {
    perf::count(perf::Counter::VISTA_HIZ_PCT,
                m_hiz.tested ? 100. * (double)m_hiz.culled / (double)m_hiz.tested : 0.);
    perf::count(perf::Counter::VISTA_HIZ_SYNC_MS, m_hiz.sync_ms);
  }
  if (perf::on() && !pending.empty()) {
    perf::count(perf::Counter::VISTA_KTRIS, (double)tris / 1000.);
    perf::count(perf::Counter::VISTA_DRAWS, (double)draws);
    perf::count(perf::Counter::VISTA_CPU_MS, cpu_timer.getMs());
  }

  render_state->vista_drawn = !m_drawn.empty();
  perf::count(perf::Counter::VISTA_LEVELS, (double)m_drawn.size());
  std::vector<std::string> drawn_names;
  for (const auto& d : m_drawn) {
    drawn_names.push_back(d.first);
  }
  if (gs.vista_neighbours) {
    render_state->vista_hide_filter = neighbours::standin_filter(viewer, drawn_names);
    if (gs.vista_hide_medres) {
      render_state->vista_medres_hidden = drawn_names;
    } else {
      render_state->vista_medres_hidden.clear();
    }
  } else {
    render_state->vista_medres_hidden.clear();
  }

  std::string names;
  for (const auto& n : drawn_names) {
    names += " " + n;
  }
  m_status = fmt::format("drawing{} from {}'s camera", names.empty() ? " nothing" : names,
                         cam->level_name);
  if (!waiting.empty()) {
    m_status += "; waiting for the loader:" + waiting;
  }
  const std::string log_line =
      fmt::format("VISTA: {} | hour {:.0f} viewer mood drift {}/64 | hide '{}'", m_status, m_hour,
                  m_mood_drift, render_state->vista_hide_filter);
  log_status(render_state, log_line);
}

VistaRenderer::~VistaRenderer() {
  if (m_leak_q[0]) {
    glDeleteQueries(4, m_leak_q);
  }
}

void VistaRenderer::leak_query_begin() {
  if (!m_leak_q[0]) {
    glGenQueries(4, m_leak_q);
  }
  for (int i = 0; i < 4; i++) {
    GLint ready = 0;
    if (m_leak_pending[i]) {
      glGetQueryObjectiv(m_leak_q[i], GL_QUERY_RESULT_AVAILABLE, &ready);
    }
    if (ready) {
      GLuint64 n = 0;
      glGetQueryObjectui64v(m_leak_q[i], GL_QUERY_RESULT, &n);
      m_leak_pending[i] = false;
      m_leak_max = std::max(m_leak_max, (double)n);
    }
  }
  perf::count(perf::Counter::VISTA_HIZ_LEAK, m_leak_max);
  m_leak_next = (m_leak_next + 1) % 4;
  glBeginQuery(GL_SAMPLES_PASSED, m_leak_q[m_leak_next]);
  m_leak_pending[m_leak_next] = true;
}

void VistaRenderer::prepare_hiz(SharedRenderState* render_state) {
  const auto& gs = Gfx::g_global_settings;
  m_hiz.clear();
  if (gs.vista_hiz && gs.vista_neighbours && !render_state->stereo_active &&
      (gs.vista_fog_mode == VISTA_FOG_MAIN || gs.vista_fog_mode == VISTA_FOG_UNIFIED) &&
      m_status.rfind("nothing", 0) != 0 && m_status.find("neighbours off") == std::string::npos) {
    m_hiz.begin(render_state);
  }
}

void VistaRenderer::log_status(SharedRenderState* render_state, const std::string& log_line) {
  if (log_line != m_last_log && render_state->frame_idx >= m_next_log_frame) {
    lg::info("{}", log_line);
    m_last_log = log_line;
    m_next_log_frame = render_state->frame_idx + 60;
  }
}

void VistaRenderer::render_alpha(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  const auto& gs = Gfx::g_global_settings;
  if (!gs.vista_neighbours || !gs.vista_draw_alpha || m_drawn.empty()) {
    return;
  }
  if (gs.vista_gpu_timing) {
    m_alpha_timer.begin();
  }
  render_state->vista_hiz = m_hiz.valid() ? &m_hiz : nullptr;
  GLint scissor_box[4];
  const GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
  if (gs.vista_probe & 1) {
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
  }
  // blended trees: always the unsorted order, whatever order the opaque pass used
  for (const auto& [level, settings] : m_unsorted) {
    if (std::find(m_offscreen.begin(), m_offscreen.end(), level) != m_offscreen.end()) {
      continue;
    }
    auto& r = renderers_for(level, render_state);
    if (r.alpha->setup_for_level(kVistaAlphaKinds, level, render_state)) {
      auto p = prof.make_scoped_child(level);
      perf::Scope ps(perf::Group::VISTA_ALPHA, "vista-alpha", level);
      render_state->vista_nb_fog = neighbour_fog(level);
      r.alpha->render_matching_trees(r.alpha->lod(), kVistaAlphaKinds, settings, render_state, p);
      render_state->vista_nb_fog = nullptr;
    }
  }
  render_state->vista_hiz = nullptr;
  if (gs.vista_probe & 1) {
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    scissor_was ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
  }
  if (gs.vista_gpu_timing) {
    m_alpha_timer.end();
  }
  // unsorted, the alpha pass leaves the lights of the last level with an alpha tree
  for (auto it = m_unsorted.rbegin(); it != m_unsorted.rend(); ++it) {
    const LevelData* data = render_state->loader->get_tfrag3_level(it->first);
    if (!data || !data->level) {
      continue;
    }
    bool has_alpha = false;
    for (const auto& t : data->level->tfrag_trees[renderers_for(it->first, render_state).alpha->lod()]) {
      has_alpha |= std::find(kVistaAlphaKinds.begin(), kVistaAlphaKinds.end(), t.kind) !=
                   kVistaAlphaKinds.end();
    }
    if (has_alpha) {
      m_lights_name = it->first;
      m_lights_settings = it->second;
      break;
    }
  }
  restore_frame_lights(render_state);
}

void VistaRenderer::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::Checkbox("Enable", &gs.vista_enable);
  ImGui::SameLine();
  ImGui::Checkbox("Neighbours", &gs.vista_neighbours);
  ImGui::InputText("Level", gs.vista_level, sizeof(gs.vista_level));
  ImGui::Checkbox("Hide stand-ins", &gs.vista_hide_standins);
  ImGui::InputText("Stand-in filter", gs.vista_hide, sizeof(gs.vista_hide));
  ImGui::Checkbox("Own mood", &gs.vista_own_mood);
  ImGui::SameLine();
  ImGui::Checkbox("Alpha trees", &gs.vista_draw_alpha);
  ImGui::Checkbox("Hide med-res actors", &gs.vista_hide_medres);
  ImGui::SameLine();
  ImGui::Checkbox("Hide neighbours' stand-ins", &gs.vista_hide_neighbour_lowres);
  ImGui::Checkbox("Landmark actors", &gs.vista_landmarks);
  ImGui::SameLine();
  ImGui::Checkbox("Stand-in neighbours", &gs.vista_standin_neighbours);
  ImGui::SliderFloat("Hour override", &gs.vista_hour_override, -1.f, 24.f);
  ImGui::RadioButton("Own far fog", &gs.vista_fog_mode, VISTA_FOG_OWN);
  ImGui::SameLine();
  ImGui::RadioButton("Main level fog", &gs.vista_fog_mode, VISTA_FOG_MAIN);
  ImGui::RadioButton("Push out", &gs.vista_fog_mode, VISTA_FOG_PUSH_OUT);
  ImGui::SameLine();
  ImGui::RadioButton("Unified", &gs.vista_fog_mode, VISTA_FOG_UNIFIED);
  ImGui::SliderFloat("Fog end (m)", &gs.vista_fog_end_m, 500.f, 10000.f);
  ImGui::SliderFloat("Unified start (m)", &gs.vista_atmo_start_m, 0.f, 1000.f);
  ImGui::SliderFloat("Unified density (1/km)", &gs.vista_atmo_density_per_km, 0.f, 3.f);
  ImGui::SliderFloat("Unified scale height (m)", &gs.vista_atmo_height_m, 0.f, 2000.f);
  ImGui::Checkbox("Unified at least the game fog", &gs.vista_fog_floor);
  ImGui::SliderFloat("Haze density (1/km)", &gs.vista_haze_density_per_km, 0.f, 3.f);
  ImGui::SliderFloat("Haze start (m)", &gs.vista_haze_start_m, 0.f, 1000.f);
  ImGui::SliderFloat("Haze max", &gs.vista_haze_max, 0.f, 1.f);
  ImGui::SliderFloat3("Haze tint", gs.vista_haze_tint, 0.f, 1.99f);
  for (int i = 0; i < gs.vista_nb_fog_count; i++) {
    auto& f = gs.vista_nb_fog[i];
    ImGui::PushID(i);
    ImGui::Text("%s fog", f.level);
    ImGui::SliderFloat("density x", &f.density_scale, 0.f, 20.f);
    ImGui::ColorEdit3("fog tint", f.tint);
    ImGui::SliderFloat("max fog", &f.max_fog, -1.f, 1.f);
    ImGui::SliderFloat("haze x", &f.haze_scale, 0.f, 4.f);
    ImGui::PopID();
  }
  ImGui::Checkbox("Shrub", &gs.vista_draw_shrub);
  ImGui::Checkbox("GPU timing", &gs.vista_gpu_timing);
  ImGui::Text("%s", m_status.c_str());
  ImGui::Text("mood hour %.2f, viewer drift %d/64", m_hour, m_mood_drift);
  if (m_timer.last_avg_ms() >= 0) {
    ImGui::Text("GPU opaque %.3f ms, alpha %.3f ms", m_timer.last_avg_ms(),
                m_alpha_timer.last_avg_ms());
  }
}
