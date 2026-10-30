#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/opengl_renderer/loader/common.h"
#include "game/graphics/texture/TexturePool.h"

class Loader {
 public:
  static constexpr float TIE_LOAD_BUDGET = 1.5f;
  static constexpr float SHARED_TEXTURE_LOAD_BUDGET = 3.f;
  Loader(const fs::path& base_path, int max_levels);
  ~Loader();
  void update(TexturePool& tex_pool);
  void update_blocking(TexturePool& tex_pool);
  /// Levels that finished initializing so far (render thread).
  int levels_loaded() const { return m_levels_loaded; }
  const LevelData* get_tfrag3_level(const std::string& level_name);
  /// Name a loaded level is registered under, empty if not loaded (inspect tool).
  std::string name_of(const LevelData* level);
  std::optional<MercRef> get_merc_model(const char* model_name);
  const tfrag3::Level& load_common(TexturePool& tex_pool, const std::string& name);
  void set_want_levels(const std::vector<std::string>& levels);
  void set_active_levels(const std::vector<std::string>& levels);
  /// Number of loaded levels at which the loader starts evicting idle ones.
  void set_max_levels(int n) { m_max_levels = n; }
  int default_max_levels() const { return m_default_max_levels; }
  std::vector<LevelData*> get_in_use_levels();
  void draw_debug_window();
  void debug_print_loaded_levels();
  void request_reload_all() { m_want_reload = true; }
  void request_reload_level(const std::string& name) { m_single_level_to_reload = name; }
  void request_reload_common() { m_want_reload_common = true; }
  /// Read `<level>.unbake` next to the fr3 at load (Jak 1 moving sun).
  void set_load_unbake(bool load) { m_load_unbake = load; }
  /// Material classes for the common level's textures (Jak 1, see Materials.h).
  void classify_common();

 private:
  void loader_thread();
  bool upload_textures(Timer& timer, LevelData& data, TexturePool& texture_pool);

  const std::string* get_most_unloadable_level();
  void unload_level_data(const std::string& name, LevelData& lev, TexturePool& tex_pool);
  void do_reload(TexturePool& tex_pool);
  void do_reload_common(TexturePool& tex_pool);
  void do_reload_level(const std::string& name, TexturePool& tex_pool);

  // used by game and loader thread
  std::unordered_map<std::string, std::unique_ptr<LevelData>> m_initializing_tfrag3_levels;

  LevelData m_common_level;

  std::string m_level_to_load;

  std::thread m_loader_thread;
  std::mutex m_loader_mutex;
  std::condition_variable m_loader_cv;
  std::condition_variable m_file_load_done_cv;
  bool m_want_shutdown = false;
  std::atomic<bool> m_want_reload{false};
  std::atomic<bool> m_want_reload_common{false};
  std::string m_single_level_to_reload;
  std::string m_selected_level_for_reload;
  uint64_t m_id = 0;

  // used only by game thread
  std::unordered_map<std::string, std::unique_ptr<LevelData>> m_loaded_tfrag3_levels;

  std::unordered_map<std::string, std::vector<MercRef>> m_all_merc_models;

  std::vector<std::string> m_desired_levels;
  std::vector<std::string> m_active_levels;
  std::vector<std::unique_ptr<LoaderStage>> m_loader_stages;
  std::vector<GLuint> m_garbage_textures;
  std::vector<GLuint> m_garbage_buffers;
  int m_levels_loaded = 0;

  fs::path m_base_path;
  std::atomic<bool> m_load_unbake{false};
  std::atomic<int> m_max_levels = 0;
  int m_default_max_levels = 0;
};
