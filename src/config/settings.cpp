#include "settings.h"

#include <fstream>
#include <sstream>

#include "core/log.h"
#include "core/yaml.h"

namespace lens {
namespace {

Settings apply(const Settings& s, const YNode& root) {
  Settings o = s;
  if (const YNode* t = root.find("target")) {
    o.descriptor_path = t->str("descriptor", o.descriptor_path);
    o.transport = t->str("transport", o.transport);
    o.process = t->str("process", o.process);
  }
  if (const YNode* v = root.find("viewport")) {
    o.viewport_w = static_cast<uint32_t>(v->i64("w", o.viewport_w));
    o.viewport_h = static_cast<uint32_t>(v->i64("h", o.viewport_h));
    o.refresh_hz = static_cast<float>(v->f64("hz", o.refresh_hz));
  }
  if (const YNode* d = root.find("output")) {
    o.dump_path = d->str("dump_ppm", o.dump_path);
    o.svg_path = d->str("dump_svg", o.svg_path);
    o.snapshot_path = d->str("snapshot", o.snapshot_path);
  }
  if (const YNode* e = root.find("esp")) {
    EspSettings& g = o.esp;
    g.enabled = e->boolean("enabled", g.enabled);
    g.box = e->boolean("box", g.box);
    g.box_style = static_cast<int>(e->i64("box_style", g.box_style));
    g.box_3d = e->boolean("box_3d", g.box_3d);
    g.skeleton = e->boolean("skeleton", g.skeleton);
    g.health_bar = e->boolean("health_bar", g.health_bar);
    g.health_text = e->boolean("health_text", g.health_text);
    g.name = e->boolean("name", g.name);
    g.distance = e->boolean("distance", g.distance);
    g.weapon = e->boolean("weapon", g.weapon);
    g.team_check = e->boolean("team_check", g.team_check);
    g.local_team = static_cast<int>(e->i64("local_team", g.local_team));
    g.hide_friends = e->boolean("hide_friends", g.hide_friends);
    g.hide_dead = e->boolean("hide_dead", g.hide_dead);
    g.hide_local = e->boolean("hide_local", g.hide_local);
    g.offscreen_indicator = e->boolean("offscreen_indicator", g.offscreen_indicator);
    g.aimed_highlight = e->boolean("aimed_highlight", g.aimed_highlight);
    g.max_distance = static_cast<float>(e->f64("max_distance", g.max_distance));
  }
  if (const YNode* p = root.find("panel")) {
    o.panel.enabled = p->boolean("enabled", o.panel.enabled);
    o.panel.show_resolution = p->boolean("show_resolution", o.panel.show_resolution);
    o.panel.show_entities = p->boolean("show_entities", o.panel.show_entities);
    o.panel.show_health = p->boolean("show_health", o.panel.show_health);
    o.panel.show_fps = p->boolean("show_fps", o.panel.show_fps);
    o.panel.x = static_cast<int>(p->i64("x", o.panel.x));
    o.panel.y = static_cast<int>(p->i64("y", o.panel.y));
  }
  if (const YNode* ed = root.find("editor")) {
    o.editor.enabled = ed->boolean("enabled", o.editor.enabled);
    o.editor.write_enabled = ed->boolean("write_enabled", o.editor.write_enabled);
  }
  if (const YNode* k = root.find("keys")) {
    for (const auto& kv : k->map) o.keys[kv.first] = static_cast<int>(std::strtol(kv.second.scalar.c_str(), nullptr, 0));
  }
  if (const YNode* w = root.find("write")) {
    o.allow_write = w->boolean("allow", o.allow_write);
  }
  return o;
}

}  // namespace

Settings Settings::load(const std::string& path) {
  Settings s;
  if (path.empty()) return s;
  std::ifstream f(path, std::ios::binary);
  if (!f) return s;
  std::stringstream ss;
  ss << f.rdbuf();
  YNode root;
  std::string err;
  if (!YNode::try_parse(ss.str(), root, &err)) {
    LERROR("config %s: %s - using defaults", path.c_str(), err.c_str());
    return s;
  }
  return apply(s, root);
}

void Settings::save(const std::string& path) const {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    LERROR("cannot write config %s", path.c_str());
    return;
  }
  f << "# lens configuration\n";
  f << "target:\n  descriptor: " << descriptor_path << "\n  transport: " << transport
    << "\n  process: " << process << "\n";
  f << "viewport:\n  w: " << viewport_w << "\n  h: " << viewport_h << "\n  hz: " << refresh_hz
    << "\n";
  f << "output:\n  dump_ppm: " << dump_path << "\n  dump_svg: " << svg_path
    << "\n  snapshot: " << snapshot_path << "\n";
  f << "write:\n  allow: " << (allow_write ? "true" : "false") << "\n";
  f << "esp:\n";
  f << "  enabled: " << (esp.enabled ? "true" : "false") << "\n";
  f << "  box: " << (esp.box ? "true" : "false") << "\n";
  f << "  box_style: " << esp.box_style << "\n";
  f << "  box_3d: " << (esp.box_3d ? "true" : "false") << "\n";
  f << "  skeleton: " << (esp.skeleton ? "true" : "false") << "\n";
  f << "  health_bar: " << (esp.health_bar ? "true" : "false") << "\n";
  f << "  health_text: " << (esp.health_text ? "true" : "false") << "\n";
  f << "  name: " << (esp.name ? "true" : "false") << "\n";
  f << "  distance: " << (esp.distance ? "true" : "false") << "\n";
  f << "  weapon: " << (esp.weapon ? "true" : "false") << "\n";
  f << "  team_check: " << (esp.team_check ? "true" : "false") << "\n";
  f << "  local_team: " << esp.local_team << "\n";
  f << "  hide_friends: " << (esp.hide_friends ? "true" : "false") << "\n";
  f << "  hide_dead: " << (esp.hide_dead ? "true" : "false") << "\n";
  f << "  hide_local: " << (esp.hide_local ? "true" : "false") << "\n";
  f << "  offscreen_indicator: " << (esp.offscreen_indicator ? "true" : "false") << "\n";
  f << "  aimed_highlight: " << (esp.aimed_highlight ? "true" : "false") << "\n";
  f << "  max_distance: " << esp.max_distance << "\n";
  f << "panel:\n";
  f << "  enabled: " << (panel.enabled ? "true" : "false") << "\n";
  f << "  show_resolution: " << (panel.show_resolution ? "true" : "false") << "\n";
  f << "  show_entities: " << (panel.show_entities ? "true" : "false") << "\n";
  f << "  show_health: " << (panel.show_health ? "true" : "false") << "\n";
  f << "  show_fps: " << (panel.show_fps ? "true" : "false") << "\n";
  f << "  x: " << panel.x << "\n  y: " << panel.y << "\n";
  f << "editor:\n  enabled: " << (editor.enabled ? "true" : "false")
    << "\n  write_enabled: " << (editor.write_enabled ? "true" : "false") << "\n";
  f << "keys:\n";
  for (const auto& kv : keys) f << "  " << kv.first << ": " << kv.second << "\n";
}

int Settings::key(const std::string& action, int def) const {
  auto it = keys.find(action);
  return it == keys.end() ? def : it->second;
}

void Settings::set_key(const std::string& action, int vk) { keys[action] = vk; }

}  // namespace lens
