// resolve.h - turns a descriptor into live addresses, and reports what broke.
//
// Resolution is separate from reading on purpose. A location resolves once per
// attach and its address is then cached; a field that fails to resolve is
// marked dead and the rest of the descriptor keeps working. That is what makes
// "which field did the patch kill" a question lens can answer instead of a
// crash.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/mem.h"
#include "descriptor/descriptor.h"

namespace lens {

enum class Health { kOk, kStale, kDead };

struct ResolvedField {
  uintptr_t addr = 0;    // absolute address in the target
  Health health = Health::kDead;
  std::string note;      // why it is stale/dead - reported verbatim to the user
  size_t sig_hits = 0;   // how many times the pattern matched
};

// A module the descriptor referenced, plus whether its anchor signature still
// exists. A missing anchor is the earliest and loudest signal of a patch.
struct ResolvedModule {
  std::string name;
  uintptr_t base = 0;
  size_t size = 0;
  Health health = Health::kDead;
  size_t sig_hits = 0;
  std::string note;
};

struct ResolvedList {
  std::string name;
  uintptr_t array = 0;
  int64_t stride = 0;
  Health health = Health::kDead;
  std::string note;
  std::map<std::string, ResolvedField> fields;  // by field name
};

struct Resolution {
  std::vector<ResolvedModule> modules;
  std::vector<ResolvedList> lists;
  ResolvedField camera_view, camera_pos, camera_fov;
  Health camera_health = Health::kDead;

  int dead_fields() const;
  int stale_fields() const;
  int total_fields() const;
  // One line per broken item, sorted worst-first.
  std::vector<std::string> failures() const;
};

class Resolver {
 public:
  Resolver(IMemorySource& mem, const Descriptor& d);

  // Runs every scan and pointer chase. Cheap enough to re-run on demand
  // (`lens resolve --rescan`) but not per frame.
  const Resolution& resolve();

  const Descriptor& descriptor() const { return d_; }
  const Resolution& resolution() const { return res_; }
  Resolution& resolution() { return res_; }

  // Address a field resolves to, or 0 if dead.
  uintptr_t field_addr(const ResolvedList& list, const std::string& field) const;

  // Find a module by name, case-insensitively.
  const ResolvedModule* module(const std::string& name) const;

  // Schema lookup: the name a target publishes for one of its own tables.
  // Populated by the runtime from the target's schema service, not from the
  // descriptor, so a patch that renumbers the schema does not break adapters.
  struct SchemaEntry {
    uintptr_t array = 0;
    uintptr_t count_ptr = 0;
    int64_t count = 0;
    int64_t stride = 0;
    bool valid = false;
  };
  void install_schema(const std::string& name, const SchemaEntry& e);
  bool lookup_schema(const std::string& name, SchemaEntry& out) const;
  const std::map<std::string, SchemaEntry>& schema_entries() const { return schema_; }

  // Scan cache: pattern text -> match addresses in a given module.
  const std::vector<size_t>* cached_scan(const std::string& module, const std::string& sig);

 private:
  uintptr_t resolve_field(const ListSpec* list, const FieldSpec& f, ResolvedField& out,
                          uintptr_t base_ctx);
  uintptr_t resolve_array(const ListSpec& ls, ResolvedList& out);
  bool resolve_count(const ListSpec& ls, uintptr_t array, int64_t& count, std::string& note);

  IMemorySource& mem_;
  const Descriptor& d_;
  Resolution res_;
  std::map<std::string, SchemaEntry> schema_;
  std::map<std::string, std::vector<size_t>> scan_cache_;
  std::map<std::string, uintptr_t> module_base_;

  friend class Runtime;
};

}  // namespace lens
