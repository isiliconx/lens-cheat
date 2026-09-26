// yaml.h - the subset of YAML a descriptor needs.
//
// Block mappings, block sequences, scalars, inline flow sequences of scalars,
// comments, and two block scalars (| and >) for pattern blobs. No anchors, no
// aliases, no flow mappings. Deliberately small: a descriptor that a
// non-specialist can read in one sitting is the whole point of the format.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lens {

struct YNode {
  enum class Kind { kScalar, kMap, kSeq };
  Kind kind = Kind::kScalar;
  std::string scalar;

  std::vector<std::pair<std::string, YNode>> map;  // insertion-ordered
  std::vector<YNode> seq;

  bool is_scalar() const { return kind == Kind::kScalar; }
  bool is_map() const { return kind == Kind::kMap; }
  bool is_seq() const { return kind == Kind::kSeq; }

  const YNode* find(const std::string& key) const;
  std::string str(const std::string& key, const std::string& def = "") const;
  long long i64(const std::string& key, long long def = 0) const;
  double f64(const std::string& key, double def = 0.0) const;
  bool boolean(const std::string& key, bool def = false) const;
  std::vector<std::string> str_list(const std::string& key) const;

  static YNode parse(const std::string& text);       // throws std::runtime_error
  static bool try_parse(const std::string& text, YNode& out, std::string* err = nullptr);
};

std::string read_file(const std::string& path);      // throws

}  // namespace lens
