#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/pattern.h"

namespace lens {

std::string Value::to_string() const {
  char buf[64];
  switch (t) {
    case T::kInt:
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i));
      return buf;
    case T::kFloat:
      std::snprintf(buf, sizeof(buf), "%.3f", f);
      return buf;
    case T::kVec3:
      std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", v3.x, v3.y, v3.z);
      return buf;
    case T::kMat4:
      std::snprintf(buf, sizeof(buf), "[%.2f %.2f %.2f | %.2f]", m4.m[0][0], m4.m[0][1],
                    m4.m[0][2], m4.m[0][3]);
      return buf;
    case T::kStr:
      return s;
    case T::kPtr:
      std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(p));
      return buf;
    default:
      return "-";
  }
}

Runtime::Runtime(std::unique_ptr<IMemorySource> mem, const Descriptor& d)
    : mem_(std::move(mem)), d_(d) {
  res_ = std::make_unique<Resolver>(*mem_, d_);
  // Resolve the module anchors first (the schema table lives in a module), then
  // pull the target's own schema, then resolve the lists and camera against a
  // populated schema map. This ordering is what makes the schema route work on
  // the very first attach with no extra call from the caller.
  res_->resolve();
  pull_schema();
  res_->resolve();
  install_bounds();
  by_list_.resize(d_.lists.size());
}

void Runtime::install_bounds() {
  // The declared extents the read path may not leave: each module image, each
  // resolved entity array (base + count * stride), and each schema-published
  // struct. A field that resolves outside all of them is a descriptor bug and
  // is reported dead rather than read.
  mem_->clear_bounds();
  for (const auto& m : res_->resolution().modules)
    if (m.base) mem_->add_bound({"module:" + m.name, m.base, m.size});

  const Resolution& r = res_->resolution();
  for (size_t li = 0; li < d_.lists.size() && li < r.lists.size(); ++li) {
    const ResolvedList& rl = r.lists[li];
    if (rl.array == 0) continue;
    const int64_t stride = stride_of(d_.lists[li]);
    int64_t count = d_.lists[li].max;
    if (!d_.lists[li].count_schema.empty()) {
      Resolver::SchemaEntry e;
      if (res_->lookup_schema(d_.lists[li].count_schema, e) && e.count_ptr) {
        uint32_t n = 0;
        if (mem_->read(e.count_ptr, &n, 4)) count = n;
      }
    }
    count = std::min<int64_t>(count, d_.lists[li].max);
    // Pad by one stride so a read that legitimately runs to the end of the last
    // element (a NUL-terminated string flush against the array end) is inside
    // the bound rather than one byte past it.
    mem_->add_bound({"list:" + rl.name, rl.array,
                     static_cast<size_t>(count * stride + stride)});
    LDEBUG("bound list:%s base=0x%llx size=0x%llx (count=%lld stride=%lld)", rl.name.c_str(),
           (unsigned long long)rl.array, (unsigned long long)(count * stride + stride),
           (long long)count, (long long)stride);
  }
  // Schema-published structs (the camera and anything else the target names).
  for (const auto& kv : res_->schema_entries()) {
    if (kv.second.array == 0) continue;
    mem_->add_bound({"schema:" + kv.first, kv.second.array, 4096});
    LDEBUG("bound schema:%s base=0x%llx size=0x1000", kv.first.c_str(),
           (unsigned long long)kv.second.array);
  }
}

const std::vector<Entity>& Runtime::list(const std::string& name) const {
  static const std::vector<Entity> kEmpty;
  for (size_t i = 0; i < d_.lists.size(); ++i)
    if (d_.lists[i].name == name) return by_list_[i];
  return kEmpty;
}

const Entity* Runtime::local_entity() const {
  for (const auto& e : entities_)
    if (e.is_local) return &e;
  return nullptr;
}

