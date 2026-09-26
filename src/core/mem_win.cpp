// mem_win.cpp - live Windows process transport.
//
// Handle discipline: PROCESS_QUERY_LIMITED_INFORMATION always, VM_READ always,
// VM_WRITE|VM_OPERATION only when allowWrite. No debug privilege, no
// PROCESS_ALL_ACCESS, handles closed in the destructor. The process handle is
// stored in a private member of a translation-unit-local wrapper so nothing in
// the rest of lens can reach it.
#include "mem.h"

#ifdef _WIN32

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace lens {
namespace {

// One image per process; attach() is a singleton-ish handle cache so repeated
// descriptor binds do not reopen the process.
class WinMemory final : public IMemorySource {
 public:
  ~WinMemory() override {
    if (h_) {
      CloseHandle(h_);
      h_ = nullptr;
    }
  }

  bool open(const std::string& exe, bool allow_write) {
    allow_write_ = allow_write;

    DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
    if (allow_write) access |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION;

    h_ = OpenProcess(access, FALSE, FindProcessId(exe));
    if (!h_) {
      LERROR("OpenProcess(%s) failed: %lu", exe.c_str(), GetLastError());
      return false;
    }

    if (GetProcessId(h_) == 0) {
      LERROR("process exited before we attached");
      CloseHandle(h_);
      h_ = nullptr;
      return false;
    }
    return true;
  }

  const char* kind() const override { return "win"; }
  bool alive() const override {
    if (!h_) return false;
    return WaitForSingleObject(h_, 0) == WAIT_TIMEOUT;
  }
  bool allowWrite() const override { return allow_write_; }

  bool read(uintptr_t addr, void* dst, size_t n) override {
    if (!h_ || !dst || n == 0) return false;
    SIZE_T got = 0;
    // Retry once: a target can unmap a page between the probe and the copy.
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), dst, n, &got) && got == n)
        return true;
      if (GetLastError() != ERROR_PARTIAL_COPY) break;
    }
    return false;
  }

  bool write(uintptr_t addr, const void* src, size_t n) override {
    if (!h_ || !src || n == 0) return false;
    if (!allow_write_) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(h_, reinterpret_cast<LPVOID>(addr), src, n, &put) && put == n;
  }

  bool region(uintptr_t addr, RegionInfo& out) const override {
    if (!h_) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(h_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
      return false;
    if (mbi.State != MEM_COMMIT) return false;
    out.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    out.size = mbi.RegionSize;
    out.readable = true;
    out.writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;
    out.executable =
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_WRITECOPY |
                        PAGE_EXECUTE_READWRITE)) != 0;
    return true;
  }

  std::vector<ModuleInfo> modules() const override {
    std::vector<ModuleInfo> out;
    if (!h_) return out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(h_));
    if (snap == INVALID_HANDLE_VALUE) {
      LERROR("CreateToolhelp32Snapshot failed: %lu", GetLastError());
      return out;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
      do {
        wchar_t name[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, MAX_PATH, nullptr, nullptr);
        ModuleInfo mi;
        mi.name = name;
        mi.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        mi.size = me.modBaseSize;
        out.push_back(mi);
      } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    std::sort(out.begin(), out.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) { return a.base < b.base; });
    return out;
  }

 private:
  static DWORD FindProcessId(const std::string& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
      do {
        wchar_t name[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, MAX_PATH, nullptr, nullptr);
        if (_stricmp(name, exe.c_str()) == 0) {
          found = pe.th32ProcessID;
          break;
        }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
  }

  HANDLE h_ = nullptr;
  bool allow_write_ = false;
};

}  // namespace

std::unique_ptr<IMemorySource> openWindows(const std::string& exe_name, bool allow_write) {
  auto m = std::make_unique<WinMemory>();
  if (!m->open(exe_name, allow_write)) return nullptr;
  return m;
}

}  // namespace lens

#endif  // _WIN32
