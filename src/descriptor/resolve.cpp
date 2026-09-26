#include "resolve.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/log.h"
#include "core/pattern.h"

namespace lens {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// disp32 -> absolute pointer, the shape of `lea rcx, [rip+disp32]`.
bool rip_dispatch(IMemorySource& mem, uintptr_t instr, int rip_off, uintptr_t& out) {
  int32_t disp = 0;
  if (!mem.read(instr + rip_off, &disp, 4)) return false;
  out = (instr + rip_off + 4) + static_cast<int64_t>(disp);
  return true;
}

}  // namespace

int Resolution::dead_fields() const {
  int n = 0;
  for (const auto& l : lists)
    for (const auto& kv : l.fields) n += (kv.second.health == Health::kDead);
  n += (camera_view.health == Health::kDead);
  n += (camera_pos.health == Health::kDead);
  n += (camera_fov.health == Health::kDead);
  return n;
}

int Resolution::stale_fields() const {
  int n = 0;
  for (const auto& l : lists)
    for (const auto& kv : l.fields) n += (kv.second.health == Health::kStale);
  n += (camera_view.health == Health::kStale);
  n += (camera_pos.health == Health::kStale);
  n += (camera_fov.health == Health::kStale);
  return n;
}

int Resolution::total_fields() const {
  int n = 3;
  for (const auto& l : lists) n += static_cast<int>(l.fields.size());
  return n;
}

std::vector<std::string> Resolution::failures() const {
  std::vector<std::string> out;
  for (const auto& m : modules)
    if (m.health != Health::kOk) out.push_back("module " + m.name + ": " + m.note);
  for (const auto& l : lists) {
    if (l.health != Health::kOk) out.push_back("list " + l.name + ": " + l.note);
    for (const auto& kv : l.fields)
      if (kv.second.health != Health::kOk)
        out.push_back(l.name + "." + kv.first + ": " + kv.second.note);
  }
  const std::pair<const char*, const ResolvedField*> cams[] = {
      {"camera.view", &camera_view}, {"camera.pos", &camera_pos}, {"camera.fov", &camera_fov}};
  for (const auto& c : cams)
    if (c.second->health != Health::kOk)
      out.push_back(std::string(c.first) + ": " + c.second->note);
  std::sort(out.begin(), out.end());
  return out;
}

Resolver::Resolver(IMemorySource& mem, const Descriptor& d) : mem_(mem), d_(d) {}

const ResolvedModule* Resolver::module(const std::string& name) const {
  for (const auto& m : res_.modules)
    if (lower(m.name) == lower(name)) return &m;
  return nullptr;
}

void Resolver::install_schema(const std::string& name, const SchemaEntry& e) {
  schema_[name] = e;
}

bool Resolver::lookup_schema(const std::string& name, SchemaEntry& out) const {
  auto it = schema_.find(name);
  if (it == schema_.end()) return false;
  out = it->second;
  return out.valid;
}

const std::vector<size_t>* Resolver::cached_scan(const std::string& module,
                                                 const std::string& sig) {
  const std::string key = lower(module) + "|" + sig;
  auto it = scan_cache_.find(key);
  if (it != scan_cache_.end()) return &it->second;

  const ResolvedModule* m = this->module(module);
  if (!m || m->base == 0) return nullptr;
  Signature s;
  try {
    s = Signature::parse(sig);
  } catch (const std::exception&) {
    return nullptr;
  }
  auto hits = s.scan(mem_, m->base, m->size, 1, 64);
  auto [ins, ok] = scan_cache_.emplace(key, std::move(hits));
  (void)ok;
  return &ins->second;
}

uintptr_t Resolver::field_addr(const ResolvedList& list, const std::string& field) const {
  auto it = list.fields.find(field);
  if (it == list.fields.end() || it->second.health == Health::kDead) return 0;
  return it->second.addr;
}