void Runtime::set_viewport(uint32_t w, uint32_t h) {
  vp_w_ = w ? w : 1;
  vp_h_ = h ? h : 1;
}

bool Runtime::pull_schema() {
  // Read the target's own schema table if the descriptor declared one. This is
  // the whole point of the schema route: the target tells us where its tables
  // and their live counts are, and the descriptor only names them. Nothing here
  // is engine-specific, and a target that renumbers its internals between
  // builds still resolves, because we never held a baked index.
  const SchemaTableSpec& st = d_.schema_table;
  if (st.ptr_sig.empty() || st.entry_stride <= 0) return false;

  // Locate the pointer cell: scan the module for the sig, follow the
  // RIP-relative disp32, then read the pointer it holds.
  const ResolvedModule* m = res_->module(st.module);
  if (!m || m->base == 0) {
    LWARN("schema_table: module %s not available", st.module.c_str());
    return false;
  }
  Signature sig;
  try {
    sig = Signature::parse(st.ptr_sig);
  } catch (const std::exception& e) {
    LERROR("schema_table: %s", e.what());
    return false;
  }
  const auto hits = sig.scan(*mem_, m->base, m->size, 1, 64);
  if (hits.size() <= static_cast<size_t>(st.match)) {
    LWARN("schema_table: ptr_sig found %zu matches, need index %d", hits.size(), st.match);
    return false;
  }
  const uintptr_t instr = m->base + hits[st.match];
  int32_t disp = 0;
  if (!mem_->read(instr + st.rip_offset, &disp, 4)) {
    LWARN("schema_table: disp32 unreadable");
    return false;
  }
  const uintptr_t cell = (instr + st.rip_offset + 4) + static_cast<int64_t>(disp);
  uintptr_t table = 0;
  if (!mem_->read(cell, &table, 8) || table == 0) {
    LWARN("schema_table: pointer cell at 0x%llx is null", (unsigned long long)cell);
    return false;
  }

  // Read entries until one looks implausible. Each entry is a fixed stride; the
  // array address and count pointer live at declared offsets within it.
  int installed = 0;
  for (int i = 0; i < st.max_entries; ++i) {
    const uintptr_t e = table + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(st.entry_stride);

    Resolver::SchemaEntry se;
    bool ok = true;
    if (st.array_off >= 0) ok = ok && mem_->read(e + st.array_off, &se.array, 8);
    if (st.countptr_off >= 0) ok = ok && mem_->read(e + st.countptr_off, &se.count_ptr, 8);
    if (st.stride_off >= 0) {
      uint32_t stride = 0;
      ok = ok && mem_->read(e + st.stride_off, &stride, sizeof(stride));
      se.stride = stride;
    }
    if (st.count_off >= 0) {
      uint32_t n = 0;
      if (mem_->read(e + st.count_off, &n, sizeof(n))) se.count = n;
    }
    LDEBUG("schema entry %d @0x%llx ok=%d array=0x%llx countptr=0x%llx stride=%lld count=%lld",
           i, (unsigned long long)e, (int)ok, (unsigned long long)se.array,
           (unsigned long long)se.count_ptr, (long long)se.stride, (long long)se.count);
    if (st.name_off >= 0) {
      uint32_t name = 0;
      ok = ok && mem_->read(e + st.name_off, &name, sizeof(name));
      if (!ok) break;
      // Publish under a stable string key. A count of 0 with a null array ends
      // the table: that is the terminator the target wrote.
      std::string key;
      if (st.name_str_off >= 0) {
        const int cap = st.name_str_max > 0 ? st.name_str_max : 31;
        char nb[40];
        const bool nok = mem_->read(e + st.name_str_off, nb, static_cast<size_t>(cap + 1));
        nb[cap] = '\0';
        LDEBUG("schema name read ok=%d name=\"%s\"", (int)nok, nb);
        if (!nok) break;
        key = std::string(nb);
        if (key.empty()) break;  // end of the table
      } else {
        key = st.publish_as + "[" + std::to_string(name) + "]";
      }
      se.valid = se.array != 0;
      res_->install_schema(key, se);
      ++installed;
      if (se.array == 0) break;  // terminator entry
      continue;
    }
    if (!ok) break;
  }

  if (installed == 0) {
    LWARN("schema_table: no entries installed from 0x%llx", (unsigned long long)table);
    return false;
  }
  LINFO("schema_table: %d entries from %s", installed, st.module.c_str());
  return true;
}

