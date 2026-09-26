// pattern.h - signature scanner with wildcards.
//
// Token grammar, whitespace separated:
//   "48 8B 05"   exact bytes
//   "4? A?"      high nibble known, low nibble wildcard
//   "??"         fully wildcard
//   "48 8B 05 ?? ?? ?? ?? 48 85 C0"   classic RIP-relative prologue
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mem.h"

namespace lens {

struct SigByte {
  uint8_t value = 0;
  uint8_t mask = 0xFF;  // bits that must match
};

class Signature {
 public:
  // Throws std::runtime_error on a malformed token.
  static Signature parse(std::string_view text);

  bool valid() const { return !bytes_.empty(); }
  size_t size() const { return bytes_.size(); }
  const std::vector<SigByte>& bytes() const { return bytes_; }
  const std::string& text() const { return text_; }

  bool matches(const uint8_t* p) const;

  // Scan a flat buffer. align constrains the start address.
  std::vector<size_t> scan(const uint8_t* data, size_t len, size_t align = 1) const;

  // Scan a region in a live target. Reads in chunks with an overlap of
  // size()-1 so a signature spanning a chunk boundary still hits.
  // Stops after max_hits matches.
  std::vector<size_t> scan(IMemorySource& mem, uintptr_t base, size_t size, size_t align = 1,
                          int max_hits = 64) const;

 private:
  size_t anchor_ = SIZE_MAX;  // index of the most selective fully-masked byte
  std::string text_;
  std::vector<SigByte> bytes_;
};

}  // namespace lens
