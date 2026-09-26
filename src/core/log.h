// log.h - one-line logging to stderr.
#pragma once
#include <cstdarg>
#include <cstdio>

namespace lens {

enum class Level { kDebug, kInfo, kWarn, kError };

inline void logf(Level lv, const char* fmt, ...) {
  static const char* kTag[] = {"dbg", "inf", "wrn", "err"};
  static const char* kCol[] = {"\x1b[90m", "\x1b[36m", "\x1b[33m", "\x1b[31m"};
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "%s[lens:%s]\x1b[0m ", kCol[(int)lv], kTag[(int)lv]);
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}

// Debug output is compiled out by default: `make DEBUG=1` (or -DLENS_DEBUG) is
// the only way to see it, so a normal run shows real warnings and nothing else.
#ifdef LENS_DEBUG
#define LDEBUG(...) ::lens::logf(::lens::Level::kDebug, __VA_ARGS__)
#else
#define LDEBUG(...) ((void)0)
#endif

#define LINFO(...)  ::lens::logf(::lens::Level::kInfo,  __VA_ARGS__)
#define LWARN(...)  ::lens::logf(::lens::Level::kWarn,  __VA_ARGS__)
#define LERROR(...) ::lens::logf(::lens::Level::kError, __VA_ARGS__)

}  // namespace lens
