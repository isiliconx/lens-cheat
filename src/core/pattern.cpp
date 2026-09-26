#include "pattern.h"

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace lens {
namespace {

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Signature Signature::parse(std::string_view text) {
  Signature s;
  s.text_ = std::string(text);
  s.bytes_.clear();

  size_t i = 0;
  const size_t n = s.text_.size();
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(s.text_[i]))) ++i;
    if (i >= n) break;

    // "??" - a fully wildcard byte, checked before any hex nibble.
    if (s.text_[i] == '?') {
      ++i;
      if (i < n && s.text_[i] == '?') {
        ++i;
        s.bytes_.push_back(SigByte{0x00, 0x00});
        if (i < n && !std::isspace(static_cast<unsigned char>(s.text_[i])))
          throw std::runtime_error("signature: expected space at offset " + std::to_string(i) +
                                   " in \"" + s.text_ + "\"");
        continue;
      }
      if (i >= n) {
        // A trailing lone '?': low nibble free.
        s.bytes_.push_back(SigByte{0x00, 0x0F});
        break;
      }
      throw std::runtime_error("signature: \"4?\" needs a space after the '?' in \"" + s.text_ +
                               "\" - use \"4? ?\" or \"??\"");
    }

    const int hi = HexNibble(s.text_[i]);
    if (hi < 0)
      throw std::runtime_error("signature: bad hex at offset " + std::to_string(i) +
                               " in \"" + s.text_ + "\"");
    ++i;

    uint8_t value = static_cast<uint8_t>(hi << 4);
    uint8_t mask = 0xF0;

    if (i < n && s.text_[i] == '?') {
      mask = 0x0F;  // "4?" - low nibble free
      ++i;
    } else if (i < n) {
      const int lo = HexNibble(s.text_[i]);
      if (lo < 0)
        throw std::runtime_error("signature: bad hex at offset " + std::to_string(i) +
                                 " in \"" + s.text_ + "\"");
      value |= static_cast<uint8_t>(lo);
      mask = 0xFF;
      ++i;
    } else {
      mask = 0x0F;
    }

    s.bytes_.push_back(SigByte{value, mask});

    if (i < n && !std::isspace(static_cast<unsigned char>(s.text_[i])))
      throw std::runtime_error("signature: expected space at offset " + std::to_string(i) +
                               " in \"" + s.text_ + "\"");
  }

  if (s.bytes_.empty()) throw std::runtime_error("signature: empty pattern \"" + s.text_ + "\"");

  // Pick the fully-masked byte that appears least often in the module corpus
  // heuristics aside, just use the first non-zero one: good enough and stable.
  for (size_t k = 0; k < s.bytes_.size(); ++k) {
    if (s.bytes_[k].mask == 0xFF && s.bytes_[k].value != 0x00) {
      s.anchor_ = k;
      break;
    }
  }
  if (s.anchor_ == SIZE_MAX) {
    for (size_t k = 0; k < s.bytes_.size(); ++k) {
      if (s.bytes_[k].mask == 0xFF) { s.anchor_ = k; break; }
    }
  }
  return s;
}

bool Signature::matches(const uint8_t* p) const {
  for (size_t k = 0; k < bytes_.size(); ++k)
    if ((p[k] & bytes_[k].mask) != (bytes_[k].value & bytes_[k].mask)) return false;
  return true;
}

std::vector<size_t> Signature::scan(const uint8_t* data, size_t len, size_t align) const {
  std::vector<size_t> hits;
  const size_t sig_len = bytes_.size();
  if (len < sig_len || sig_len == 0) return hits;
  if (align == 0) align = 1;

  const size_t last = len - sig_len;
  size_t i = 0;
  if (anchor_ != SIZE_MAX) {
    const uint8_t a = bytes_[anchor_].value;
    while (i <= last) {
      const uint8_t* base = data + i;
      size_t rel = 0;
      // memchr for the anchor byte
      const void* found = memchr(base + anchor_, a, last - i - anchor_ + 1);
      if (!found) break;
      rel = static_cast<size_t>(static_cast<const uint8_t*>(found) - base);
      i += rel;
      if (matches(data + i)) {
        hits.push_back(i);
        i += align;
      } else {
        i += 1;
      }
      if (i > last) break;
    }
    return hits;
  }

  for (i = 0; i <= last; i += align) {
    if (matches(data + i)) hits.push_back(i);
  }
  return hits;
}

std::vector<size_t> Signature::scan(IMemorySource& mem, uintptr_t base, size_t size,
                                    size_t align, int max_hits) const {
  std::vector<size_t> out;
  const size_t sig_len = bytes_.size();
  if (size < sig_len || sig_len == 0) return out;

  const size_t kChunk = 1u << 16;
  const size_t overlap = sig_len - 1;
  std::vector<uint8_t> buf(kChunk + overlap);

  size_t pos = 0;
  while (pos < size) {
    const size_t remain = size - pos;
    const size_t have = std::min(kChunk + (pos ? overlap : 0), remain);
    if (have < sig_len) break;  // a tail too short to hold a match
    if (!mem.read(base + pos, buf.data(), have)) {
      // Unreadable gap (guard page, unmapped hole): step over it. Never step
      // backwards, or a short tail loops forever.
      pos += kChunk;
      continue;
    }
    const size_t usable = have - (pos ? overlap : 0);
    if (usable >= sig_len) {
      for (size_t hit : scan(buf.data(), usable, align)) {
        const size_t abs_off = pos + hit;
        if (abs_off + sig_len > size) continue;
        out.push_back(abs_off);
        if (max_hits > 0 && static_cast<int>(out.size()) >= max_hits) return out;
      }
    }
    if (have <= overlap) break;  // consumed everything readable
    pos += have - overlap;
  }
  return out;
}

}  // namespace lens
