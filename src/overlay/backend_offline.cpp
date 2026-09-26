// backend_offline.cpp - software rasteriser + PPM writer + SVG writer.
#include "backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "core/output.h"
#include "overlay/font.h"

namespace lens {
namespace {

class OfflineBackend final : public IBackend {
 public:
  explicit OfflineBackend(std::string path) : out_(std::move(path)) {}

  const char* name() const override { return "offline"; }

  bool begin(uint32_t w, uint32_t h) override {
    w_ = w ? w : 1;
    h_ = h ? h : 1;
    fb_.assign(static_cast<size_t>(w_) * h_ * 3, 0);
    // A dark background so a dumped frame is readable.
    for (size_t i = 0; i < fb_.size(); i += 3) {
      fb_[i] = 16; fb_[i + 1] = 18; fb_[i + 2] = 22;
    }
    return true;
  }

  void replay(const DrawList& dl) override {
    for (const PrimCmd& c : dl.commands()) {
      switch (c.kind) {
        case Prim::kLine: line(c.x0, c.y0, c.x1, c.y1, c.color, c.thickness); break;
        case Prim::kRect: rect(c.rect, c.color, true); break;
        case Prim::kRectOutline: rect(c.rect, c.color, false, c.thickness); break;
        case Prim::kCircle: circle(c.cx, c.cy, c.r, c.color, true); break;
        case Prim::kCircleOutline: circle(c.cx, c.cy, c.r, c.color, false, c.thickness); break;
        case Prim::kPolygon: polygon(c.pts, c.color, c.filled, c.thickness); break;
        case Prim::kText: text(c); break;
      }
    }
  }

  void end() override {
    if (out_.empty()) return;
    ensure_parent_dir(out_);
    std::ofstream f(out_, std::ios::binary);
    if (!f) return;
    f << "P6\n" << w_ << " " << h_ << "\n255\n";
    f.write(reinterpret_cast<const char*>(fb_.data()),
            static_cast<std::streamsize>(fb_.size()));
  }

  // Non-black pixel count, so the self-test can assert "something was drawn".
  uint32_t lit_pixels() const {
    uint32_t n = 0;
    for (size_t i = 0; i < fb_.size(); i += 3) {
      const uint8_t bg_r = 16, bg_g = 18, bg_b = 22;
      if (std::abs(static_cast<int>(fb_[i]) - bg_r) > 12 ||
          std::abs(static_cast<int>(fb_[i + 1]) - bg_g) > 12 ||
          std::abs(static_cast<int>(fb_[i + 2]) - bg_b) > 12)
        ++n;
    }
    return n;
  }

  uint32_t width() const { return w_; }
  uint32_t height() const { return h_; }

 private:
  void put(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(w_) || y >= static_cast<int>(h_)) return;
    const size_t o = (static_cast<size_t>(y) * w_ + x) * 3;
    // Simple source-over blend.
    const float a = c.a / 255.f;
    fb_[o] = static_cast<uint8_t>(c.r * a + fb_[o] * (1 - a));
    fb_[o + 1] = static_cast<uint8_t>(c.g * a + fb_[o + 1] * (1 - a));
    fb_[o + 2] = static_cast<uint8_t>(c.b * a + fb_[o + 2] * (1 - a));
  }

  void line(float x0, float y0, float x1, float y1, Color c, float t) {
    const float dx = x1 - x0, dy = y1 - y0;
    const int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy))) + 1;
    const int half = static_cast<int>(std::max(0.f, t * 0.5f));
    for (int i = 0; i <= steps; ++i) {
      const float u = static_cast<float>(i) / steps;
      const int px = static_cast<int>(x0 + dx * u);
      const int py = static_cast<int>(y0 + dy * u);
      for (int oy = -half; oy <= half; ++oy)
        for (int ox = -half; ox <= half; ++ox) put(px + ox, py + oy, c);
    }
  }

  void rect(const Rect& r, Color c, bool filled, float t = 1.f) {
    if (filled) {
      for (int y = static_cast<int>(r.y); y < static_cast<int>(r.y + r.h); ++y)
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.x + r.w); ++x) put(x, y, c);
      return;
    }
    line(r.x, r.y, r.x + r.w, r.y, c, t);
    line(r.x, r.y + r.h, r.x + r.w, r.y + r.h, c, t);
    line(r.x, r.y, r.x, r.y + r.h, c, t);
    line(r.x + r.w, r.y, r.x + r.w, r.y + r.h, c, t);
  }

  void circle(float cx, float cy, float r, Color c, bool filled, float t = 1.f) {
    if (r <= 0) return;
    if (filled) {
      for (int y = static_cast<int>(cy - r); y <= static_cast<int>(cy + r); ++y)
        for (int x = static_cast<int>(cx - r); x <= static_cast<int>(cx + r); ++x)
          if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) put(x, y, c);
      return;
    }
    const int steps = std::max(12, static_cast<int>(r * 8));
    for (int i = 0; i < steps; ++i) {
      const float a0 = 6.2831853f * i / steps;
      const float a1 = 6.2831853f * (i + 1) / steps;
      line(cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r,
           cy + std::sin(a1) * r, c, t);
    }
  }

  void polygon(const std::vector<float>& pts, Color c, bool filled, float t) {
    if (pts.size() < 6) return;
    if (filled) {
      // Scanline over the bounding box.
      float min_y = pts[1], max_y = pts[1];
      for (size_t i = 1; i < pts.size(); i += 2) {
        min_y = std::min(min_y, pts[i]);
        max_y = std::max(max_y, pts[i]);
      }
      for (int y = static_cast<int>(min_y); y <= static_cast<int>(max_y); ++y) {
        std::vector<float> xs;
        for (size_t i = 0; i + 3 < pts.size(); i += 2) {
          const float y0 = pts[i + 1], y1 = pts[i + 3];
          if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
            const float u = (y - y0) / (y1 - y0);
            xs.push_back(pts[i] + (pts[i + 2] - pts[i]) * u);
          }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
          for (int x = static_cast<int>(xs[i]); x <= static_cast<int>(xs[i + 1]); ++x) put(x, y, c);
      }
    }
    for (size_t i = 0; i + 3 < pts.size(); i += 2) line(pts[i], pts[i + 1], pts[i + 2], pts[i + 3], c, t);
  }

  void text(const PrimCmd& c) {
    const Font& f = Font::instance();
    float x = c.x0;
    if (c.thickness > 0.5f) x -= f.text_width(c.text, c.size) * 0.5f;  // centred
    std::vector<float> quads;
    f.layout(c.text, x, c.y0, c.size, quads);
    // Each glyph pixel arrives as a unit-sized quad; fill it as an integer
    // pixel block so small text is solid rather than a dotted smear.
    for (size_t i = 0; i + 7 < quads.size(); i += 8) {
      const float px = quads[i], py = quads[i + 1];
      const int pw = std::max(1, static_cast<int>(std::lround(quads[i + 2] - px)));
      const int ph = std::max(1, static_cast<int>(std::lround(quads[i + 5] - py)));
      const int ox = static_cast<int>(std::floor(px));
      const int oy = static_cast<int>(std::floor(py));
      for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) put(ox + x, oy + y, c.color);
    }
  }

  std::string out_;
  uint32_t w_ = 0, h_ = 0;
  std::vector<uint8_t> fb_;
};

}  // namespace

