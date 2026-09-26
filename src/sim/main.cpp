// sim/main.cpp - lens_sim, the synthetic live target.
//
// Not a stub: it allocates a segment, lays out a module image containing real
// x86-64 instruction bytes, publishes RIP-relative pointers into its data
// half, spawns a set of entities that walk around, and animates a camera. lens
// then attaches to it over the sim transport and drives the same descriptor,
// the same resolver and the same draw path used against a real process.
//
//   ./lens_sim --shm <name> [--module <name>] [--ticks <n>] [--no-animate]
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "core/matrix.h"
#include "sim/sim_layout.h"

namespace {

using lens::Mat4;
using lens::Vec3;

// Builds the same view*projection pair the descriptor's camera field is read
// with: DirectX-style perspective (z in [0,1]) times a look-at view.
void build_viewproj(::SimCamera& cam, float aspect) {
  const Vec3 eye{cam.eye[0], cam.eye[1], cam.eye[2]};
  const Vec3 target{0.f, 1.2f, 0.f};
  const Vec3 up{0.f, 1.f, 0.f};
  const Mat4 view = lens::mat_view_from_pos(eye, target, up);
  const float fov = cam.fov_deg * 3.14159265358979f / 180.f;
  const Mat4 proj = lens::mat_perspective(fov, aspect, 0.1f, 500.f);
  const Mat4 vp = lens::mat_mul(proj, view);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) cam.viewproj[i * 4 + j] = vp.m[i][j];
}

}  // namespace

