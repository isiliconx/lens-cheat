// drawlist.h - a backend-agnostic list of primitives.
//
// Features never touch a graphics API. They push into a DrawList, and whichever
// backend is present (DirectX 11 present hook, offline PPM dump) replays it. The
// same ESP code therefore produces the same picture in a live overlay and in a
// headless test, which is how the self-test can assert on it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/matrix.h"

namespace lens {

struct Color {
  uint8_t r = 255, g = 255, b = 255, a = 255;
  constexpr Color() = default;
  constexpr Color(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) : r(R), g(G), b(B), a(A) {}
  constexpr uint32_t abgr() const {
    return (uint32_t)a << 24 | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
  }
};

namespace col {
inline constexpr Color kWhite(255, 255, 255);
inline constexpr Color kBlack(0, 0, 0);
inline constexpr Color kRed(235, 60, 60);
inline constexpr Color kGreen(70, 220, 110);
inline constexpr Color kBlue(80, 150, 255);
inline constexpr Color kYellow(240, 210, 80);
inline constexpr Color kOrange(255, 150, 50);
inline constexpr Color kCyan(70, 220, 220);
inline constexpr Color kMagenta(230, 90, 230);
inline constexpr Color kGray(120, 120, 130);
inline constexpr Color kEnemy(255, 90, 90);
inline constexpr Color kFriendly(90, 200, 255);
}  // namespace col

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
};

enum class Prim {
  kLine, kRect, kRectOutline, kCircle, kCircleOutline, kText, kPolygon,
};

struct PrimCmd {
  Prim kind = Prim::kLine;
  Color color{};
  float thickness = 1.f;
  // Line
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  // Rect
  Rect rect{};
  // Circle
  float cx = 0, cy = 0, r = 0;
  // Text
  std::string text;
  float size = 13.f;
  // Polygon (3D box edges): flat x,y pairs
  std::vector<float> pts;
  bool filled = false;
};

class DrawList {
 public:
  void clear() { cmds_.clear(); }

  void line(float x0, float y0, float x1, float y1, Color c, float t = 1.f) {
    PrimCmd p;
    p.kind = Prim::kLine;
    p.x0 = x0; p.y0 = y0; p.x1 = x1; p.y1 = y1;
    p.color = c; p.thickness = t;
    cmds_.push_back(std::move(p));
  }

  void rect(const Rect& r, Color c) {
    PrimCmd p;
    p.kind = Prim::kRect;
    p.rect = r; p.color = c;
    cmds_.push_back(std::move(p));
  }

  void rect_outline(const Rect& r, Color c, float t = 1.f) {
    PrimCmd p;
    p.kind = Prim::kRectOutline;
    p.rect = r; p.color = c; p.thickness = t;
    cmds_.push_back(std::move(p));
  }

  void circle(float cx, float cy, float r, Color c) {
    PrimCmd p;
    p.kind = Prim::kCircle;
    p.cx = cx; p.cy = cy; p.r = r; p.color = c;
    cmds_.push_back(std::move(p));
  }

  void circle_outline(float cx, float cy, float r, Color c, float t = 1.f) {
    PrimCmd p;
    p.kind = Prim::kCircleOutline;
    p.cx = cx; p.cy = cy; p.r = r; p.color = c; p.thickness = t;
    cmds_.push_back(std::move(p));
  }

  void text(float x, float y, const std::string& s, Color c, float size = 13.f,
            bool centered = false) {
    PrimCmd p;
    p.kind = Prim::kText;
    p.x0 = x; p.y0 = y;
    p.text = s; p.color = c; p.size = size;
    p.thickness = centered ? 1.f : 0.f;
    cmds_.push_back(std::move(p));
  }

  void polygon(std::vector<float> pts, Color c, bool filled, float t = 1.f) {
    PrimCmd p;
    p.kind = Prim::kPolygon;
    p.pts = std::move(pts);
    p.color = c;
    p.filled = filled;
    p.thickness = t;
    cmds_.push_back(std::move(p));
  }

  const std::vector<PrimCmd>& commands() const { return cmds_; }
  size_t size() const { return cmds_.size(); }
  size_t count(Prim k) const {
    size_t n = 0;
    for (const auto& c : cmds_) n += (c.kind == k);
    return n;
  }

  // Project a 3D box given its two opposite corners, returning the screen
  // rect. Returns false when any corner is behind the camera, which is how the
  // off-screen indicator path is chosen.
  bool project_box(const Mat4& vp, const Vec3& a, const Vec3& b, float sw, float sh, Rect& out) const;

  static void box_edges(const Mat4& vp, const Vec3& a, const Vec3& b, float sw, float sh,
                        std::vector<float>& out_edges);

 private:
  std::vector<PrimCmd> cmds_;
};

}  // namespace lens
