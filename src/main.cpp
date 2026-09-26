// main.cpp - the CLI. Every capability lens has is a subcommand, and every
// subcommand is one command in a script.
//
//   lens run <descriptor>            attach, resolve, render (default)
//   lens resolve <descriptor>        resolve only, print the health table
//   lens snapshot <descriptor> <out> capture a semantic picture to JSON
//   lens diff <a.json> <b.json>      field-by-field diff of two snapshots
//   lens list <descriptor>           print the entity table to stdout
//   lens dump <descriptor> <out.ppm> render one frame offline and write a PPM
//   lens selftest                    the whole pipeline against lens_sim
//
// The runtime is headless by default: it draws into a DrawList and replays it
// through the offline backend, so every command above works with no display.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config/settings.h"
#include "core/log.h"
#include "core/mem.h"
#include "descriptor/descriptor.h"
#include "features/editor.h"
#include "features/esp.h"
#include "features/panel.h"
#include "overlay/backend.h"
#include "runtime/runtime.h"
#include "runtime/snapshot.h"

namespace lens {
int run_selftest(int argc, char** argv);
}  // namespace lens

#ifndef LENS_VERSION
#define LENS_VERSION "0.0.0-dev"
#endif

using namespace lens;

namespace {

const char* kUsage =
    "lens " LENS_VERSION " - descriptor-driven cross-engine live inspector\n"
    "\n"
    "usage:\n"
    "  lens run <descriptor> [--write] [--config <file>] [--frames N]\n"
    "  lens resolve <descriptor>\n"
    "  lens list <descriptor> [--frames N]\n"
    "  lens snapshot <descriptor> <out.json> [--write]\n"
    "  lens diff <a.json> <b.json>\n"
    "  lens dump <descriptor> <out.ppm> [--svg <out.svg>]\n"
    "  lens selftest [--keep]\n"
    "  lens version\n"
    "\n"
    "flags:\n"
    "  --write         open the transport with write permission (off by default)\n"
    "  --transport T   force sim|win instead of the descriptor's own field\n"
    "  --sim <path>    path to lens_sim for the sim transport (auto-detected)\n"
    "  --config <file> load a settings file\n"
    "  --frames N      number of read passes (default 120 for run)\n";

std::string find_sim_binary(const char* argv0) { return sibling_path(argv0, "lens_sim"); }

struct Options {
  std::string descriptor;
  std::string out_a, out_b;
  std::string svg;
  std::string transport;
  std::string sim;
  std::string config;
  bool write = false;
  int frames = 120;
  bool keep = false;
};

bool parse_args(int argc, char** argv, int from, Options& o) {
  for (int i = from; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 < argc) { dst = argv[++i]; return true; }
      LERROR("%s needs a value", a.c_str());
      return false;
    };
    if (a == "--write") o.write = true;
    else if (a == "--keep") o.keep = true;
    else if (a == "--transport") { if (!next(o.transport)) return false; }
    else if (a == "--sim") { if (!next(o.sim)) return false; }
    else if (a == "--config") { if (!next(o.config)) return false; }
    else if (a == "--svg") { if (!next(o.svg)) return false; }
    else if (a == "--frames") {
      std::string v;
      if (!next(v)) return false;
      o.frames = static_cast<int>(std::strtol(v.c_str(), nullptr, 10));
    } else if (a.rfind("--", 0) == 0) {
      LERROR("unknown flag %s", a.c_str());
      return false;
    } else if (o.descriptor.empty()) {
      o.descriptor = a;
    } else if (o.out_a.empty()) {
      o.out_a = a;
    } else if (o.out_b.empty()) {
      o.out_b = a;
    } else {
      LERROR("unexpected argument %s", a.c_str());
      return false;
    }
  }
  return true;
}

// Attach to whatever the descriptor names. Returns null and logs why on failure.
std::unique_ptr<Runtime> attach(const Descriptor& d, const Options& o) {
  std::string transport = o.transport.empty() ? d.transport : o.transport;
  std::string process = d.process;
  // A sim descriptor names its shared segment in the process field.
  std::unique_ptr<IMemorySource> mem;
  if (transport.empty()) transport = "sim";

  if (transport == "sim") {
    if (process.empty() || process.rfind("/", 0) == 0) {
      process = d.process.empty() ? "/lens_sim" : d.process;
      if (process[0] != '/') process = "/" + process;
    }
    std::string sim_bin = o.sim.empty() ? find_sim_binary(nullptr) : o.sim;
    mem = openTargetAutoSpawn("sim", process, sim_bin, o.write);
    if (!mem) {
      LERROR("sim target \"%s\" unavailable - build lens_sim and pass --sim <path>",
             process.c_str());
      return nullptr;
    }
  } else {
    mem = openTargetAutoSpawn(transport, process, "", o.write);
    if (!mem) {
      LERROR("could not attach to %s over %s", process.c_str(), transport.c_str());
      return nullptr;
    }
  }

  auto rt = std::make_unique<Runtime>(std::move(mem), d);
  LINFO("attached: transport=%s write=%s", rt->mem().kind(),
        rt->mem().allowWrite() ? "on" : "off");
  for (const auto& m : rt->resolver().resolution().modules)
    LINFO("module %-24s base=0x%llx size=0x%zx %s", m.name.c_str(),
          static_cast<unsigned long long>(m.base), m.size,
          m.health == Health::kOk ? "ok" : m.health == Health::kStale ? "STALE" : "DEAD");
  std::fflush(stderr);
  return rt;
}