bool Runtime::read_value(uintptr_t addr, const FieldSpec& f, Value& out) const {
  out = Value{};
  if (addr == 0) return false;
  // Refuse a read that leaves every declared extent. A field offset that has
  // drifted past the end of its struct is a dead field, not a value.
  if (!mem_->in_bounds(addr, 1)) return false;

  const std::string& ty = f.type;
  if (ty == "i8") {
    int8_t v = 0;
    if (!mem_->read(addr, &v, 1)) return false;
    out.t = Value::T::kInt; out.i = v; return true;
  }
  if (ty == "u8" || ty == "bool") {
    uint8_t v = 0;
    if (!mem_->read(addr, &v, 1)) return false;
    out.t = Value::T::kInt; out.i = (ty == "bool") ? (v ? 1 : 0) : v; return true;
  }
  if (ty == "i16") {
    int16_t v = 0;
    if (!mem_->read(addr, &v, 2)) return false;
    out.t = Value::T::kInt; out.i = v; return true;
  }
  if (ty == "i32") {
    int32_t v = 0;
    if (!mem_->read(addr, &v, 4)) return false;
    out.t = Value::T::kInt; out.i = v; return true;
  }
  if (ty == "u32" || ty == "hash") {
    uint32_t v = 0;
    if (!mem_->read(addr, &v, 4)) return false;
    out.t = Value::T::kInt; out.i = v; return true;
  }
  if (ty == "i64" || ty == "u64" || ty == "ptr") {
    uint64_t v = 0;
    if (!mem_->read(addr, &v, 8)) return false;
    out.t = (ty == "ptr") ? Value::T::kPtr : Value::T::kInt;
    out.i = static_cast<int64_t>(v);
    out.p = static_cast<uintptr_t>(v);
    return true;
  }
  if (ty == "f32") {
    float v = 0;
    if (!mem_->read(addr, &v, 4)) return false;
    out.t = Value::T::kFloat; out.f = v; return true;
  }
  if (ty == "f32x3") {
    float v[3] = {0, 0, 0};
    if (!mem_->read(addr, v, 12)) return false;
    out.t = Value::T::kVec3; out.v3 = {v[0], v[1], v[2]}; return true;
  }
  if (ty == "f32x4") {
    float v[4] = {0, 0, 0, 0};
    if (!mem_->read(addr, v, 16)) return false;
    out.t = Value::T::kVec3; out.v3 = {v[0], v[1], v[2]}; return true;
  }
  if (ty == "mat4") {
    if (!mem_->read(addr, out.m4.m, 64)) return false;
    out.t = Value::T::kMat4; return true;
  }
  if (ty == "str" || ty == "cstr") {
    const size_t cap = static_cast<size_t>(f.count > 0 ? f.count : 32);
    // A string read must stay inside its declared extent: the last entity's
    // name sits flush against the end of the array, so an over-read by one
    // byte would step outside the bound the descriptor installed.
    const size_t want = std::min(cap + 1, cap);
    char* buf = new char[cap + 1];
    if (!mem_->in_bounds(addr, want) || !mem_->read(addr, buf, want)) {
      delete[] buf;
      return false;
    }
    buf[want] = '\0';
    out.t = Value::T::kStr;
    out.s = buf;
    delete[] buf;
    return true;
  }
  // Array of scalars, e.g. bones: f32x3 with count N.
  if (f.count > 1) {
    const size_t esz = (ty == "f32") ? 4 : 4;
    std::vector<float> buf(static_cast<size_t>(f.count) * (esz / 4));
    if (!mem_->read(addr, buf.data(), buf.size() * 4)) return false;
    out.t = Value::T::kVec3;
    if (buf.size() >= 3) out.v3 = {buf[0], buf[1], buf[2]};
    return true;
  }
  return false;
}

