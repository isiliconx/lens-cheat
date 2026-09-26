#include "panel.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace lens {
namespace {
std::string fmt(const char* f, ...) {
  char b[128];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof(b), f, ap);
  va_end(ap);
  return b;
}
}  // namespace

void Panel::render(Runtime& rt, const PanelSettings& cfg, const EspStats& esp, DrawList& dl) {
  if (!cfg.enabled) return;
  const FrameStats& s = rt.stats();
  const Resolution& r = rt.resolution();

  int y = cfg.y;
  const int x = cfg.x;
  auto line = [&](const std::string& text, Color c) {
    dl.text(x, y, text, c, 16.f);
    y += 19;
  };

  const char* mod = r.modules.empty() ? "?" : r.modules.front().name.c_str();
  uintptr_t base = r.modules.empty() ? 0 : r.modules.front().base;

  if (cfg.show_resolution) {
    const Health h = r.modules.empty() ? Health::kDead : r.modules.front().health;
    const Color c = h == Health::kOk ? col::kGreen : (h == Health::kStale ? col::kYellow : col::kRed);
    line(fmt("lens  %s  @0x%llx", mod, static_cast<unsigned long long>(base)), c);
  }
  if (cfg.show_entities) {
    Color c = s.entity_count > 0 ? col::kWhite : col::kRed;
    if (s.entity_count == 0) line("no entities resolved - check the descriptor", c);
    line(fmt("entities  %d   drawn %d   offscreen %d", s.entity_count, esp.drawn, esp.offscreen), c);
  }
  if (cfg.show_health) {
    if (s.dead_fields > 0)
      line(fmt("fields  %d dead  %d stale", s.dead_fields, s.stale_fields), col::kRed);
    else if (s.stale_fields > 0)
      line(fmt("fields  %d stale", s.stale_fields), col::kYellow);
    else
      line(fmt("fields  all %d ok", r.total_fields()), col::kGreen);
  }
  if (cfg.show_fps) line(fmt("read %.2f ms   tick %u", s.read_ms, s.frame), col::kGray);

  // The failures list is the single most useful thing on screen when an adapter
  // breaks: it names the field, not the file.
  const auto fails = r.failures();
  for (size_t i = 0; i < fails.size() && i < 6; ++i)
    line("! " + fails[i], col::kOrange);
}

}  // namespace lens