int cmd_resolve(const Options& o) {
  Descriptor d;
  std::string err;
  if (!Descriptor::try_load(o.descriptor, d, &err)) {
    LERROR("%s: %s", o.descriptor.c_str(), err.c_str());
    return 2;
  }
  auto rt = attach(d, o);
  if (!rt) return 3;
  const Resolution& r = rt->resolution();

  std::printf("descriptor : %s (%s, %s)\n", d.name.c_str(), d.engine.c_str(), d.process.c_str());
  std::printf("modules    : %zu\n", r.modules.size());
  for (const auto& m : r.modules)
    std::printf("  %-22s 0x%llx  %s%s\n", m.name.c_str(),
                static_cast<unsigned long long>(m.base),
                m.health == Health::kOk ? "ok" : m.health == Health::kStale ? "stale" : "DEAD",
                m.note.empty() ? "" : ("  - " + m.note).c_str());
  std::printf("fields     : %d total, %d dead, %d stale\n", r.total_fields(), r.dead_fields(),
              r.stale_fields());
  for (const auto& l : r.lists) {
    std::printf("list %s  array=0x%llx stride=%lld  %s\n", l.name.c_str(),
                static_cast<unsigned long long>(l.array), static_cast<long long>(l.stride),
                l.health == Health::kOk ? "ok" : "DEAD");
    for (const auto& kv : l.fields)
      std::printf("    %-16s 0x%llx  %-6s %s\n", kv.first.c_str(),
                  static_cast<unsigned long long>(kv.second.addr),
                  kv.second.health == Health::kOk ? "ok"
                  : kv.second.health == Health::kStale ? "stale" : "DEAD",
                  kv.second.note.c_str());
  }
  std::printf("camera     : view=0x%llx pos=0x%llx fov=0x%llx\n",
              static_cast<unsigned long long>(r.camera_view.addr),
              static_cast<unsigned long long>(r.camera_pos.addr),
              static_cast<unsigned long long>(r.camera_fov.addr));

  // One read pass so "resolved" is proven against live data, not just addresses.
  const FrameStats& s = rt->tick();
  std::printf("read pass  : %d entities, %.2f ms\n", s.entity_count, s.read_ms);
  return r.dead_fields() > 0 ? 1 : 0;
}

int cmd_list(const Options& o) {
  Descriptor d;
  std::string err;
  if (!Descriptor::try_load(o.descriptor, d, &err)) {
    LERROR("%s: %s", o.descriptor.c_str(), err.c_str());
    return 2;
  }
  auto rt = attach(d, o);
  if (!rt) return 3;
  rt->tick();

  for (const auto& ls : d.lists) {
    const auto& ents = rt->list(ls.name);
    std::printf("== %s (%zu)\n", ls.name.c_str(), ents.size());
    for (const auto& e : ents) {
      std::printf("  [%d] 0x%llx  dist=%.1f  %s\n", e.index,
                  static_cast<unsigned long long>(e.addr), e.distance,
                  e.visible ? "front" : "behind");
      for (const auto& kv : e.fields)
        std::printf("        %-16s %s\n", kv.first.c_str(), kv.second.to_string().c_str());
    }
  }
  return 0;
}

int cmd_snapshot(const Options& o) {
  Descriptor d;
  std::string err;
  if (!Descriptor::try_load(o.descriptor, d, &err)) {
    LERROR("%s: %s", o.descriptor.c_str(), err.c_str());
    return 2;
  }
  auto rt = attach(d, o);
  if (!rt) return 3;
  for (int i = 0; i < std::max(1, o.frames / 10); ++i) rt->tick();
  const Snapshot s = capture(*rt);
  if (!s.save(o.out_a.empty() ? "snapshot.json" : o.out_a)) return 4;
  std::printf("wrote %s (%d entities, %zu notes)\n", o.out_a.c_str(), s.entity_count,
              s.notes.size());
  return 0;
}

int cmd_diff(const Options& o) {
  if (o.descriptor.empty() || o.out_a.empty()) {
    LERROR("diff needs two snapshot files");
    return 2;
  }
  Snapshot a, b;
  std::string err;
  if (!Snapshot::load(o.descriptor, a, &err)) { LERROR("%s", err.c_str()); return 2; }
  if (!Snapshot::load(o.out_a, b, &err)) { LERROR("%s", err.c_str()); return 2; }
  const SnapshotDiff d = diff(a, b);
  std::fputs(d.report().c_str(), stdout);
  return d.identical() ? 0 : 1;
}