int64_t Runtime::stride_of(const ListSpec& ls) const {
  // A schema-driven list carries the target's own stride; a signature-driven
  // one derives it from the field layout the descriptor declared.
  if (!ls.count_schema.empty()) {
    Resolver::SchemaEntry e;
    if (res_->lookup_schema(ls.count_schema, e) && e.stride) return e.stride;
  }
  int64_t widest = 0;
  for (const auto& f : ls.fields) {
    int64_t sz = 4;
    if (f.type == "i64" || f.type == "u64" || f.type == "ptr" || f.type == "mat4") sz = f.type == "mat4" ? 64 : 8;
    else if (f.type == "f32x3") sz = 12;
    else if (f.type == "f32x4" || f.type == "str" || f.type == "cstr") sz = 16;
    else if (f.type == "i16") sz = 2;
    if (f.count > 1) sz = static_cast<int64_t>(f.count) * 4;
    widest = std::max(widest, sz + f.offset);
  }
  return widest ? widest : 4;
}

void Runtime::read_camera() {
  const Resolution& r = res_->resolution();
  if (r.camera_view.health == Health::kOk && r.camera_view.addr) {
    Value v;
    if (read_value(r.camera_view.addr, d_.camera.view, v) && v.t == Value::T::kMat4) vp_ = v.m4;
  } else if (d_.camera.view.name.empty()) {
    // No camera declared: synthesise one from the eye position and fov so the
    // 3D features still have a projection to work with.
    Value p, f;
    if (r.camera_pos.health == Health::kOk && read_value(r.camera_pos.addr, d_.camera.pos, p) &&
        p.t == Value::T::kVec3) {
      eye_ = p.v3;
      const Mat4 view = mat_view_from_pos(eye_, Vec3{0.f, eye_.y, 0.f}, Vec3{0.f, 1.f, 0.f});
      const Mat4 proj = mat_perspective(fov_deg_ * 3.14159265f / 180.f,
                                         static_cast<float>(vp_w_) / vp_h_, 0.1f, 2000.f);
      vp_ = mat_mul(proj, view);
    }
  }
  if (r.camera_pos.health == Health::kOk) {
    Value p;
    if (read_value(r.camera_pos.addr, d_.camera.pos, p) && p.t == Value::T::kVec3) eye_ = p.v3;
  }
  if (r.camera_fov.health == Health::kOk) {
    Value f;
    if (read_value(r.camera_fov.addr, d_.camera.fov, f) && f.t == Value::T::kFloat)
      fov_deg_ = f.f;
  }
}

