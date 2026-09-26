// mem_sim.cpp - POSIX shared-memory transport.
//
// Present so the full pipeline (attach -> scan -> resolve -> enumerate ->
// project -> draw) runs on a Linux box with no game installed, and so the
// self-test has something to attach to. The code path is identical to the
// Windows transport: every read goes through IMemorySource and every pointer
// is translated through the same base arithmetic a real process needs.
#include "mem.h"

#ifndef _WIN32

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include "log.h"
#include "sim/sim_layout.h"

namespace lens {
namespace {

class SimMemory final : public IMemorySource {
 public:
  ~SimMemory() override {
    if (map_) munmap(map_, SIM_SHM_SIZE);
    if (fd_ >= 0) close(fd_);
  }

  bool open(const std::string& shm_name, bool allow_write) {
    LDEBUG("openSim: %s", shm_name.c_str());
    allow_write_ = allow_write;
    fd_ = shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (fd_ < 0) { LDEBUG("openSim: shm_open failed"); return false; }
    map_ = static_cast<uint8_t*>(mmap(nullptr, SIM_SHM_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd_, 0));
    if (map_ == MAP_FAILED) {
      map_ = nullptr; close(fd_); fd_ = -1;
      LDEBUG("openSim: mmap failed");
      return false;
    }
    const SimHeader* h = header();
    const bool ok = h && h->magic == SIM_SHM_MAGIC && h->ready == 1;
    LDEBUG("openSim: magic=%08x ready=%u -> %s", h ? h->magic : 0, h ? h->ready : 0, ok ? "OK" : "not ready");
    if (!ok) { munmap(map_, SIM_SHM_SIZE); map_ = nullptr; close(fd_); fd_ = -1; }
    return ok;
  }

  const char* kind() const override { return "sim"; }
  bool alive() const override { return map_ && header() && header()->ready == 1; }
  bool allowWrite() const override { return allow_write_; }

  bool read(uintptr_t addr, void* dst, size_t n) override {
    if (!map_ || !dst || n == 0) return false;
    if (!in_range(addr, n)) return false;
    std::memcpy(dst, map_ + (addr - SIM_BASE), n);
    return true;
  }

  bool write(uintptr_t addr, const void* src, size_t n) override {
    if (!allow_write_ || !map_ || !src || n == 0) return false;
    if (!in_range(addr, n)) return false;
    std::memcpy(map_ + (addr - SIM_BASE), src, n);
    return true;
  }

  bool region(uintptr_t addr, RegionInfo& out) const override {
    if (!map_ || !in_range(addr, 1)) return false;
    const uintptr_t off = addr - SIM_BASE;
    out.base = SIM_BASE + (off & ~0xFFFull);
    out.size = 0x1000;
    out.readable = true;
    // The data half of the segment is genuinely writable; the text half is not.
    const uintptr_t seg = off & ~(uintptr_t)(SIM_OFF_SCRATCH - 1);
    out.writable = seg >= SIM_OFF_DATA;
    out.executable = seg < SIM_OFF_DATA;
    return true;
  }

  std::vector<ModuleInfo> modules() const override {
    std::vector<ModuleInfo> out;
    const SimHeader* h = header();
    if (!h || h->magic != SIM_SHM_MAGIC) return out;
    ModuleInfo mi;
    mi.name = h->module_name;
    mi.base = static_cast<uintptr_t>(h->module_base);
    mi.size = static_cast<size_t>(h->module_size);
    out.push_back(mi);
    return out;
  }

 private:
  const SimHeader* header() const { return reinterpret_cast<const SimHeader*>(map_); }

  static bool in_range(uintptr_t addr, size_t n) {
    if (addr < SIM_BASE) return false;
    const uint64_t off = addr - SIM_BASE;
    return off < SIM_SHM_SIZE && n <= SIM_SHM_SIZE - off;
  }

  uint8_t* map_ = nullptr;
  int fd_ = -1;
  bool allow_write_ = false;
};

}  // namespace

std::unique_ptr<IMemorySource> openSim(const std::string& shm_name, bool allow_write) {
  auto m = std::make_unique<SimMemory>();
  if (!m->open(shm_name, allow_write)) return nullptr;
  return m;
}

}  // namespace lens

#endif  // _WIN32
