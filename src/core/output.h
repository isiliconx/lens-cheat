// output.h - create the parent directory a path names, so a --config that
// points at out/frame.ppm works on a fresh checkout.
#pragma once
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace lens {

inline void ensure_parent_dir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return;
  const std::string dir = path.substr(0, slash);
#ifdef _WIN32
  ::_mkdir(dir.c_str());
#else
  // Create each component in turn; an existing directory is not an error.
  std::string acc;
  size_t i = 0;
  if (!dir.empty() && dir[0] == '/') { acc = "/"; i = 1; }
  while (i <= dir.size()) {
    const size_t next = dir.find('/', i);
    const std::string part =
        dir.substr(i, next == std::string::npos ? std::string::npos : next - i);
    if (!part.empty()) {
      if (acc.empty() || acc == "/") acc += part;
      else acc += "/" + part;
      ::mkdir(acc.c_str(), 0755);
    }
    if (next == std::string::npos) break;
    i = next + 1;
  }
#endif
}

}  // namespace lens
