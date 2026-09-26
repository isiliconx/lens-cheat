// snapshot.h - record a resolved picture, and diff two pictures.
//
// This is the reason the project exists. Take a snapshot of every declared
// field across every entity, save it, let the game update, take another, and
// the diff tells you which fields moved, which values changed shape, and which
// signatures stopped matching - per field, not per file.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace lens {

struct FieldSample {
  int index = 0;
  std::string type;
  std::string value;      // canonical text form, comparable
  uintptr_t addr = 0;
  bool readable = true;
  Health health = Health::kOk;
};

struct EntitySample {
  int index = 0;
  uintptr_t addr = 0;
  std::map<std::string, FieldSample> fields;
};

struct Snapshot {
  std::string descriptor;
  std::string descriptor_version;
  std::string transport;
  std::string module_name;
  uintptr_t module_base = 0;
  uint64_t taken_at = 0;
  int entity_count = 0;
  std::map<std::string, std::vector<EntitySample>> lists;
  // field name -> health at capture time
  std::map<std::string, Health> field_health;
  std::vector<std::string> notes;

  std::string to_json(int indent = 0) const;
  static bool from_json(const std::string& text, Snapshot& out, std::string* err);
  bool save(const std::string& path) const;
  static bool load(const std::string& path, Snapshot& out, std::string* err);
};

enum class FieldDelta { kSame, kChanged, kAppeared, kVanished, kAddressMoved, kHealthWorse };

struct FieldDiff {
  std::string field;
  FieldDelta delta = FieldDelta::kSame;
  std::string before, after;
  uintptr_t addr_before = 0, addr_after = 0;
  int sample_count = 0;
};

struct ListDiff {
  std::string list;
  int count_before = 0, count_after = 0;
  std::vector<FieldDiff> fields;
};

struct SnapshotDiff {
  std::vector<ListDiff> lists;
  std::vector<std::string> module_changes;
  bool identical() const {
    if (!module_changes.empty()) return false;
    for (const auto& l : lists)
      for (const auto& f : l.fields)
        if (f.delta != FieldDelta::kSame) return false;
    return true;
  }
  // Human-readable, sorted worst-first. This is the output people paste in an
  // issue.
  std::string report() const;
  int broken_count() const;
};

Snapshot capture(Runtime& rt);
SnapshotDiff diff(const Snapshot& a, const Snapshot& b);

}  // namespace lens
