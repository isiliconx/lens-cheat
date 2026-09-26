#include "mem.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

namespace lens {

std::string rd_str(IMemorySource& m, uintptr_t a, int max_len) {
  if (max_len <= 0) max_len = 32;
  if (max_len > 4096) max_len = 4096;
  char buf[4097];
  size_t got = 0;
  for (; got < static_cast<size_t>(max_len); ++got) {
    char c = 0;
    if (!m.read(a + got, &c, 1)) break;
    if (c == '\0') break;
    buf[got] = c;
  }
  buf[got] = '\0';
  return std::string(buf, got);
}

bool read_range(IMemorySource& m, uintptr_t base, size_t size, std::vector<uint8_t>& out) {
  out.assign(size, 0);
  if (size == 0) return true;
  return m.read(base, out.data(), size);
}

bool IMemorySource::in_bounds(uintptr_t addr, size_t n) const {
  if (bounds_.empty()) return true;  // unbounded: a real process address space
  for (const Bound& b : bounds_) {
    if (addr < b.base) continue;
    const size_t off = addr - b.base;
    if (n <= b.size && off <= b.size - n) return true;
  }
  return false;
}

std::unique_ptr<IMemorySource> openTarget(const std::string& transport_name,
                                          const std::string& process_name, bool allow_write) {
  std::string t = transport_name;
  if (t.empty()) {
#ifdef _WIN32
    t = "win";
#else
    t = "sim";
#endif
  }
  if (t == "sim") return openSim(process_name, allow_write);
#ifdef _WIN32
  if (t == "win") return openWindows(process_name, allow_write);
#endif
  LERROR("unknown transport \"%s\" (want sim or win)", t.c_str());
  return nullptr;
}

#ifndef _WIN32

namespace {
pid_t g_spawned = -1;
std::string g_shm_created;

void reap_spawned() {
  if (g_spawned > 0) {
    kill(g_spawned, SIGTERM);
    int st = 0;
    waitpid(g_spawned, &st, 0);
    g_spawned = -1;
  }
  if (!g_shm_created.empty()) {
    shm_unlink(g_shm_created.c_str());
    g_shm_created.clear();
  }
}
}  // namespace

void shutdownSpawnedTarget() { reap_spawned(); }

std::string sibling_path(const char* argv0, const char* name) {
  std::string p = argv0 ? argv0 : "";
  const size_t slash = p.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
  return dir + "/" + name;
}

std::unique_ptr<IMemorySource> openTargetAutoSpawn(const std::string& transport_name,
                                                   const std::string& process_name,
                                                   const std::string& sim_binary,
                                                   bool allow_write) {
  std::string t = transport_name.empty() ? std::string("sim") : transport_name;
  if (t != "sim") return openTarget(t, process_name, allow_write);

  // Already running?
  if (auto m = openSim(process_name, false)) return m;

  // The module name the descriptor expects for a sim target is always
  // "lens_sim_module", independent of the segment name, so the selftest and
  // `lens run` resolve the same module.
  const char* kSimModule = "lens_sim_module";

  // The child owns segment creation: it unlinks any stale name, then creates
  // and populates it. The parent never creates the segment, so there is no
  // O_EXCL collision and no window where the name resolves to an empty page.
  const pid_t pid = fork();
  if (pid == 0) {
    if (sim_binary.empty()) _exit(127);
    char* argv[] = {const_cast<char*>(sim_binary.c_str()),
                    const_cast<char*>("--shm"),
                    const_cast<char*>(process_name.c_str()),
                    const_cast<char*>("--module"),
                    const_cast<char*>(kSimModule),
                    nullptr};
    execv(sim_binary.c_str(), argv);
    _exit(127);
  }
  if (pid < 0) {
    LERROR("fork failed: %s", std::strerror(errno));
    return nullptr;
  }
  g_spawned = pid;
  g_shm_created = process_name;  // reap_spawned unlinks it if we bail early
  atexit(reap_spawned);

  // Wait for the child to publish a ready flag.
  for (int i = 0; i < 400; ++i) {
    if (auto m = openSim(process_name, allow_write)) return m;
    if (i == 40) LWARN("child pid %d not up yet; is %s executable?", pid, sim_binary.c_str());
    usleep(10000);
  }
  LERROR("sim target did not come up within 4s (shm %s)", process_name.c_str());
  reap_spawned();
  return nullptr;
}

#else  // _WIN32

void shutdownSpawnedTarget() {}
std::unique_ptr<IMemorySource> openTargetAutoSpawn(const std::string&, const std::string&,
                                                   const std::string&, bool allow_write) {
  return openTarget("", "", allow_write);
}

#endif

}  // namespace lens