std::unique_ptr<IBackend> make_offline_backend(const std::string& out_path) {
  return std::make_unique<OfflineBackend>(out_path);
}

bool write_svg(const DrawList& dl, uint32_t w, uint32_t h, const std::string& path) {
  ensure_parent_dir(path);
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h
    << "\" viewBox=\"0 0 " << w << " " << h << "\">\n";
  f << "<rect width=\"100%\" height=\"100%\" fill=\"#101216\"/>\n";
  auto hex = [](Color c) {
    char b[16];
    std::snprintf(b, sizeof(b), "#%02x%02x%02x", c.r, c.g, c.b);
    return std::string(b);
  };
  for (const PrimCmd& c : dl.commands()) {
    switch (c.kind) {
      case Prim::kLine:
        f << "<line x1=\"" << c.x0 << "\" y1=\"" << c.y0 << "\" x2=\"" << c.x1 << "\" y2=\""
          << c.y1 << "\" stroke=\"" << hex(c.color) << "\" stroke-width=\"" << c.thickness
          << "\"/>\n";
        break;
      case Prim::kRect:
      case Prim::kRectOutline: {
        const char* tag = c.kind == Prim::kRect ? "rect" : "rect";
        f << "<" << tag << " x=\"" << c.rect.x << "\" y=\"" << c.rect.y << "\" width=\""
          << c.rect.w << "\" height=\"" << c.rect.h << "\" fill=\""
          << (c.kind == Prim::kRect ? hex(c.color) : "none") << "\" stroke=\""
          << (c.kind == Prim::kRect ? "none" : hex(c.color)) << "\" stroke-width=\""
          << (c.kind == Prim::kRect ? 0 : c.thickness) << "\"/>\n";
        break;
      }
      case Prim::kCircle:
      case Prim::kCircleOutline:
        f << "<circle cx=\"" << c.cx << "\" cy=\"" << c.cy << "\" r=\"" << c.r << "\" fill=\""
          << (c.kind == Prim::kCircle ? hex(c.color) : "none") << "\" stroke=\""
          << (c.kind == Prim::kCircle ? "none" : hex(c.color)) << "\" stroke-width=\""
          << (c.kind == Prim::kCircle ? 0 : c.thickness) << "\"/>\n";
        break;
      case Prim::kPolygon:
        if (c.pts.size() >= 6) {
          f << "<polygon points=\"";
          for (size_t i = 0; i + 1 < c.pts.size(); i += 2)
            f << c.pts[i] << "," << c.pts[i + 1] << " ";
          f << "\" fill=\"" << (c.filled ? hex(c.color) : "none") << "\" stroke=\"" << hex(c.color)
            << "\" stroke-width=\"" << c.thickness << "\"/>\n";
        }
        break;
      case Prim::kText: {
        // The 6x8 cell is emitted as a <text> with a monospace family; exact
        // metrics differ from the live overlay but it stays readable in a diff.
        f << "<text x=\"" << c.x0 << "\" y=\"" << (c.y0 + c.size) << "\" fill=\"" << hex(c.color)
          << "\" font-family=\"monospace\" font-size=\"" << (c.size * 0.82f)
          << "\">" << c.text << "</text>\n";
        break;
      }
    }
  }
  f << "</svg>\n";
  return f.good();
}

}  // namespace lens
