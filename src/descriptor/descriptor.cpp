#include "descriptor.h"

#include <stdexcept>

#include "core/pattern.h"
#include "core/yaml.h"

namespace lens {
namespace {

// Parse now, so a bad pattern is a load-time error with the field name in the
// message rather than a silent miss on frame one.
void validate_sig(const std::string& field_name, const std::string& sig) {
  try {
    (void)Signature::parse(sig);
  } catch (const std::exception& e) {
    throw std::runtime_error("descriptor: field \"" + field_name + "\": " + e.what());
  }
}

LocKind parse_kind(const std::string& s) {
  if (s == "static") return LocKind::kStatic;
  if (s == "sig") return LocKind::kSig;
  if (s == "schema") return LocKind::kSchema;
  if (s == "rip") return LocKind::kRip;
  if (s == "pointer") return LocKind::kPointer;
  if (s == "map") return LocKind::kMapRead;
  if (s == "computed") return LocKind::kComputed;
  throw std::runtime_error("descriptor: unknown location kind \"" + s + "\"");
}

FieldSpec field_from(const YNode& n) {
  FieldSpec f;
  f.name = n.str("name");
  if (f.name.empty()) throw std::runtime_error("descriptor: field is missing 'name'");
  f.kind = parse_kind(n.str("kind", "static"));

  f.module = n.str("module");
  f.offset = n.i64("offset", 0);
  f.sig = n.str("sig");
  f.sig_index = static_cast<int>(n.i64("match", 0));
  f.rip_offset = n.str("rip_offset");
  f.schema = n.str("schema");
  f.deref = n.i64("deref", 0);
  f.type = n.str("type", "i32");
  f.count = static_cast<int>(n.i64("count", 1));
  f.stride = static_cast<int>(n.i64("stride", 0));
  f.writable = n.boolean("writable", false);
  f.label = n.str("label", f.name);

  if (f.kind == LocKind::kSig || f.kind == LocKind::kRip) {
    if (f.sig.empty())
      throw std::runtime_error("descriptor: field \"" + f.name + "\" needs a 'sig'");
    // Validate the pattern at load time, not on the first frame.
    validate_sig(f.name, f.sig);
  }
  if (f.kind == LocKind::kStatic && f.module.empty())
    throw std::runtime_error("descriptor: static field \"" + f.name + "\" needs a 'module'");
  if (f.count < 1) f.count = 1;
  return f;
}

}  // namespace

Descriptor Descriptor::from_yaml(const std::string& text) {
  const YNode root = YNode::parse(text);

  Descriptor d;
  const YNode* target = root.find("target");
  if (!target) throw std::runtime_error("descriptor: no 'target' section");
  d.name = target->str("name");
  d.engine = target->str("engine", "custom");
  d.process = target->str("process");
  d.transport = target->str("transport");
  d.min_version = static_cast<int>(target->i64("min_build", 0));
  if (d.name.empty()) throw std::runtime_error("descriptor: target.name is required");
  if (d.process.empty()) throw std::runtime_error("descriptor: target.process is required");

  if (const YNode* mods = root.find("modules")) {
    for (const auto& m : mods->seq) {
      ModuleSpec ms;
      ms.name = m.str("name");
      ms.sig = m.str("sig");
      if (ms.name.empty()) continue;
      d.modules.push_back(ms);
    }
  }

  if (const YNode* st = root.find("schema_table")) {
    SchemaTableSpec& s = d.schema_table;
    s.module = st->str("module", d.modules.empty() ? d.process : d.modules[0].name);
    s.ptr_sig = st->str("ptr_sig");
    s.match = static_cast<int>(st->i64("match", 0));
    s.rip_offset = static_cast<int>(st->i64("rip_offset", 3));
    s.entry_stride = static_cast<int>(st->i64("entry_stride", 0));
    s.array_off = static_cast<int>(st->i64("array_off", 0));
    s.count_off = static_cast<int>(st->i64("count_off", -1));
    s.stride_off = static_cast<int>(st->i64("stride_off", -1));
    s.name_off = static_cast<int>(st->i64("name_off", -1));
    s.name_str_off = static_cast<int>(st->i64("name_str_off", -1));
    s.name_str_max = static_cast<int>(st->i64("name_str_max", 31));
    s.countptr_off = static_cast<int>(st->i64("countptr_off", -1));
    s.max_entries = static_cast<int>(st->i64("max_entries", 4096));
    s.publish_as = st->str("publish_as", "schema");
    if (!s.ptr_sig.empty()) validate_sig("schema_table.ptr_sig", s.ptr_sig);
  }

  if (const YNode* lists = root.find("lists")) {
    for (const auto& l : lists->seq) {
      ListSpec ls;
      ls.name = l.str("name");
      ls.module = l.str("module", d.modules.empty() ? d.process : d.modules[0].name);
      ls.array_sig = l.str("array_sig");
      ls.array_addend = l.i64("array_addend", 0);
      ls.count_schema = l.str("count_schema");
      ls.count_offset = l.i64("count_offset", 0);
      ls.count_deref = l.i64("count_deref", 0);
      ls.max = static_cast<int>(l.i64("max", 256));
      if (ls.name.empty()) throw std::runtime_error("descriptor: list is missing 'name'");
      if (const YNode* fs = l.find("fields")) {
        for (const auto& f : fs->seq) ls.fields.push_back(field_from(f));
      }
      if (ls.fields.empty())
        throw std::runtime_error("descriptor: list \"" + ls.name + "\" has no fields");
      d.lists.push_back(std::move(ls));
    }
  }

  if (const YNode* cam = root.find("camera")) {
    d.camera.name = cam->str("name", "camera");
    if (const YNode* v = cam->find("view")) d.camera.view = field_from(*v);
    if (const YNode* p = cam->find("pos")) d.camera.pos = field_from(*p);
    if (const YNode* f = cam->find("fov")) d.camera.fov = field_from(*f);
  }

  d.local_field = root.str("local_field");

  if (d.lists.empty() && d.camera.view.name.empty())
    throw std::runtime_error("descriptor: nothing to inspect (no lists, no camera)");

  return d;
}

Descriptor Descriptor::load(const std::string& path) {
  return from_yaml(read_file(path));
}

bool Descriptor::try_load(const std::string& path, Descriptor& out, std::string* err) {
  try {
    out = load(path);
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

}  // namespace lens