int main(int argc, char** argv) {
  std::string shm = "/lens_sim";
  std::string module = "lens_sim_module";
  long ticks = 0;  // 0 = forever
  bool animate = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--shm" && i + 1 < argc) shm = argv[++i];
    else if (a == "--module" && i + 1 < argc) module = argv[++i];
    else if (a == "--ticks" && i + 1 < argc) ticks = std::atol(argv[++i]);
    else if (a == "--no-animate") animate = false;
    else {
      std::fprintf(stderr,
                   "usage: %s --shm <name> [--module <name>] [--ticks <n>] [--no-animate]\n",
                   argv[0]);
      return 2;
    }
  }

  shm_unlink(shm.c_str());
  int fd = shm_open(shm.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    std::perror("shm_open");
    return 1;
  }
  if (ftruncate(fd, SIM_SHM_SIZE) != 0) {
    std::perror("ftruncate");
    close(fd);
    return 1;
  }
  auto* base = static_cast<uint8_t*>(mmap(nullptr, SIM_SHM_SIZE, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd, 0));
  if (base == MAP_FAILED) {
    std::perror("mmap");
    close(fd);
    return 1;
  }
  std::memset(base, 0, SIM_SHM_SIZE);

  auto* H = reinterpret_cast<SimHeader*>(base + SIM_OFF_HEADER);
  H->magic = SIM_SHM_MAGIC;
  H->version = 3;
  std::snprintf(H->module_name, sizeof(H->module_name), "%s", module.c_str());
  H->module_base = SIM_BASE + SIM_OFF_TEXT;
  H->module_size = SIM_OFF_DATA - SIM_OFF_TEXT;
  H->viewport[0] = 1280;
  H->viewport[1] = 720;

  // ---- text region: a real code stub the descriptor signature-scans for ----
  //
  //   48 8B 05 <disp32>      mov  rax, [rip+disp32]   -> schema array
  //   48 85 C0               test rax, rax
  //   74 10                  jz   +0x10
  //   EB 10                  jmp  +0x10
  //   90 90 90 90            nop nop nop nop
  //
  // The disp32 is patched below once the data addresses are known, so the
  // resolver really does have to decode a RIP-relative displacement.
  uint8_t* text = base + SIM_OFF_TEXT;
  static const uint8_t kStub[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85,
                                  0xC0, 0x74, 0x10, 0xEB, 0x10, 0x90, 0x90,
                                  0x90, 0x90};
  uint8_t* stub_at = text + 0x2000;
  std::memcpy(stub_at, kStub, sizeof(kStub));
  // A decoy 3 KiB earlier, so a naive "first match wins" resolver is wrong.
  uint8_t* decoy = text + 0x1000;
  std::memcpy(decoy, kStub, sizeof(kStub));

  // ---- data region ----
  auto* schema = reinterpret_cast<SimSchemaEntry*>(base + SIM_OFF_DATA);
  auto* entities = reinterpret_cast<SimEntity*>(base + SIM_OFF_DATA + 0x10000);
  auto* count_cell = reinterpret_cast<uint32_t*>(base + SIM_OFF_DATA + 0x8000);
  auto* camera = reinterpret_cast<SimCamera*>(base + SIM_OFF_DATA + 0x9000);
  auto* local_cell = reinterpret_cast<uint32_t*>(base + SIM_OFF_DATA + 0xA000);

  const uint64_t kMaxEntities = 16;
  const uint64_t ents_addr = SIM_BASE + SIM_OFF_DATA + 0x10000;

  for (uint64_t i = 0; i < kMaxEntities; ++i) {
    SimEntity& e = entities[i];
    e.health = 100.0f;
    e.team = (i % 2 == 0) ? 2 : 3;
    e.alive = 1;
    e.local = (i == 0) ? 1 : 0;   // entity 0 is the local player
    e.weapon = static_cast<int32_t>(i % 4);
    std::snprintf(e.name, sizeof(e.name), "unit-%02u", static_cast<unsigned>(i));
  }

  // Two schema tables: the enemy roster, then a decoy with a tiny count. A
  // resolver that ignores the descriptor's table name grabs the wrong one.
  // A third, all-zero entry terminates the table.
  schema[0].array_addr = ents_addr;
  schema[0].count = static_cast<uint32_t>(kMaxEntities);
  schema[0].cap = static_cast<uint32_t>(kMaxEntities);
  schema[0].stride = sizeof(SimEntity);
  schema[0].name_hash = 0x9E3779B1u;
  schema[0].array_count_addr = SIM_BASE + SIM_OFF_DATA + 0x8000;
  std::snprintf(schema[0].name, sizeof(schema[0].name), "players");

  schema[1].array_addr = ents_addr;
  schema[1].count = 2;  // decoy table
  schema[1].cap = static_cast<uint32_t>(kMaxEntities);
  schema[1].stride = sizeof(SimEntity);
  schema[1].name_hash = 0x85EBCA6Bu;
  schema[1].array_count_addr = SIM_BASE + SIM_OFF_DATA + 0x8004;
  std::snprintf(schema[1].name, sizeof(schema[1].name), "decoy_roster");

  // The camera, published as a third table so the descriptor reaches it the
  // same way it reaches everything else: by name, through the target's own
  // table. The array address is the SimCamera struct.
  schema[2].array_addr = SIM_BASE + SIM_OFF_DATA + 0x9000;
  schema[2].count = 1;
  schema[2].cap = 1;
  schema[2].stride = sizeof(SimCamera);
  schema[2].name_hash = 0xC2B2AE35u;
  schema[2].array_count_addr = SIM_BASE + SIM_OFF_DATA + 0x9000;
  std::snprintf(schema[2].name, sizeof(schema[2].name), "camera");

  // schema[3] stays zeroed: the empty name is the terminator.

  *count_cell = static_cast<uint32_t>(kMaxEntities);
  *local_cell = 0;

  H->schema_array_addr = SIM_BASE + SIM_OFF_DATA;
  H->schema_count = 4;  // two rosters, the camera, plus the terminator
  H->camera_addr = SIM_BASE + SIM_OFF_DATA + 0x9000;
  H->local_index_addr = SIM_BASE + SIM_OFF_DATA + 0xA000;
  H->local_index = 0;

  // Pointer cells: a uint64 holding the schema array address, and a uint64
  // holding the camera address. The code stub points at the first one.
  auto* schema_ptr = reinterpret_cast<uint64_t*>(base + SIM_OFF_DATA + 0xB000);
  auto* camera_ptr = reinterpret_cast<uint64_t*>(base + SIM_OFF_DATA + 0xB008);
  *schema_ptr = H->schema_array_addr;
  *camera_ptr = H->camera_addr;
  H->schema_ptr_addr = SIM_BASE + SIM_OFF_DATA + 0xB000;
  H->camera_ptr_addr = SIM_BASE + SIM_OFF_DATA + 0xB008;

  // Second stub 0x40 past the first: reads the camera pointer.
  uint8_t* cam_stub = stub_at + 0x40;
  std::memcpy(cam_stub, kStub, sizeof(kStub));

  // Patch disp32 for both stubs, relative to the end of the instruction.
  auto patch = [&](uint8_t* at, uint64_t target) {
    const uint64_t next_rip = (SIM_BASE + (at - base)) + 7;  // +7 = past disp32
    const int32_t disp = static_cast<int32_t>(static_cast<int64_t>(target) -
                                              static_cast<int64_t>(next_rip));
    std::memcpy(at + SIM_SIG_RIP_OFF, &disp, 4);
  };
  patch(stub_at, H->schema_ptr_addr);
  patch(cam_stub, H->camera_ptr_addr);
  // The decoy points somewhere harmless so a wrong resolver reads garbage
  // rather than crashing.
  patch(decoy, SIM_BASE + SIM_OFF_SCRATCH);

  H->tick = 0;
  std::atomic_thread_fence(std::memory_order_release);
  H->ready = 1;

  const double step = animate ? 0.016 : 0.0;
  uint32_t frame = 0;
  while (ticks <= 0 || static_cast<long>(frame) < ticks) {
    const float t = static_cast<float>(frame) * 0.05f;

    for (uint64_t i = 0; i < kMaxEntities; ++i) {
      SimEntity& e = entities[i];
      const float phase = t + static_cast<float>(i) * 0.7f;
      e.origin[0] = std::cos(phase) * (6.f + static_cast<float>(i) * 0.9f);
      e.origin[1] = std::sin(phase * 0.8f) * 2.0f;
      e.origin[2] = std::sin(phase) * (6.f + static_cast<float>(i) * 0.9f);
      e.health = 100.0f - 30.0f * (0.5f + 0.5f * std::sin(phase * 0.35f));

      // A bone table: a simple standing skeleton, 24 joints.
      static const float kY[24] = {0.00f, 0.12f, 0.24f, 0.36f, 0.48f, 0.60f,
                                   0.72f, 0.84f, 0.96f, 0.84f, 0.72f, 0.60f,
                                   0.48f, 0.60f, 0.72f, 0.84f, 0.72f, 0.60f,
                                   0.48f, 0.36f, 0.24f, 0.12f, 0.00f, 0.00f};
      for (int b = 0; b < 24; ++b) {
        const float side = (b % 3 == 0) ? 0.0f : ((b % 3 == 1) ? 0.22f : -0.22f);
        e.bones[b * 3 + 0] = e.origin[0] + side;
        e.bones[b * 3 + 1] = e.origin[1] + kY[b];
        e.bones[b * 3 + 2] = e.origin[2];
      }
    }
    schema[0].count = static_cast<uint32_t>(kMaxEntities);
    *count_cell = static_cast<uint32_t>(kMaxEntities);

    // Camera orbits the arena looking at the centre.
    const float eye_a = t * 0.25f;
    camera->eye[0] = std::cos(eye_a) * 14.f;
    camera->eye[1] = 3.2f;
    camera->eye[2] = std::sin(eye_a) * 14.f;
    camera->fov_deg = 75.0f;
    build_viewproj(*camera, 1280.f / 720.f);

    H->tick = ++frame;
    if (!animate) break;
    usleep(static_cast<useconds_t>(step * 1e6));
  }

  H->ready = 0;
  std::atomic_thread_fence(std::memory_order_release);
  munmap(base, SIM_SHM_SIZE);
  close(fd);
  shm_unlink(shm.c_str());
  return 0;
}