const Resolution& Resolver::resolve() {
  res_ = Resolution{};
  scan_cache_.clear();
  module_base_.clear();

  // ---- modules ----
  const auto live = mem_.modules();
  if (live.empty()) {
    res_.modules.push_back({"", 0, 0, Health::kDead, 0, "no modules reported by transport"});
  }
  for (const auto& ms : d_.modules) {
    ResolvedModule rm;
    rm.name = ms.name;
    for (const auto& lm : live) {
      if (lower(lm.name) == lower(ms.name)) {
        rm.base = lm.base;
        rm.size = lm.size;
        break;
      }
    }
    // Fall back to a fuzzy match: engines rename modules between versions, and a
    // descriptor that dies on a suffix change is useless.
    if (rm.base == 0) {
      for (const auto& lm : live) {
        if (lower(lm.name).find(lower(ms.name)) != std::string::npos) {
          rm.base = lm.base;
          rm.size = lm.size;
          rm.note = "fuzzy-matched module \"" + lm.name + "\"";
          break;
        }
      }
    }
    if (rm.base == 0) {
      rm.health = Health::kDead;
      rm.note = "module not loaded in target";
    } else {
      rm.health = Health::kOk;
      module_base_[lower(ms.name)] = rm.base;
      if (!ms.sig.empty()) {
        Signature s = Signature::parse(ms.sig);
        auto hits = s.scan(mem_, rm.base, rm.size, 1, 8);
        rm.sig_hits = hits.size();
        if (hits.empty()) {
          rm.health = Health::kStale;
          rm.note = "anchor signature absent - module changed or wrong build";
        }
      }
    }
    res_.modules.push_back(rm);
  }
  if (res_.modules.empty() && !d_.modules.empty() == false) {
    // descriptor declared no modules: assume the main image
  }

  // ---- lists ----
  for (const auto& ls : d_.lists) {
    ResolvedList rl;
    rl.name = ls.name;
    rl.array = resolve_array(ls, rl);
    if (rl.array == 0) {
      rl.health = Health::kDead;
      if (rl.note.empty()) rl.note = "array base did not resolve";
    } else {
      rl.health = Health::kOk;
    }

    for (const auto& f : ls.fields) {
      ResolvedField rf;
      rf.addr = resolve_field(&ls, f, rf, rl.array);
      rf.health = rf.addr ? (rf.health == Health::kOk ? Health::kOk : rf.health) : Health::kDead;
      if (rf.addr == 0 && rf.note.empty()) rf.note = "unresolved";
      rl.fields[f.name] = rf;
    }
    res_.lists.push_back(std::move(rl));
  }

  // ---- camera ----
  auto cam_field = [&](const FieldSpec& f, ResolvedField& out) {
    if (f.name.empty()) return;
    out.addr = resolve_field(nullptr, f, out, 0);
    if (out.addr == 0) {
      out.health = Health::kDead;
      if (out.note.empty()) out.note = "unresolved";
    } else if (out.health != Health::kStale) {
      out.health = Health::kOk;
    }
  };
  cam_field(d_.camera.view, res_.camera_view);
  cam_field(d_.camera.pos, res_.camera_pos);
  cam_field(d_.camera.fov, res_.camera_fov);
  res_.camera_health = (res_.camera_view.health == Health::kOk) ? Health::kOk : res_.camera_view.health;

  return res_;
}

uintptr_t Resolver::resolve_array(const ListSpec& ls, ResolvedList& out) {
  // Schema-driven first: the target tells us where its own entity table is, and
  // that survives renumbering in a way a hardcoded address never does.
  if (!ls.count_schema.empty()) {
    SchemaEntry e;
    if (lookup_schema(ls.count_schema, e) && e.array) {
      out.stride = e.stride;
      out.note = "schema \"" + ls.count_schema + "\"";
      return e.array;
    }
    out.note = "schema \"" + ls.count_schema + "\" not published by target";
  }

  if (ls.array_sig.empty()) {
    if (out.note.empty()) out.note = "no array_sig and no usable schema";
    return 0;
  }

  const ResolvedModule* m = module(ls.module);
  if (!m || m->base == 0) {
    out.note = "module \"" + ls.module + "\" not available";
    return 0;
  }
  const std::vector<size_t>* hits = cached_scan(ls.module, ls.array_sig);
  if (!hits || hits->empty()) {
    out.note = "array_sig found 0 matches in " + ls.module;
    return 0;
  }
  if (hits->size() > 1) {
    out.note = "array_sig is ambiguous (" + std::to_string(hits->size()) +
               " matches) - add wildcards or a match index";
    out.health = Health::kStale;
  }
  out.stride = 0;  // filled in from the field strides by the runtime
  return m->base + hits->front() + static_cast<uintptr_t>(ls.array_addend);
}

bool Resolver::resolve_count(const ListSpec& ls, uintptr_t array, int64_t& count,
                             std::string& note) {
  if (!ls.count_schema.empty()) {
    SchemaEntry e;
    if (lookup_schema(ls.count_schema, e) && e.count_ptr) {
      uint32_t n = 0;
      if (!mem_.read(e.count_ptr, &n, 4)) {
        note = "count cell unreadable";
        return false;
      }
      count = n;
      return true;
    }
  }
  if (ls.count_offset != 0 || !ls.count_schema.empty()) {
    uint32_t n = 0;
    uintptr_t cell = array + static_cast<uintptr_t>(ls.count_offset);
    for (int i = 0; i < ls.count_deref; ++i) {
      if (!mem_.read(cell, &cell, 8)) {
        note = "count deref " + std::to_string(i) + " failed";
        return false;
      }
    }
    if (mem_.read(cell, &n, 4)) {
      count = n;
      return true;
    }
  }
  note = "no count source";
  return false;
}