void Runtime::enumerate(const ResolvedList& rl, const ListSpec& ls, std::vector<Entity>& into) {
  into.clear();
  if (rl.array == 0) return;

  int64_t count = 0;
  std::string note;
  // Count first: schema count pointer if the target publishes one, else the
  // descriptor's count cell, else fall back to a bounded walk that stops at the
  // first implausible record.
  bool have_count = false;
  if (!ls.count_schema.empty()) {
    Resolver::SchemaEntry e;
    if (res_->lookup_schema(ls.count_schema, e) && e.count_ptr) {
      uint32_t n = 0;
      if (mem_->read(e.count_ptr, &n, 4)) { count = n; have_count = true; }
    }
  }
  if (!have_count && ls.count_offset != 0) {
    uintptr_t cell = rl.array + static_cast<uintptr_t>(ls.count_offset);
    for (int i = 0; i < ls.count_deref; ++i)
      if (!mem_->read(cell, &cell, 8)) { cell = 0; break; }
    if (cell) {
      uint32_t n = 0;
      if (mem_->read(cell, &n, 4)) { count = n; have_count = true; }
    }
  }
  if (!have_count) {
    // No count: walk until a record fails to read or the clamp trips.
    count = ls.max;
  }
  count = std::min<int64_t>(count, ls.max);

  const int64_t stride = stride_of(ls);
  const std::string& local_field = d_.local_field;

  for (int64_t i = 0; i < count; ++i) {
    const uintptr_t addr = rl.array + static_cast<uintptr_t>(i * stride);

    Entity e;
    e.addr = addr;
    e.index = static_cast<int>(i);
    e.frame = stats_.frame;
    e.fields.reserve(ls.fields.size());

    bool any_read = false;
    for (const auto& f : ls.fields) {
      const ResolvedField* rf = nullptr;
      for (const auto& kv : rl.fields)
        if (kv.first == f.name) { rf = &kv.second; break; }
      if (!rf || rf->health == Health::kDead || rf->addr == 0) continue;

      const uintptr_t faddr = rf->addr + static_cast<uintptr_t>(i * stride);
      Value v;
      if (!read_value(faddr, f, v)) continue;
      any_read = true;
      e.fields.emplace_back(f.name, std::move(v));
    }
    if (!any_read && have_count) continue;  // slot exists but nothing readable

    // Pull the well-known fields out of the bag by name.
    for (const auto& kv : e.fields) {
      if (kv.first == "pos" || kv.first == "origin") { e.world = kv.second.v3; break; }
    }
    // Aliveness. Most engines expose it as a float health or as a life-state
    // int; read whichever the descriptor actually declared, and read a float
    // through its float member (the int member of a float value is unset and
    // would mark everyone dead).
    bool have_alive_field = false;
    bool alive_flag = true;
    float health_f = 0.f;
    int64_t health_i = 0;
    bool have_health = false;
    for (const auto& kv : e.fields) {
      if (kv.first == "health") {
        have_health = true;
        if (kv.second.t == Value::T::kFloat) health_f = kv.second.f;
        else health_i = kv.second.i;
      } else if (kv.first == "alive" || kv.first == "life_state") {
        have_alive_field = true;
        alive_flag = kv.second.i != 0;
      }
    }
    if (have_alive_field) e.alive = alive_flag;
    else if (have_health) {
      const bool hp_is_float = [&] {
        for (const auto& kv : e.fields)
          if (kv.first == "health") return kv.second.t == Value::T::kFloat;
        return false;
      }();
      e.alive = hp_is_float ? (health_f > 0.f) : (health_i > 0);
    } else {
      e.alive = true;
    }
    if (!local_field.empty()) {
      for (const auto& kv : e.fields) {
        if (kv.first == local_field) { e.is_local = (kv.second.i != 0); break; }
      }
    }
    e.distance = vec_len(vec_sub(e.world, eye_));
    e.visible = world_to_screen(vp_, e.world, static_cast<float>(vp_w_), static_cast<float>(vp_h_),
                                e.screen);
    into.push_back(std::move(e));
  }
}

const FrameStats& Runtime::tick() {
  const auto t0 = std::chrono::steady_clock::now();
  stats_.frame++;
  stats_.frame = stats_.frame;

  read_camera();

  entities_.clear();
  by_list_.clear();
  by_list_.resize(d_.lists.size());

  // by_list_ owns the storage; entities_ is the flattened view the features
  // read. Copied, not moved, so list(name) and entities() stay valid together
  // for the whole frame.
  for (size_t li = 0; li < d_.lists.size(); ++li) {
    const ResolvedList& rl = res_->resolution().lists[li];
    enumerate(rl, d_.lists[li], by_list_[li]);
    for (auto& e : by_list_[li]) {
      e.frame = stats_.frame;
      entities_.push_back(e);
    }
  }

  stats_.entity_count = static_cast<int>(entities_.size());
  const Resolution& r = res_->resolution();
  stats_.dead_fields = r.dead_fields();
  stats_.stale_fields = r.stale_fields();

  const auto t1 = std::chrono::steady_clock::now();
  stats_.read_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
  fps_acc_ += stats_.read_ms;
  if (++fps_frames_ >= 20) {
    stats_.fps = 1000.f / (fps_acc_ / fps_frames_);
    fps_acc_ = 0.f;
    fps_frames_ = 0;
  }
  return stats_;
}

