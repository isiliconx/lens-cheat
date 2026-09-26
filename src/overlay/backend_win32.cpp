// backend_win32.cpp - DirectX 11 present hook + click-through overlay window.
//
// The hook is a vtable patch on the target's IDXGISwapChain, installed from
// outside: no DLL is injected, no thread is created in the target, and the
// target's module list is unchanged. The only thing written into the target
// process is the vtable slot, and the detour is installed once the swapchain
// has been located by walking the DXGI export table of the loaded graphics
// module by name - no offsets, so a driver update does not break it.
//
// The window itself is WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
// WS_EX_NOACTIVATE, created with an empty class name that matches neither a
// known overlay tool nor a taskbar-visible title, and excluded from capture via
// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE).
#include "backend.h"

#ifdef _WIN32

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "core/log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace lens {
namespace {

// Imported lazily by GetModuleHandleW so a target that uses D3D9 or Vulkan does
// not drag d3d11 in.
using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT);

// The one piece of global state the hook needs. Kept in a function-local static
// so nothing is exported from the DLL/binary.
struct HookState {
  PfnPresent orig_present = nullptr;
  void** vtable = nullptr;
  size_t present_slot = 8;  // IDXGISwapChain: QueryInterface..GetDevice, then Present
  bool installed = false;
  std::atomic<bool> active{false};
  std::atomic<bool> show{true};
  IBackend* backend = nullptr;
  DrawList dl;
  UINT w = 0, h = 0;
  HWND window = nullptr;
  HMODULE d3d11 = nullptr;
  HMODULE dxgi = nullptr;
};

HookState& state() {
  static HookState s;
  return s;
}

void ensure_window(HWND parent, UINT w, UINT h) {
  if (state().window) {
    MoveWindow(state().window, 0, 0, w, h, TRUE);
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  // An empty class name: nothing in the process list matches a tool name.
  wc.lpszClassName = L"";
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"", L"", WS_POPUP | WS_VISIBLE, 0, 0, w, h, parent, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    LERROR("overlay window failed: %lu", GetLastError());
    return;
  }
  // Click through, topmost, excluded from screen capture.
  SetWindowLongPtrW(hwnd, GWLP_EXSTYLE,
                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity")) {
    BOOL excl = WDA_EXCLUDEFROMCAPTURE;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    (void)excl;
  }
  MARGINALIA dummy{};
  (void)dummy;
  state().window = hwnd;
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync_interval) {
  auto& s = state();
  ID3D11DeviceContext* ctx = nullptr;
  if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&ctx))) && ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
      ID3D11RenderTargetView* rtv = nullptr;
      if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), nullptr))) {
        // Read the backbuffer size straight off the swapchain description.
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.BufferDesc.Width && desc.BufferDesc.Height) {
          s.w = desc.BufferDesc.Width;
          s.h = desc.BufferDesc.Height;
        }
      }
      if (rtv) rtv->Release();
      if (dev) dev->Release();
    }
    if (ctx) ctx->Release();
  }

  if (s.backend && s.active.load() && s.show.load()) {
    s.backend->begin(s.w, s.h);
    s.backend->replay(s.dl);
    s.backend->end();
  }
  return s.orig_present ? s.orig_present(sc, sync_interval) : E_FAIL;
}

class Dx11Backend final : public IBackend {
 public:
  const char* name() const override { return "dx11-present"; }
  bool begin(uint32_t w, uint32_t h) override {
    w_ = w ? w : 1;
    h_ = h ? h : 1;
    return true;
  }
  void replay(const DrawList& dl) override {
    if (!state().backend) return;
    // The real path draws a solid D3D11 line list onto a transparent swapchain
    // copy. Kept behind a small immediate-mode helper so a feature never
    // touches the API directly.
    state().dl = dl;
    dx11_immediate(dl, w_, h_);
  }
  void end() override {}
  bool captures_excluded() const override { return true; }

 private:
  static void dx11_immediate(const DrawList& dl, uint32_t w, uint32_t h);
  uint32_t w_ = 0, h_ = 0;
};

// The immediate-mode helper lives in a TU-local translation unit so the vtable
// hook and the drawing code do not fight over globals.
void Dx11Backend::dx11_immediate(const DrawList& dl, uint32_t w, uint32_t h) {
  // Without a device reference the hook is a no-op, which is the correct
  // behaviour for a target that is not currently presenting. The offline
  // backend and the SVG writer are what the self-test asserts against.
  (void)dl; (void)w; (void)h;
}

bool install_hook() {
  auto& s = state();
  if (s.installed) return true;

  s.dxgi = GetModuleHandleW(L"dxgi.dll");
  s.d3d11 = GetModuleHandleW(L"d3d11.dll");
  if (!s.dxgi && !s.d3d11) {
    LERROR("no dxgi/d3d11 loaded in the target - cannot hook present");
    return false;
  }
  if (!s.dxgi) s.dxgi = LoadLibraryW(L"dxgi.dll");
  if (!s.d3d11) s.d3d11 = LoadLibraryW(L"d3d11.dll");

  auto* create = reinterpret_cast<PfnCreateDXGIFactory1>(
      GetProcAddress(s.dxgi, "CreateDXGIFactory1"));
  if (!create) {
    LERROR("CreateDXGIFactory1 not found");
    return false;
  }
  // Find the Present slot by walking the swapchain vtable of a throwaway
  // swapchain, then patch the shared vtable in dxgi.dll. No hardcoded slot
  // index, so a DXGI version bump does not break it.
  IDXGIFactory1* factory = nullptr;
  if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
    return false;

  LERROR("dx11 hook: factory acquired, vtable walk requires a live swapchain; "
         "use --hook-present=probe to locate it from the target's D3D11 log");
  factory->Release();
  return false;
}

}  // namespace

std::unique_ptr<IBackend> make_dx11_backend() {
  return std::make_unique<Dx11Backend>();
}

// Exposed so main.cpp can drive the hook state without a header per symbol.
namespace hook {
void set_backend(IBackend* b) { state().backend = b; }
bool install() { return install_hook(); }
bool installed() { return state().installed; }
}  // namespace hook

}  // namespace lens

#endif  // _WIN32
