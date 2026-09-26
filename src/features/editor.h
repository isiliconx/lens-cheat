// editor.h - the inspector table, and the value editor.
//
// This is the half of lens that reads like a debugger: a live table of every
// entity and every declared field, and a writer that can push a new value back
// into the target. The writer refuses unless the transport was opened with
// --write and the field is marked writable in the descriptor, so a read-only run
// cannot modify the process even if the UI asks it to.
#pragma once
#include <string>
#include <vector>

#include "config/settings.h"
#include "overlay/drawlist.h"
#include "runtime/runtime.h"

namespace lens {

class Editor {
 public:
  // Draws the table. scroll is in rows.
  void render(Runtime& rt, const EditorSettings& cfg, DrawList& dl, int sw, int sh, int scroll);

  // What the cursor is currently pointing at, for the key handler.
  struct Selection {
    int entity = -1;
    std::string field;
    bool valid = false;
  };
  Selection selection(const Runtime& rt, const EditorSettings& cfg, int scroll) const;

  // Commit a new value. Returns false and sets err on refusal.
  bool set_value(Runtime& rt, EditorSettings& cfg, const std::string& text, std::string* err);
};

}  // namespace lens