bool Runtime::read_field(const Entity& e, const std::string& field, Value& out) const {
  for (const auto& kv : e.fields)
    if (kv.first == field) { out = kv.second; return true; }
  return false;
}

bool Runtime::write_field(const Entity& e, const std::string& field, const Value& v,
                          std::string* err) {
  if (!mem_->allowWrite()) {
    if (err) *err = "transport is read-only (start lens with --write)";
    return false;
  }
  // Find the field spec: the descriptor decides what is writable, not the UI.
  const ListSpec* ls = nullptr;
  const FieldSpec* fs = nullptr;
  for (const auto& l : d_.lists) {
    for (const auto& f : l.fields)
      if (f.name == field) { ls = &l; fs = &f; break; }
    if (fs) break;
  }
  if (!fs) {
    if (err) *err = "no such field \"" + field + "\"";
    return false;
  }
  if (!fs->writable) {
    if (err) *err = "field \"" + field + "\" is not marked writable in the descriptor";
    return false;
  }

  // Locate the entity inside its list to get the right record base.
  const ResolvedList* rl = nullptr;
  for (const auto& r : res_->resolution().lists)
    if (r.name == ls->name) { rl = &r; break; }
  if (!rl) { if (err) *err = "list not resolved"; return false; }
  auto it = rl->fields.find(field);
  if (it == rl->fields.end() || it->second.addr == 0) {
    if (err) *err = "field did not resolve";
    return false;
  }

  const int64_t stride = stride_of(*ls);
  int idx = 0;
  for (const auto& ent : entities_)
    if (ent.addr == e.addr) { idx = ent.index; break; }
  const uintptr_t addr = it->second.addr + static_cast<uintptr_t>(idx * stride);

  bool ok = false;
  if (v.t == Value::T::kInt || v.t == Value::T::kPtr) {
    ok = mem_->write(addr, &v.i, 8);
    if (!ok && fs->type != "i64" && fs->type != "u64" && fs->type != "ptr")
      ok = mem_->write(addr, &v.i, 4);
  } else if (v.t == Value::T::kFloat || v.t == Value::T::kVec3) {
    ok = mem_->write(addr, v.t == Value::T::kFloat ? (void*)&v.f : (void*)&v.v3,
                     v.t == Value::T::kFloat ? 4 : 12);
  } else if (v.t == Value::T::kStr) {
    ok = mem_->write(addr, v.s.c_str(), v.s.size() + 1);
  }
  if (!ok && err) *err = "write refused by transport";
  return ok;
}

bool Runtime::poke_static(const std::string& module, int64_t offset, const Value& v,
                          std::string* err) {
  if (!mem_->allowWrite()) {
    if (err) *err = "transport is read-only (start lens with --write)";
    return false;
  }
  const ResolvedModule* m = res_->module(module);
  if (!m || m->base == 0) { if (err) *err = "module not available"; return false; }
  const uintptr_t addr = m->base + static_cast<uintptr_t>(offset);
  if (v.t == Value::T::kFloat) return mem_->write(addr, &v.f, 4);
  if (v.t == Value::T::kInt || v.t == Value::T::kPtr) {
    if (mem_->write(addr, &v.i, 8)) return true;
    return mem_->write(addr, &v.i, 4);
  }
  if (err) *err = "unsupported value type for a static poke";
  return false;
}

}  // namespace lens
