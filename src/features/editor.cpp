#include "editor.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace lens {
namespace {

struct Row {
  int entity;
  std::string field;
  std::string value;
  std::string type;
  bool is_local = false;
};

// Build the flat row list once; both the renderer and the selection walk use it.
std::vector<Row> build_rows(Runtime& rt) {
  std::vector<Row> rows;
  for (const Entity& e : rt.entities()) {
    for (const auto& kv : e.fields) {
      Row r;
      r.entity = e.index;
      r.field = kv.first;
      r.value = kv.second.to_string();
      r.type = kv.second.t == Value::T::kInt ? "int" : (kv.second.t == Value::T::kFloat ? "flt" : "val");
      r.is_local = e.is_local;
      rows.push_back(std::move(r));
    }
  }
  return rows;
}

std::string fmt(const char* f, ...) {
  char b[160];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof(b), f, ap);
  va_end(ap);
  return b;
}

}  // namespace

void Editor::render(Runtime& rt, const EditorSettings& cfg, DrawList& dl, int sw, int sh,
                    int scroll) {
  if (!cfg.enabled) return;

  const std::vector<Row> rows = build_rows(rt);
  const int x = 200, y = 40;
  const int w = sw - x - 20;
  const int row_h = 20;
  const int visible = std::max(4, (sh - y - 60) / row_h);

  dl.rect(Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
               static_cast<float>(row_h * (visible + 1) + 28)}, Color(12, 14, 18, 225));
  dl.text(x + 8, y + 6, fmt("inspector: %zu fields live%s", rows.size(),
                            cfg.write_enabled ? "  [WRITE ARMED]" : "  [read-only]"),
          cfg.write_enabled ? col::kOrange : col::kGray, 16.f);

  char hdr[96];
  std::snprintf(hdr, sizeof(hdr), "%-5s %-16s %-14s", "ent", "field", "value");
  dl.text(x + 8, y + 22, hdr, col::kCyan, 16.f);
  dl.line(x, y + row_h + 18, x + w, y + row_h + 18, col::kGray, 1.f);

  const int start = std::max(0, scroll);
  for (int i = 0; i < visible; ++i) {
    const int idx = start + i;
    if (idx >= static_cast<int>(rows.size())) break;
    const Row& r = rows[idx];
    const bool sel = (cfg.row == idx);
    const float ry = y + row_h * 2 + i * row_h;
    if (sel) dl.rect(Rect{static_cast<float>(x + 2), ry - 2, static_cast<float>(w - 4),
                          static_cast<float>(row_h)}, Color(40, 60, 90, 200));
    const Color c = r.is_local ? col::kCyan : col::kWhite;
    char line[160];
    std::snprintf(line, sizeof(line), "%-5d %-16s %-14s", r.entity, r.field.c_str(),
                  r.value.c_str());
    dl.text(x + 8, ry, line, c, 16.f);
  }

  if (rows.empty()) {
    dl.text(x + 8, y + row_h * 2, "no fields resolved - run `lens resolve` to see why",
            col::kRed, 11.f);
  } else {
    dl.text(x + 8, sh - 40,
            fmt("up/down move   enter edit   type a value   enter commits   esc cancels"),
            col::kGray, 16.f);
  }
}

Editor::Selection Editor::selection(const Runtime& rt, const EditorSettings& cfg,
                                    int scroll) const {
  (void)rt;
  Selection s;
  const int idx = cfg.row;
  (void)scroll;
  if (idx < 0) return s;
  s.entity = idx;  // resolved to a real field by the caller
  return s;
}

bool Editor::set_value(Runtime& rt, EditorSettings& cfg, const std::string& text,
                       std::string* err) {
  if (!cfg.write_enabled) {
    if (err) *err = "editor is read-only: start lens with --write to arm it";
    return false;
  }
  // The selected row names a field; find the entity index and the field on it.
  const std::vector<Entity>& ents = rt.entities();
  if (cfg.selected < 0 || cfg.selected >= static_cast<int>(ents.size())) {
    if (err) *err = "no entity selected";
    return false;
  }
  // Walk the selected entity's fields in order to find the one at cfg.row.
  const Entity& e = ents[cfg.selected];
  if (cfg.row < 0 || cfg.row >= static_cast<int>(e.fields.size())) {
    if (err) *err = "row out of range for this entity";
    return false;
  }
  const std::string field = e.fields[cfg.row].first;

  Entity mutable_copy = e;
  Value v;
  const Value& cur = e.fields[cfg.row].second;
  v.t = cur.t;
  if (cur.t == Value::T::kStr) {
    v.s = text;
  } else if (cur.t == Value::T::kFloat) {
    v.f = static_cast<float>(std::atof(text.c_str()));
  } else if (cur.t == Value::T::kInt || cur.t == Value::T::kPtr) {
    v.i = static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 0));
    v.p = static_cast<uintptr_t>(v.i);
  } else {
    if (err) *err = "field type is not editable";
    return false;
  }
  return rt.write_field(mutable_copy, field, v, err);
}

}  // namespace lens
