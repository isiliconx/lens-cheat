// panel.h - the corner readout. Frame stats, entity count, field health, and
// the resolution report are all on screen, because the first question anyone
// asks a new adapter is "is it even reading anything".
#pragma once
#include "config/settings.h"
#include "features/esp.h"
#include "overlay/drawlist.h"
#include "runtime/runtime.h"

namespace lens {

class Panel {
 public:
  void render(Runtime& rt, const PanelSettings& cfg, const EspStats& esp, DrawList& dl);
};

}  // namespace lens
