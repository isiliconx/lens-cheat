// runtime.h - live read loop.
//
// Ties descriptor + resolver + transport into a per-frame picture: resolve,
// enumerate, read, project. The entity record is a flat typed-value bag so the
// overlay, the inspector table, the script layer and the diff all read the same
// data without a feature knowing anything about the engine.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/matrix.h"
#include "core/mem.h"
#include "descriptor/descriptor.h"
#include "descriptor/resolve.h"

namespace lens {

struct Value {
  enum class T { kNone, kInt, kFloat, kVec3, kMat4, kStr, kPtr };
  T t = T::kNone;
  int64_t i = 0;
  float f = 0.f;
  Vec3 v3{};
  Mat4 m4{};
  std::string s;
  uintptr_t p = 0;

  std::string to_string() const;
};

// One entity, as read this frame.
struct Entity {
  uintptr_t addr = 0;      // the address of the struct itself
  int index = -1;
  bool is_local = false;
  bool visible = false;    // in front of the camera
  Vec2 screen{};           // project() result
  Vec3 world{};
  float distance = 0.f;
  std::vector<std::pair<std::string, Value>> fields;
  bool alive = true;
  uint64_t frame = 0;
};

struct FrameStats {
  uint32_t frame = 0;
  uint64_t tick = 0;        // the target's own tick when it exposes one
  float fps = 0.f;
  float read_ms = 0.f;
  int entity_count = 0;
  int drawn = 0;
  int dead_fields = 0;
  int stale_fields = 0;
};

class Runtime {
 public:
  Runtime(std::unique_ptr<IMemorySource> mem, const Descriptor& d);

  bool ready() const { return mem_ != nullptr; }
  const Descriptor& descriptor() const { return d_; }
  const Resolver& resolver() const { return *res_; }
  Resolution& resolution() { return res_->resolution(); }
  const Resolution& resolution() const { return res_->resolution(); }
  IMemorySource& mem() { return *mem_; }
  const IMemorySource& mem() const { return *mem_; }

  // Pull the target's own schema table, if the descriptor named one. Called on
  // attach and on demand; this is what makes a schema-driven engine work
  // without a single hardcoded address.
  bool pull_schema();

  // One full read pass. Returns the stats it filled in.
  const FrameStats& tick();

  const std::vector<Entity>& entities() const { return entities_; }
  const std::vector<Entity>& list(const std::string& name) const;
  const Entity* local_entity() const;
  const FrameStats& stats() const { return stats_; }
  const Mat4& viewproj() const { return vp_; }
  const Vec3& eye() const { return eye_; }
  float fov() const { return fov_deg_; }
  uint32_t viewport_w() const { return vp_w_; }
  uint32_t viewport_h() const { return vp_h_; }
  void set_viewport(uint32_t w, uint32_t h);

  // Read/write a single named field on a live entity. Write is refused unless
  // the transport was opened with write permission and the field is declared
  // writable in the descriptor.
  bool read_field(const Entity& e, const std::string& field, Value& out) const;
  // Const Entity: a write targets the address, not the read-side record, so a
  // caller holding a const frame can still patch a field.
  bool write_field(const Entity& e, const std::string& field, const Value& v,
                   std::string* err);

  // Change one of the descriptor's own knobs at runtime (local team, fov clamp).
  bool poke_static(const std::string& module, int64_t offset, const Value& v, std::string* err);

 private:
  void read_camera();
  void enumerate(const ResolvedList& rl, const ListSpec& ls, std::vector<Entity>& into);
  bool read_value(uintptr_t addr, const FieldSpec& f, Value& out) const;
  int64_t stride_of(const ListSpec& ls) const;
  void install_bounds();

  std::unique_ptr<IMemorySource> mem_;
  const Descriptor d_;
  std::unique_ptr<Resolver> res_;
  std::vector<Entity> entities_;
  std::vector<std::vector<Entity>> by_list_;
  Mat4 vp_;
  Vec3 eye_{};
  float fov_deg_ = 90.f;
  uint32_t vp_w_ = 1280, vp_h_ = 720;
  FrameStats stats_;
  float fps_acc_ = 0.f;
  int fps_frames_ = 0;
};

}  // namespace lens
