// sim_layout.h - the contract between lens_sim (the synthetic target) and the
// sim transport in lens.
//
// The sim publishes ONE shared-memory segment that stands in for a whole
// process address space. Fake addresses are handed out from SIM_BASE so the
// client code does pointer arithmetic exactly as it would against a real
// process: signature scan a module image, follow a RIP-relative instruction,
// read a pointer, walk an array.
//
// The code region contains real x86-64 byte sequences built by the sim. The
// descriptor scans for those byte sequences and resolves pointers through the
// same relative-disp32 math the real engine uses, so the resolver path is
// exercised end to end, not stubbed.
#pragma once
#include <cstdint>

#define SIM_SHM_MAGIC   0x4D534E4Cu  // 'LNSM'
#define SIM_BASE        0x7FF600000000ull
#define SIM_SHM_SIZE    (4u << 20)    // 4 MiB

// Region offsets inside the segment.
#define SIM_OFF_HEADER  0x0000u
#define SIM_OFF_TEXT    0x1000u       // module image: code + rodata
#define SIM_OFF_DATA    0x80000u      // heap-ish: schema, entities, camera
#define SIM_OFF_SCRATCH 0xC0000u

// The signature the descriptor scans for, and the layout of the code around
// it. Kept here so the sim and the adapter cannot drift apart.
#define SIM_SIG_BYTES   "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 10 EB 10 90 90 90 90"
#define SIM_SIG_RIP_OFF 3u   // disp32 sits at match+3
#define SIM_SIG_LEN     4u   // disp32 is 4 bytes

#pragma pack(push, 8)

struct SimCamera {
  float viewproj[16];
  float eye[3];
  float _pad;
  float fov_deg;
};

struct SimEntity {
  float origin[3];
  float health;
  int32_t team;
  int32_t alive;
  int32_t weapon;
  int32_t local;          // non-zero on the entity that is "me"
  float bones[24 * 3];
  char name[32];
};

// One entry per "netvar table" the runtime can enumerate. Mirrors the shape a
// schema-driven engine hands out: an array pointer, a live count, a stride, and
// the name the target publishes for it. lens refers to tables by this name, so
// the target is free to renumber them.
struct SimSchemaEntry {
  uint64_t array_addr;   // fake address of SimEntity[cap]
  uint32_t count;        // live entities
  uint32_t stride;       // sizeof(SimEntity)
  uint32_t cap;
  uint32_t name_hash;    // stable id, for diagnostics
  uint64_t array_count_addr;  // fake address of a uint32 holding the count
  char name[32];         // the name lens refers to
};

struct SimHeader {
  uint32_t magic;
  uint32_t version;
  char module_name[64];
  uint64_t module_base;    // fake base the client is told the module loaded at
  uint64_t module_size;
  uint64_t schema_ptr_addr;      // fake address of a uint64 holding schema array addr
  uint64_t schema_array_addr;    // fake address of SimSchemaEntry[]
  uint32_t schema_count;
  uint64_t camera_ptr_addr;      // fake address of a uint64 holding camera addr
  uint64_t camera_addr;          // fake address of SimCamera
  uint64_t local_index_addr;     // fake address of a uint32: our own entity slot
  uint32_t local_index;
  uint64_t tick;
  uint32_t ready;
  uint32_t viewport[2];          // the sim's own window size, for honest scaling
  uint32_t pad[8];
};

#pragma pack(pop)
