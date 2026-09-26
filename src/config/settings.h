// settings.h - one config struct, one file, hotkeys, per-feature blocks.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lens {

struct EspSettings {
  bool enabled = true;
  bool box = true;
  int box_style = 0;        // 0 = 2D rect, 1 = cornered, 2 = 3D box
  bool box_3d = false;
  bool skeleton = false;
  bool health_bar = true;
  bool health_text = true;
  bool name = true;
  bool distance = true;
  bool weapon = false;
  bool team_check = true;
  int local_team = 0;       // 0 = infer from the local entity
  bool hide_friends = true;
  bool hide_dead = true;
  bool hide_local = true;
  bool offscreen_indicator = true;
  bool aimed_highlight = false;
  float max_distance = 0.f;  // 0 = unlimited
};

struct PanelSettings {
  bool enabled = true;
  bool show_resolution = true;
  bool show_entities = true;
  bool show_health = true;
  bool show_fps = true;
  int x = 12, y = 12;
};

struct EditorSettings {
  bool enabled = false;
  int selected = -1;
  int row = 0;
  bool write_enabled = false;  // requires --write on the command line
};

struct Settings {
  EspSettings esp;
  PanelSettings panel;
  EditorSettings editor;

  std::string descriptor_path;
  std::string transport;
  std::string process;
  bool allow_write = false;
  uint32_t viewport_w = 1280, viewport_h = 720;
  float refresh_hz = 60.f;
  std::string dump_path;      // one-frame PPM dump path
  std::string svg_path;       // one-frame SVG dump path
  std::string snapshot_path = "snapshots/latest.json";

  std::map<std::string, int> keys;  // action -> virtual key code

  static Settings load(const std::string& path);   // missing file = defaults
  void save(const std::string& path) const;
  int key(const std::string& action, int def) const;
  void set_key(const std::string& action, int vk);
};

}  // namespace lens
