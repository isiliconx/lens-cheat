#include "drawlist.h"

namespace lens {

void DrawList::box_edges(const Mat4& vp, const Vec3& lo, const Vec3& hi, float sw, float sh,
                         std::vector<float>& out) {
  // 8 corners, 12 edges. Both corner sets must project in front; a box straddling
  // the near plane is not drawn rather than drawn wrong.
  Vec3 c[8] = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
  Vec2 s[8];
  for (int i = 0; i < 8; ++i)
    if (!world_to_screen(vp, c[i], sw, sh, s[i])) { out.clear(); return; }

  static const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                              {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  out.clear();
  out.reserve(24);
  for (const auto& ed : e) {
    out.push_back(s[ed[0]].x); out.push_back(s[ed[0]].y);
    out.push_back(s[ed[1]].x); out.push_back(s[ed[1]].y);
  }
}

bool DrawList::project_box(const Mat4& vp, const Vec3& lo, const Vec3& hi, float sw, float sh,
                           Rect& out) const {
  std::vector<float> edges;
  box_edges(vp, lo, hi, sw, sh, edges);
  if (edges.size() != 24) return false;
  float min_x = edges[0], max_x = edges[0], min_y = edges[1], max_y = edges[1];
  for (size_t i = 0; i < edges.size(); i += 2) {
    min_x = std::min(min_x, edges[i]);
    max_x = std::max(max_x, edges[i]);
    min_y = std::min(min_y, edges[i + 1]);
    max_y = std::max(max_y, edges[i + 1]);
  }
  out = Rect{min_x, min_y, max_x - min_x, max_y - min_y};
  return true;
}

}  // namespace lens
