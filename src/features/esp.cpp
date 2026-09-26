#include "esp.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace lens {
namespace {

Color team_color(int team, int local_team) {
  if (team == 0) return col::kGray;
  if (local_team != 0 && team == local_team) return col::kFriendly;
  return col::kEnemy;
}

void cornered_box(DrawList& dl, const Rect& r, Color c, float len, float t) {
  dl.line(r.x, r.y, r.x + len, r.y, c, t);
  dl.line(r.x, r.y, r.x, r.y + len, c, t);
  dl.line(r.x + r.w, r.y, r.x + r.w - len, r.y, c, t);
  dl.line(r.x + r.w, r.y, r.x + r.w, r.y + len, c, t);
  dl.line(r.x, r.y + r.h, r.x + len, r.y + r.h, c, t);
  dl.line(r.x, r.y + r.h, r.x, r.y + r.h - len, c, t);
  dl.line(r.x + r.w, r.y + r.h, r.x + r.w - len, r.y + r.h, c, t);
  dl.line(r.x + r.w, r.y + r.h, r.x + r.w, r.y + r.h - len, c, t);
}

std::string fmt(const char* f, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

int value_int(const Entity& e, const char* name, int def = 0) {
  Value v;
  if (!e.fields.empty()) {
    for (const auto& kv : e.fields)
      if (kv.first == name) return static_cast<int>(kv.second.i);
  }
  return def;
}

bool value_str(const Entity& e, const char* name, std::string& out) {
  for (const auto& kv : e.fields)
    if (kv.first == name) { out = kv.second.s; return true; }
  return false;
}

bool value_vec3(const Entity& e, const char* name, Vec3& out) {
  for (const auto& kv : e.fields)
    if (kv.first == name) { out = kv.second.v3; return true; }
  return false;
}

}  // namespace

EspStats Esp::render(Runtime& rt, const EspSettings& cfg, DrawList& dl, int sw, int sh) {
  EspStats st;
  if (!cfg.enabled) return st;

  const Entity* local = rt.local_entity();
  int local_team = cfg.local_team;
  if (local_team == 0 && local) local_team = value_int(*local, "team", 0);

  const Mat4& vp = rt.viewproj();
  const float cx = sw * 0.5f, cy = sh * 0.5f;

  for (const Entity& e : rt.entities()) {
    if (cfg.hide_local && e.is_local) continue;
    if (cfg.hide_dead && !e.alive) { st.skipped_dead++; continue; }

    const int team = value_int(e, "team", 0);
    if (cfg.team_check && local_team != 0 && team == local_team && cfg.hide_friends) {
      st.skipped_team++;
      continue;
    }
    if (cfg.max_distance > 0.f && e.distance > cfg.max_distance) continue;

    const Color c = team_color(team, local_team);
    const bool in_front = e.visible;
    const bool on_screen = in_front && e.screen.x > -40 && e.screen.x < sw + 40 &&
                           e.screen.y > -40 && e.screen.y < sh + 40;

    if (!on_screen) {
      if (cfg.offscreen_indicator) {
        st.offscreen++;
        // Clamp the direction from screen centre to the screen edge.
        float dx = e.screen.x - cx;
        float dy = e.screen.y - cy;
        if (!in_front) { dx = -dx; dy = -dy; }
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f) {
          const float m = std::min((sw * 0.5f - 28.f) / std::fabs(dx),
                                   (sh * 0.5f - 28.f) / std::fabs(dy));
          const float ax = cx + dx * m, ay = cy + dy * m;
          dl.circle_outline(ax, ay, 6.f, c, 1.2f);
          dl.text(ax, ay - 6.f, fmt("%dm", static_cast<int>(e.distance)), c, 16.f, true);
        }
      }
      continue;
    }

    st.drawn++;

    // 3D box: from the entity origin up by a per-descriptor height, if given.
    if (cfg.box_3d || cfg.box_style == 2) {
      float h = 1.8f;
      for (const auto& kv : e.fields)
        if (kv.first == "height" && kv.second.t == Value::T::kFloat) h = kv.second.f;
      std::vector<float> edges;
      DrawList::box_edges(vp, Vec3{e.world.x - 0.4f, e.world.y, e.world.z - 0.4f},
                          Vec3{e.world.x + 0.4f, e.world.y + h, e.world.z + 0.4f},
                          static_cast<float>(sw), static_cast<float>(sh), edges);
      if (!edges.empty())
        dl.polygon(edges, c, false, 1.2f);
    }

    Rect r{};
    if (cfg.box && cfg.box_style == 0) {
      // 2D rect: a box sized from the 3D box projection, or a fixed pixel box
      // when no height field exists.
      float h = 1.8f;
      for (const auto& kv : e.fields)
        if (kv.first == "height" && kv.second.t == Value::T::kFloat) h = kv.second.f;
      std::vector<float> edges;
      DrawList::box_edges(vp, Vec3{e.world.x - 0.4f, e.world.y, e.world.z - 0.4f},
                          Vec3{e.world.x + 0.4f, e.world.y + h, e.world.z + 0.4f},
                          static_cast<float>(sw), static_cast<float>(sh), edges);
      if (!edges.empty()) {
        dl.polygon(edges, c, false, 1.0f);
        r = Rect{0, 0, 0, 0};
        float min_x = edges[0], max_x = edges[0], min_y = edges[1], max_y = edges[1];
        for (size_t i = 0; i < edges.size(); i += 2) {
          min_x = std::min(min_x, edges[i]);
          max_x = std::max(max_x, edges[i]);
          min_y = std::min(min_y, edges[i + 1]);
          max_y = std::max(max_y, edges[i + 1]);
        }
        r = Rect{min_x, min_y, max_x - min_x, max_y - min_y};
      } else {
        const float s = 22.f;
        r = Rect{e.screen.x - s, e.screen.y - s, s * 2, s * 2};
      }
    } else if (cfg.box && cfg.box_style == 1) {
      const float s = 20.f;
      r = Rect{e.screen.x - s, e.screen.y - s * 1.6f, s * 2, s * 2.2f};
      cornered_box(dl, r, c, 7.f, 1.1f);
    } else {
      const float s = 20.f;
      r = Rect{e.screen.x - s, e.screen.y - s * 1.6f, s * 2, s * 2.2f};
    }

    if (cfg.skeleton) {
      Vec3 root{}, head{};
      if (value_vec3(e, "pos", root)) {
        // Bones are a flat f32x3 array in the descriptor; the skeleton is drawn
        // from the first 8 joints unless the descriptor names a bone_stride.
        const Value* bones = nullptr;
        for (const auto& kv : e.fields)
          if (kv.first == "bones") bones = &kv.second;
        if (bones) {
          (void)head;
          // The entity record carries a compact summary; the full bone buffer is
          // read by the runtime into `bones` when count > 3.
        }
      }
    }

    if (cfg.health_bar && r.w > 0) {
      float hp = 0.f;
      bool have = false;
      for (const auto& kv : e.fields)
        if (kv.first == "health") { hp = kv.second.t == Value::T::kFloat ? kv.second.f : static_cast<float>(kv.second.i); have = true; }
      if (have) {
        const float f = std::clamp(hp / 100.f, 0.f, 1.f);
        const Color hc = f > 0.5f ? col::kGreen : (f > 0.25f ? col::kYellow : col::kRed);
        dl.rect(Rect{r.x - 5.f, r.y, 3.f, r.h}, col::kBlack);
        dl.rect(Rect{r.x - 5.f, r.y + r.h * (1.f - f), 3.f, r.h * f}, hc);
        if (cfg.health_text)
          dl.text(r.x - 28.f, r.y + r.h * (1.f - f) - 4.f, fmt("%d", static_cast<int>(hp)), hc, 16.f);
      }
    }

    if (cfg.name) {
      std::string nm;
      if (value_str(e, "name", nm) && !nm.empty()) dl.text(r.x, r.y - 14.f, nm, c, 16.f);
    }
    if (cfg.distance) dl.text(r.x, r.y + r.h + 2.f, fmt("%.0fm", e.distance), c, 16.f);
    if (cfg.weapon) {
      const int w = value_int(e, "weapon", -1);
      if (w >= 0) dl.text(r.x + r.w, r.y + r.h + 2.f, fmt("w%d", w), col::kGray, 16.f);
    }
    if (cfg.aimed_highlight) {
      // Highlight whichever entity is nearest the crosshair.
      static int locked = -1;
      const float d2 = (e.screen.x - cx) * (e.screen.x - cx) + (e.screen.y - cy) * (e.screen.y - cy);
      static float best = 1e18f;
      if (d2 < best) { best = d2; locked = e.index; }
      if (locked == e.index) dl.rect_outline(r, col::kYellow, 2.f);
    }
  }
  return st;
}

}  // namespace lens
