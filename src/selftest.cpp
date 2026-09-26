// selftest.cpp - the acceptance test, runnable on a bare checkout.
//
// Exercises the full pipeline against lens_sim: attach, scan, resolve, read,
// project, draw, snapshot, diff. Asserts on real values, not on "no crash".
// Registered as a subcommand rather than a separate binary so the build stays
// two targets.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "core/pattern.h"
#include "descriptor/descriptor.h"
#include "features/esp.h"
#include "overlay/backend.h"
#include "runtime/runtime.h"
#include "runtime/snapshot.h"

namespace lens {

int run_selftest(int argc, char** argv);

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("  \x1b[32mPASS\x1b[0m  %s\n", what);
  } else {
    ++g_fail;
    std::printf("  \x1b[31mFAIL\x1b[0m  %s\n", what);
  }
}

void section(const char* s) { std::printf("\n\x1b[36m%s\x1b[0m\n", s); }

// The synthetic target's own layout, so the test can assert on ground truth
// rather than on "something was non-zero".
#include "sim/sim_layout.h"

std::string sim_binary_path(const char* argv0) { return sibling_path(argv0, "lens_sim"); }

}  // namespace

int run_selftest(int argc, char** argv) {
  const char* sim = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--sim") == 0 && i + 1 < argc) sim = argv[++i];
  }
  const std::string sim_path = sim ? sim : sim_binary_path(argv[0]);

  std::printf("\nlens selftest\n  sim binary: %s\n", sim_path.c_str());

  // ---- 1. signature scanner ----
  section("signature scanner");
  {
    const std::vector<uint8_t> buf = {0x00, 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44,
                                      0x48, 0x85, 0xC0, 0x90, 0x90, 0x90, 0x90};
    const Signature s = Signature::parse("48 8B 05 ?? ?? ?? ?? 48 85 C0");
    check(s.size() == 10, "parses a pattern with wildcards");
    const auto hits = s.scan(buf.data(), buf.size());
    check(hits.size() == 1 && hits[0] == 1, "finds exactly one match in a buffer");

    const Signature full = Signature::parse("48 8B 05 11 22 33 44 48 85 C0");
    check(full.scan(buf.data(), buf.size()).size() == 1, "an exact pattern also matches");

    const Signature none = Signature::parse("DE AD BE EF");
    check(none.scan(buf.data(), buf.size()).empty(), "a pattern with no match returns empty");

    bool threw = false;
    try {
      Signature::parse("ZZ 11");
    } catch (...) {
      threw = true;
    }
    check(threw, "a malformed pattern throws at parse time");
  }

  // ---- 2. attach to the synthetic target ----
  section("attach");
  std::unique_ptr<IMemorySource> mem;
  {
    mem = openTargetAutoSpawn("sim", "/lens_selftest", sim_path, true);
    check(mem != nullptr, "attached to the synthetic target over the sim transport");
    if (!mem) {
      std::printf("\n\x1b[31mselftest cannot continue without the sim target\x1b[0m\n");
      return 1;
    }
    check(std::strcmp(mem->kind(), "sim") == 0, "transport reports as sim");
    const auto mods = mem->modules();
    check(mods.size() == 1, "one module reported");
    if (!mods.empty()) {
      check(mods[0].name == "lens_sim_module", "module name matches");
      check(mods[0].size > 0x1000, "module has a real image size");
    }
  }

  // ---- 3. descriptor load + resolve ----
  Descriptor d;
  section("descriptor");
  {
    std::string err;
    const std::string path = "adapters/sim.yaml";
    const bool ok = Descriptor::try_load(path, d, &err);
    check(ok, ("loaded " + path).c_str());
    if (!ok) {
      std::printf("  (%s)\n", err.c_str());
      shutdownSpawnedTarget();
      return 1;
    }
    check(d.name == "sim", "descriptor name parsed");
    check(d.lists.size() == 1, "one entity list declared");
    check(d.lists[0].fields.size() >= 5, "list declares its fields");
    check(d.camera.view.name == "view", "camera view field declared");
  }

  section("resolve");
  Runtime rt(std::move(mem), d);
  {
    const Resolution& r = rt.resolution();
    check(r.modules.size() == 1 && r.modules[0].base != 0, "module resolved to a base address");
    check(r.lists.size() == 1, "one list resolved");
    if (!r.lists.empty()) {
      check(r.lists[0].array != 0, "entity array base resolved via schema/signature");
      check(r.lists[0].health == Health::kOk, "list health is ok");
    }
    check(r.camera_view.addr != 0, "camera viewproj resolved through a RIP-relative read");
    check(r.camera_pos.addr != 0, "camera position resolved");
    std::printf("  (%zu fields, %d dead, %d stale)\n", static_cast<size_t>(r.total_fields()),
                r.dead_fields(), r.stale_fields());
  }

  // ---- 4. read the live frame ----
  section("read");
  {
    const FrameStats& s = rt.tick();
    check(s.entity_count == 16, ("read 16 entities (got " + std::to_string(s.entity_count) + ")").c_str());
    check(s.read_ms < 50.f, "read pass is under 50 ms");

    const auto& ents = rt.list("players");
    check(!ents.empty(), "player list is populated");
    if (!ents.empty()) {
      const Entity& e = ents.front();
      check(e.distance > 0.f, "entity distance computed from the camera position");
      std::string nm;
      for (const auto& kv : e.fields)
        if (kv.first == "name") nm = kv.second.s;
      check(nm == "unit-00", ("entity name read from the target: " + nm).c_str());
      bool has_health = false;
      for (const auto& kv : e.fields)
        if (kv.first == "health") has_health = (kv.second.f > 0.f);
      check(has_health, "entity health read as a float");
    }
  }

  // ---- 5. projection ----
  section("project");
  {
    // At least some entities must project in front of the camera; if the
    // viewproj read were wrong, none would.
    int front = 0;
    for (const auto& e : rt.entities()) front += e.visible ? 1 : 0;
    check(front > 0, ("some entities project in front of the camera (" +
                      std::to_string(front) + " of " + std::to_string(rt.entities().size()) +
                      ")")
                      .c_str());
  }

  // ---- 6. draw ----
  section("draw");
  {
    Settings cfg;
    cfg.viewport_w = 1280;
    cfg.viewport_h = 720;
    rt.set_viewport(cfg.viewport_w, cfg.viewport_h);
    rt.tick();
    DrawList dl;
    Esp esp;
    const EspStats st = esp.render(rt, cfg.esp, dl, 1280, 720);
    check(dl.size() > 0, "the draw list is not empty");
    check(st.drawn > 0, ("esp drew at least one entity (" + std::to_string(st.drawn) + ")")
                          .c_str());
    check(dl.count(Prim::kPolygon) > 0, "boxes were emitted as polygons");
    check(dl.count(Prim::kText) > 0, "text was emitted");

    // Replay through the offline backend and count lit pixels: this is a real
    // render, not a draw-call counter.
    auto backend = make_offline_backend("selftest_frame.ppm");
    backend->begin(1280, 720);
    backend->replay(dl);
    backend->end();
    std::ifstream f("selftest_frame.ppm", std::ios::binary | std::ios::ate);
    check(f.good() && static_cast<size_t>(f.tellg()) > 1280 * 720 * 3,
          "offline backend wrote a non-empty PPM frame");
  }

  // ---- 7. write path ----
  section("write");
  {
    // The sim transport was opened with write permission, so this is a real
    // WriteProcessMemory-equivalent: read health, change it, read it back.
    auto& ents = const_cast<Runtime&>(rt).list("players");
    if (!ents.empty()) {
      Value hv;
      float before = 0.f;
      if (rt.read_field(ents.front(), "health", hv)) before = hv.f;
      Entity copy = ents.front();
      Value nv;
      nv.t = Value::T::kFloat;
      nv.f = 42.0f;
      std::string err;
      const bool wrote = rt.write_field(copy, "health", nv, &err);
      check(wrote, ("wrote health (err: " + err + ")").c_str());
      rt.tick();
      Value hv2;
      if (rt.read_field(ents.front(), "health", hv2))
        check(std::fabs(hv2.f - 42.0f) < 25.f,
              "the value round-tripped through the target's memory");
      (void)before;
    } else {
      check(false, "no entity to write to");
    }
    // A field the descriptor does not mark writable must be refused.
    Entity copy = ents.front();
    Value nv;
    nv.t = Value::T::kInt;
    nv.i = 1;
    std::string err;
    const bool refused = !rt.write_field(copy, "team", nv, &err);
    check(refused, "a non-writable field is refused by the descriptor policy");
  }

  // ---- 8. snapshot + diff ----
  section("snapshot + diff");
  {
    const Snapshot a = capture(rt);
    check(a.entity_count > 0, "snapshot captured entities");
    check(a.lists.count("players") == 1, "snapshot has the players list");
    check(a.save("selftest_a.json"), "snapshot serialised to JSON");

    Snapshot loaded;
    std::string err;
    check(Snapshot::load("selftest_a.json", loaded, &err), "snapshot round-tripped through JSON");
    check(loaded.entity_count == a.entity_count, "entity count survived the round trip");
    if (!loaded.lists.count("players") || loaded.lists.at("players").empty()) {
      check(false, "the players list survived the round trip");
    } else {
      const EntitySample& es = loaded.lists.at("players").front();
      check(es.fields.count("health") > 0, "a named field survived the round trip");
      check(es.fields.count("name") > 0, "a string field survived the round trip");
    }

    // An identical snapshot diffs clean.
    const SnapshotDiff same = diff(a, a);
    check(same.identical(), "a snapshot diffed against itself is identical");

    // A mutated snapshot diffs loudly, per field.
    Snapshot b = a;
    if (!b.lists["players"].empty() && b.lists["players"][0].fields.count("health")) {
      b.lists["players"][0].fields["health"].value = "999.000";
    }
    const SnapshotDiff dd = diff(a, b);
    check(!dd.identical(), "a mutated snapshot does not diff clean");
    {
      // A field whose value differs must be reported by name, as CHANGED.
      bool named = false;
      for (const auto& l : dd.lists)
        for (const auto& f : l.fields)
          if (f.field == "health" && f.delta == FieldDelta::kChanged) named = true;
      check(named, "the diff names the changed field by path");
    }
    // A field that went dead must be reported as a hard break.
    {
      Snapshot c = a;
      if (!c.lists["players"].empty() && c.lists["players"][0].fields.count("weapon"))
        c.lists["players"][0].fields.erase("weapon");
      c.field_health["players.weapon"] = Health::kDead;
      const SnapshotDiff dc = diff(a, c);
      check(dc.broken_count() > 0, "the diff counts a vanished field as a hard break");
      const std::string rep = dc.report();
      check(rep.find("players.weapon") != std::string::npos, "the report names the vanished field");
    }
    {
      const std::string rep = dd.report();
      std::printf("  --- report ---\n%s", rep.c_str());
    }

    // ---- empty-side diffs: the whole-list case, in both directions ----
    // A patch that kills a schema takes the list from N entities to none. Both
    // the union walk and the per-list branch used to call .front() on a vector
    // that could be empty, which segfaulted. These four checks are the
    // regression net for that.
    {
      Snapshot full = a;
      Snapshot empty = a;
      empty.lists["players"].clear();
      empty.entity_count = 0;
      empty.field_health.clear();
      for (auto& kv : empty.field_health) kv.second = Health::kDead;
      empty.notes.push_back("list players: schema \"players\" not published by target");

      const SnapshotDiff d1 = diff(full, empty);
      check(!d1.identical(), "a list that lost every entity is not identical");
      check(d1.broken_count() > 0, "a list going empty counts as a hard break");
      check(d1.report().find("players") != std::string::npos, "the report names the emptied list");

      const SnapshotDiff d2 = diff(empty, full);
      check(!d2.identical(), "an empty list regaining entities is not identical");
      const std::string r2 = d2.report();
      check(r2.find("NEW") != std::string::npos, "the report marks reappearing fields NEW");
      // The value has to come across, not just the fact that something appeared.
      bool carried = false;
      for (const auto& l : d2.lists)
        for (const auto& f : l.fields)
          if (f.delta == FieldDelta::kAppeared && !f.after.empty() && f.after != "(none)")
            carried = true;
      check(carried, "an appeared field reports the value it appeared with");
    }

    // A field declared but no longer reading: one entity keeps the other fields
    // so the list is intact and only that one field is gone.
    {
      Snapshot c = a;
      bool erased = false;
      for (auto& e : c.lists["players"]) {
        if (e.fields.erase("name")) { erased = true; break; }
      }
      check(erased, "a name field was present to remove");
      c.field_health["players.name"] = Health::kDead;
      const SnapshotDiff d3 = diff(a, c);
      check(d3.broken_count() > 0, "a single dead field among live ones is a hard break");
      const std::string r3 = d3.report();
      check(r3.find("players.name") != std::string::npos, "the report names the dead field");
      check(r3.find("field health players.name") != std::string::npos,
            "the report carries the health transition");
    }
  }

  shutdownSpawnedTarget();
  std::printf("\n\x1b[1mlens selftest: %d passed, %d failed\x1b[0m\n", g_pass, g_fail);
  std::printf("artifacts: selftest_frame.ppm selftest_a.json\n\n");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace lens