uintptr_t Resolver::resolve_field(const ListSpec* list, const FieldSpec& f, ResolvedField& out,
                                  uintptr_t base_ctx) {
  out = ResolvedField{};
  uintptr_t addr = 0;

  switch (f.kind) {
    case LocKind::kSchema: {
      SchemaEntry e;
      if (!lookup_schema(f.schema, e) || !e.array) {
        out.note = "schema \"" + f.schema + "\" not published by target";
        return 0;
      }
      addr = e.array;
      out.health = Health::kOk;
      break;
    }
    case LocKind::kStatic: {
      const ResolvedModule* m = module(f.module);
      if (!m || m->base == 0) {
        out.note = "module \"" + f.module + "\" not available";
        return 0;
      }
      addr = m->base + static_cast<uintptr_t>(f.offset);
      out.health = Health::kOk;
      break;
    }
    case LocKind::kSig: {
      const ResolvedModule* m = module(f.module);
      if (!m || m->base == 0) {
        out.note = "module \"" + f.module + "\" not available";
        return 0;
      }
      const std::vector<size_t>* hits = cached_scan(f.module, f.sig);
      out.sig_hits = hits ? hits->size() : 0;
      if (!hits || hits->empty()) {
        out.note = "signature found 0 matches in " + f.module;
        return 0;
      }
      if (f.sig_index >= static_cast<int>(hits->size())) {
        out.note = "match index " + std::to_string(f.sig_index) + " out of range (" +
                   std::to_string(hits->size()) + " matches)";
        return 0;
      }
      if (hits->size() > 1 && f.sig_index == 0) {
        out.health = Health::kStale;
        out.note = "ambiguous: " + std::to_string(hits->size()) + " matches, taking first";
      }
      addr = m->base + (*hits)[f.sig_index] + static_cast<uintptr_t>(f.offset);
      if (out.health == Health::kDead) out.health = Health::kOk;
      break;
    }
    case LocKind::kRip: {
      const ResolvedModule* m = module(f.module);
      if (!m || m->base == 0) {
        out.note = "module \"" + f.module + "\" not available";
        return 0;
      }
      const std::vector<size_t>* hits = cached_scan(f.module, f.sig);
      out.sig_hits = hits ? hits->size() : 0;
      if (!hits || hits->empty()) {
        out.note = "signature found 0 matches in " + f.module;
        return 0;
      }
      if (f.sig_index >= static_cast<int>(hits->size())) {
        out.note = "match index out of range";
        return 0;
      }
      const uintptr_t instr = m->base + (*hits)[f.sig_index];
      int rip_off = 3;
      if (!f.rip_offset.empty()) rip_off = static_cast<int>(std::strtol(f.rip_offset.c_str(), nullptr, 0));
      uintptr_t target = 0;
      if (!rip_dispatch(mem_, instr, rip_off, target)) {
        out.note = "RIP disp32 at " + f.rip_offset + " unreadable";
        return 0;
      }
      addr = target + static_cast<uintptr_t>(f.offset);
      out.health = Health::kOk;
      if (hits->size() > 1) {
        out.health = Health::kStale;
        out.note = "ambiguous: " + std::to_string(hits->size()) + " matches";
      }
      break;
    }
    case LocKind::kPointer: {
      // base_ctx is the entity array base; a field at offset N of a struct is
      // just pointer + N, and the deref count is the hop chain.
      //
      // A pointer field that names a schema resolves against the target's
      // published table instead of the entity array - that is how a struct
      // like the camera, which is not an array element, is reached.
      if (!f.schema.empty()) {
        SchemaEntry e;
        if (!lookup_schema(f.schema, e) || !e.array) {
          out.note = "schema \"" + f.schema + "\" not published by target";
          return 0;
        }
        addr = e.array + static_cast<uintptr_t>(f.offset);
        for (int i = 0; i < f.deref; ++i) {
          uintptr_t next = 0;
          if (!mem_.read(addr, &next, 8)) {
            out.note = "deref " + std::to_string(i) + " failed";
            return 0;
          }
          addr = next;
        }
        out.health = Health::kOk;
        return addr;
      }
      if (base_ctx == 0) base_ctx = module_base_.count(lower(f.module))
                                     ? module_base_[lower(f.module)]
                                     : 0;
      addr = base_ctx + static_cast<uintptr_t>(f.offset);
      for (int i = 0; i < f.deref; ++i) {
        uintptr_t next = 0;
        if (!mem_.read(addr, &next, 8)) {
          out.note = "deref " + std::to_string(i) + " failed at 0x" + std::to_string(addr);
          return 0;
        }
        addr = next;
        if (addr == 0) {
          out.note = "null pointer after " + std::to_string(i + 1) + " deref(s)";
          return 0;
        }
      }
      out.health = Health::kOk;
      break;
    }
    case LocKind::kMapRead:
    case LocKind::kComputed: {
      out.note = "location kind not implemented in this build";
      return 0;
    }
  }
  return addr;
}

}  // namespace lens
