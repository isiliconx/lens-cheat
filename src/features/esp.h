// esp.h - the drawing feature. Every option is a settings field, every field
// is read from the descriptor, nothing about the engine is known here.
#pragma once
#include "config/settings.h"
#include "overlay/drawlist.h"
#include "runtime/runtime.h"

namespace lens {

struct EspStats {
  int drawn = 0;
  int skipped_dead = 0;
  int skipped_team = 0;
  int offscreen = 0;
  int unresolved_fields = 0;
};

class Esp {
 public:
  // Returns the stats for this frame; the DrawList is filled in place.
  EspStats render(Runtime& rt, const EspSettings& cfg, DrawList& dl, int screen_w, int screen_h);
};

}  // namespace lens
