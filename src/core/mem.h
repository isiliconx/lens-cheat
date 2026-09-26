// mem.h - transport abstraction.
//
// Every read in lens goes through IMemorySource. Two transports ship:
//   win - OpenProcess + ReadProcessMemory, module list from ToolHelp32.
//   sim - a POSIX shared-memory segment published by lens_sim, laid out to
//         look exactly like a loaded module image, so the same descriptor
//         resolves and the same code path runs.
//
// Writes are gated: a transport opened with allowWrite=false refuses writes
// outright, so a read-only run physically cannot modify the target.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lens {

struct ModuleInfo {
  std::string name;
  uintptr_t base = 0;
  size_t size = 0;
};

struct RegionInfo {
  uintptr_t base = 0;
  size_t size = 0;
  bool readable = false;
  bool writable = false;
  bool executable = false;
};

class IMemorySource {
 public:
  virtual ~IMemorySource() = default;

  virtual const char* kind() const = 0;
  virtual bool alive() const = 0;
  virtual bool allowWrite() const = 0;

  virtual bool read(uintptr_t addr, void* dst, size_t n) = 0;
  virtual bool write(uintptr_t addr, const void* src, size_t n) = 0;
  virtual bool region(uintptr_t addr, RegionInfo& out) const = 0;
  virtual std::vector<ModuleInfo> modules() const = 0;

  // A declared extent the read path refuses to leave. A real process has no
  // finite address space, but a schema-driven struct does: the entity array
  // plus its stride, the module plus its size. Reading outside one of these is
  // a broken descriptor, not data, and is refused so a wrong offset is reported
  // as a dead field instead of reading whatever happens to be mapped.
  struct Bound {
    std::string name;
    uintptr_t base = 0;
    size_t size = 0;
  };
  virtual void add_bound(const Bound& b) { bounds_.push_back(b); }
  virtual void clear_bounds() { bounds_.clear(); }
  // Returns false when addr+n falls outside every bound, or outside all bounds
  // when none are set.
  bool in_bounds(uintptr_t addr, size_t n) const;
  const std::vector<Bound>& bounds() const { return bounds_; }

 private:
  std::vector<Bound> bounds_;
};

template <class T>
inline bool rd(IMemorySource& m, uintptr_t a, T& out) {
  return m.read(a, &out, sizeof(T));
}

// Reads up to max_len bytes, stops at the first NUL.
std::string rd_str(IMemorySource& m, uintptr_t a, int max_len);

// Pull a whole module into memory for offline scanning / dumping.
bool read_range(IMemorySource& m, uintptr_t base, size_t size, std::vector<uint8_t>& out);

// Transports.
std::unique_ptr<IMemorySource> openSim(const std::string& shm_name, bool allow_write);

#ifdef _WIN32
std::unique_ptr<IMemorySource> openWindows(const std::string& exe_name, bool allow_write);
#endif

// Dispatch on the descriptor's transport field. Empty transport_name means
// "win on Windows, sim elsewhere".
std::unique_ptr<IMemorySource> openTarget(const std::string& transport_name,
                                          const std::string& process_name, bool allow_write);

// Factory that spawns the synthetic target on demand, so `lens` is one command.
std::unique_ptr<IMemorySource> openTargetAutoSpawn(const std::string& transport_name,
                                                    const std::string& process_name,
                                                    const std::string& sim_binary,
                                                    bool allow_write);
// Absolute path to a sibling executable, resolved once so a forked child can
// execv a relative path from any working directory.
std::string sibling_path(const char* argv0, const char* name);
void shutdownSpawnedTarget();

}  // namespace lens