int cmd_dump(const Options& o) {
  Descriptor d;
  std::string err;
  if (!Descriptor::try_load(o.descriptor, d, &err)) {
    LERROR("%s: %s", o.descriptor.c_str(), err.c_str());
    return 2;
  }
  auto rt = attach(d, o);
  if (!rt) return 3;

  Settings cfg = o.config.empty() ? Settings{} : Settings::load(o.config);
  if (cfg.viewport_w == 0) cfg.viewport_w = 1280;
  if (cfg.viewport_h == 0) cfg.viewport_h = 720;
  rt->set_viewport(cfg.viewport_w, cfg.viewport_h);

  DrawList dl;
  Esp esp;
  Panel panel;
  Editor editor;
  EspStats es;
  for (int i = 0; i < std::max(1, o.frames / 20); ++i) {
    rt->tick();
    dl.clear();
    es = esp.render(*rt, cfg.esp, dl, cfg.viewport_w, cfg.viewport_h);
    panel.render(*rt, cfg.panel, es, dl);
    if (cfg.editor.enabled) editor.render(*rt, cfg.editor, dl, cfg.viewport_w, cfg.viewport_h, 0);
  }

  const std::string ppm = o.out_a.empty() ? "frame.ppm" : o.out_a;
  auto backend = make_offline_backend(ppm);
  backend->begin(cfg.viewport_w, cfg.viewport_h);
  backend->replay(dl);
  backend->end();
  if (!o.svg.empty()) write_svg(dl, cfg.viewport_w, cfg.viewport_h, o.svg);

  std::printf("wrote %s (%u primitives, %d entities, %d drawn)\n", ppm.c_str(),
              static_cast<unsigned>(dl.size()), rt->stats().entity_count, es.drawn);
  return 0;
}

int cmd_run(const Options& o) {
  Descriptor d;
  std::string err;
  if (!Descriptor::try_load(o.descriptor, d, &err)) {
    LERROR("%s: %s", o.descriptor.c_str(), err.c_str());
    return 2;
  }
  auto rt = attach(d, o);

  Settings cfg = o.config.empty() ? Settings{} : Settings::load(o.config);
  if (cfg.viewport_w == 0) cfg.viewport_w = 1280;
  if (cfg.viewport_h == 0) cfg.viewport_h = 720;
  cfg.allow_write = o.write;
  rt->set_viewport(cfg.viewport_w, cfg.viewport_h);

  DrawList dl;
  Esp esp;
  Panel panel;
  Editor editor;
  const int frames = o.frames;

  std::printf("lens " LENS_VERSION ": %s, %d frames, %dx%d, transport %s, write %s\n",
              d.name.c_str(), frames, cfg.viewport_w, cfg.viewport_h,
              rt->mem().kind(), o.write ? "ON" : "off");
  for (int i = 0; i < frames; ++i) {
    rt->tick();
    dl.clear();
    const EspStats es = esp.render(*rt, cfg.esp, dl, cfg.viewport_w, cfg.viewport_h);
    panel.render(*rt, cfg.panel, es, dl);
    if (cfg.editor.enabled) editor.render(*rt, cfg.editor, dl, cfg.viewport_w, cfg.viewport_h, i);
    if (i == 0 || (i + 1) % 30 == 0)
      std::printf("frame %4d  %d entities  %d drawn  read %.2f ms\n", i,
                  rt->stats().entity_count, es.drawn, rt->stats().read_ms);
  }

  if (!cfg.dump_path.empty()) {
    auto b = make_offline_backend(cfg.dump_path);
    b->begin(cfg.viewport_w, cfg.viewport_h);
    b->replay(dl);
    b->end();
  }
  shutdownSpawnedTarget();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs(kUsage, stdout);
    return 0;
  }
  const std::string cmd = argv[1];
  if (cmd == "version" || cmd == "--version" || cmd == "-v") {
    std::printf("lens %s\n", LENS_VERSION);
    return 0;
  }
  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    std::fputs(kUsage, stdout);
    return 0;
  }

  Options o;
  o.frames = 120;
  if (!parse_args(argc, argv, 2, o)) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  LDEBUG("cmd=%s descriptor=%s", cmd.c_str(), o.descriptor.c_str());
  std::fflush(stderr);
  // The sim path defaults to a sibling lens_sim, resolved once here so every
  // subcommand attaches the same way.
  if (o.sim.empty()) o.sim = find_sim_binary(argv[0]);

  if (cmd == "resolve") return cmd_resolve(o);
  if (cmd == "list") return cmd_list(o);
  if (cmd == "snapshot") return cmd_snapshot(o);
  if (cmd == "diff") return cmd_diff(o);
  if (cmd == "dump") return cmd_dump(o);
  if (cmd == "run") return cmd_run(o);
  if (cmd == "selftest") return lens::run_selftest(argc, argv);

  LERROR("unknown command \"%s\"", cmd.c_str());
  std::fputs(kUsage, stderr);
  return 2;
}
