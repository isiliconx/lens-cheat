// descriptor.h - the format that is the product.
//
// A descriptor declares a process to lens. It never hardcodes an address: every
// location is either a signature, a name that the runtime looks up in a schema
// the target itself publishes, or a chain of dereferences from one of those.
// Adapters are data, not code.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lens {

// How a location is found.
enum class LocKind {
  kStatic,   // a fixed offset inside a module's .data
  kSig,      // scan a module for a pattern, apply an addend
  kSchema,   // look a name up in the target's own schema table
  kRip,      // scan, then follow a RIP-relative disp32
  kPointer,  // read a pointer, then chain N dereferences
  kMapRead,  // same as kPointer but the pointer lives in a map-like table
  kComputed, // an expression over already-resolved locations
};

// One field on an entity. Every field is independently resolvable, which is
// what makes a partial break reportable instead of fatal.
struct FieldSpec {
  std::string name;
  LocKind kind = LocKind::kStatic;

  std::string module;    // kStatic / kSig
  int64_t offset = 0;    // kStatic
  std::string sig;       // kSig / kRip
  int sig_index = 0;     // which match to take, for adapters that need match #2
  std::string rip_offset = "";   // "" = use the default (match + pattern tail)

  std::string schema;    // kSchema
  int64_t deref = 0;     // kPointer: pointer hops after the first read

  std::string type = "i32";  // i8 i16 i32 i64 u32 u64 f32 f32x3 f32x4 mat4 str ptr bool
  int count = 1;             // element count for arrays, bone counts
  int stride = 0;            // 0 = tight packing
  bool writable = false;
  std::string label;         // shown in the inspector
};

// The list of entities. Where the array is, how big a stride is, and how to
// find the count.
struct ListSpec {
  std::string name;          // "players"
  std::string module;        // for the count pointer, when not schema-driven
  std::string array_sig;     // signature that yields the array base
  int64_t array_addend = 0;
  std::string count_schema;  // schema name of a uint32 count, when schema-driven
  int64_t count_offset = 0;  // offset of the count cell inside the array header
  int64_t count_deref = 0;
  int max = 256;             // hard clamp, so a corrupt count cannot spin
  std::vector<FieldSpec> fields;
};

struct CameraSpec {
  std::string name = "camera";
  FieldSpec view;    // mat4 world-to-screen
  FieldSpec pos;     // f32x3 eye
  FieldSpec fov;     // f32 degrees
};

struct ModuleSpec {
  std::string name;
  std::string sig;  // optional: signature that must be present in the module
};

// A schema table the target publishes about itself. This is the "ask the engine
// where it put things" route: the descriptor says how to reach the table (a
// signature that yields the pointer, plus the entry layout), and the entries
// come out of the target's own memory. A real engine's indices shift between
// builds; its table still does not, so a descriptor written this way survives
// a patch that would break baked offsets.
struct SchemaTableSpec {
  std::string module;
  // Signature whose RIP-relative disp32 (or direct addend) points at a cell
  // holding a pointer to the first entry. Default rip_offset is 3.
  std::string ptr_sig;
  int match = 0;
  int rip_offset = 3;
  // Entry layout, in bytes.
  int entry_stride = 0;
  int array_off = 0;   // u64: address of the array
  int count_off = 0;   // u32: live element count (informational)
  int stride_off = 0;  // u32: element stride
  int name_off = 0;    // u32: name hash the descriptor refers to
  int countptr_off = 0;// u64: address of the live count cell
  // u32[8]: a NUL-terminated name the descriptor refers to. -1 = absent.
  int name_str_off = -1;
  int name_str_max = 31;
  // How many entries to read before giving up.
  int max_entries = 4096;
  // Publish each entry under this name in the schema map.
  std::string publish_as;
};

struct Descriptor {
  std::string name;        // "cs2"
  std::string engine;      // "source2" | "unreal" | "custom"
  std::string process;     // "cs2.exe"
  std::string transport;   // "win" | "sim"
  int min_version = 0;

  std::vector<ModuleSpec> modules;
  SchemaTableSpec schema_table;  // valid when ptr_sig is non-empty
  std::vector<ListSpec> lists;
  CameraSpec camera;

  std::string local_field;  // field name holding "this is me", or ""

  static Descriptor from_yaml(const std::string& text);   // throws
  static Descriptor load(const std::string& path);        // throws
  static bool try_load(const std::string& path, Descriptor& out, std::string* err);
};

}  // namespace lens
