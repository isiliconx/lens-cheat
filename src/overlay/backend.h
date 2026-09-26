// backend.h - a replayable renderer.
//
// Two implementations ship: an offline PPM writer (so the self-test can assert
// on real pixels with no GPU and no window) and a DirectX 11 present hook. The
// DrawList is the contract between them.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "overlay/drawlist.h"

namespace lens {

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual const char* name() const = 0;
  virtual bool begin(uint32_t w, uint32_t h) = 0;
  virtual void replay(const DrawList& dl) = 0;
  virtual void end() = 0;

  // Set on the DX11 backend from a present hook, ignored elsewhere.
  virtual void present_hook(void* swapchain) { (void)swapchain; }
  virtual bool captures_excluded() const { return false; }
};

// Offline: rasterises into an RGB buffer and writes a binary PPM on end().
// Deterministic, no dependencies, and the same code path as the live overlay.
std::unique_ptr<IBackend> make_offline_backend(const std::string& out_path);

// DirectX 11 present hook. Only meaningful in a real target; the hook installs
// itself with a vtable patch, not an injected DLL, so the target's module list
// is unchanged.
#ifdef _WIN32
std::unique_ptr<IBackend> make_dx11_backend();
#endif

// One frame of an SVG snapshot, for pasting into an issue.
bool write_svg(const DrawList& dl, uint32_t w, uint32_t h, const std::string& path);

}  // namespace lens
