#include "matrix.h"

#include <cmath>

namespace lens {

Mat4 mat_mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
      r.m[i][j] = s;
    }
  return r;
}

Mat4 mat_perspective(float fov_y_rad, float aspect, float znear, float zfar) {
  // Left-handed perspective paired with mat_view_from_pos.
  //
  // View space has +Z in front of the eye. The output is DirectX-style clip
  // space: z_ndc in [0,1], and w = +z_view so world_to_screen's divide gives
  // the perspective foreshortening directly.
  //
  //   x_clip = (f/aspect) * x_view
  //   y_clip = f * y_view
  //   z_clip = (zf/(zf-zn)) * z_view - (zn*zf)/(zf-zn)
  //   w_clip = z_view
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.m[i][j] = 0.f;
  const float f = 1.f / std::tan(fov_y_rad * 0.5f);
  r.m[0][0] = f / aspect;
  r.m[1][1] = f;
  r.m[2][2] = zfar / (zfar - znear);
  r.m[2][3] = -(znear * zfar) / (zfar - znear);
  r.m[3][2] = 1.f;  // w = z_view
  return r;
}

Mat4 mat_translate(const Vec3& t) {
  Mat4 r;
  r.m[0][3] = t.x;
  r.m[1][3] = t.y;
  r.m[2][3] = t.z;
  return r;
}

Mat4 mat_view_from_pos(const Vec3& eye, const Vec3& target, const Vec3& up) {
  // Left-handed look-at, paired with mat_perspective's DirectX clip space
  // (z in [0,1], w = -z_view). The camera looks down +Z in view space, so the
  // third row IS the forward axis and its translation is -dot(fwd, eye). A point
  // in front of the camera then has a positive w after projection.
  const Vec3 delta = vec_sub(target, eye);
  const float len = vec_len(delta);
  const Vec3 fwd = (len > 1e-6f) ? vec_scale(delta, 1.f / len) : Vec3{0.f, 0.f, 1.f};

  // right = normalize(cross(up, fwd)) for left-handed
  Vec3 r = {up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z,
            up.x * fwd.y - up.y * fwd.x};
  const float rl = vec_len(r);
  r = (rl > 1e-6f) ? vec_scale(r, 1.f / rl) : Vec3{1.f, 0.f, 0.f};
  // up = cross(fwd, r)
  const Vec3 u = {fwd.y * r.z - fwd.z * r.y, fwd.z * r.x - fwd.x * r.z,
                  fwd.x * r.y - fwd.y * r.x};

  Mat4 m;
  m.m[0][0] = r.x;   m.m[0][1] = r.y;   m.m[0][2] = r.z;   m.m[0][3] = -vec_dot(r, eye);
  m.m[1][0] = u.x;   m.m[1][1] = u.y;   m.m[1][2] = u.z;   m.m[1][3] = -vec_dot(u, eye);
  m.m[2][0] = fwd.x; m.m[2][1] = fwd.y; m.m[2][2] = fwd.z; m.m[2][3] = -vec_dot(fwd, eye);
  m.m[3][0] = 0.f;    m.m[3][1] = 0.f;    m.m[3][2] = 0.f;    m.m[3][3] = 1.f;
  return m;
}

float vec_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 vec_sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vec_add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vec_scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float vec_len(const Vec3& a) { return std::sqrt(vec_dot(a, a)); }

bool world_to_screen(const Mat4& vp, const Vec3& w, float sw, float sh, Vec2& out) {
  const float x = w.x, y = w.y, z = w.z;
  const float cx = vp.m[0][0] * x + vp.m[0][1] * y + vp.m[0][2] * z + vp.m[0][3];
  const float cy = vp.m[1][0] * x + vp.m[1][1] * y + vp.m[1][2] * z + vp.m[1][3];
  const float cw = vp.m[3][0] * x + vp.m[3][1] * y + vp.m[3][2] * z + vp.m[3][3];
  if (std::fabs(cw) < 1e-6f) return false;
  const float inv = 1.f / cw;
  out.x = (cx * inv * 0.5f + 0.5f) * sw;
  out.y = (1.f - (cy * inv * 0.5f + 0.5f)) * sh;
  return cw > 0.f;  // true = in front of the camera (left-handed view space)
}

Basis basis_from_view(const Mat4& view) {
  Basis b;
  b.right = {view.m[0][0], view.m[0][1], view.m[0][2]};
  b.up = {view.m[1][0], view.m[1][1], view.m[1][2]};
  b.forward = {view.m[2][0], view.m[2][1], view.m[2][2]};
  return b;
}

}  // namespace lens
