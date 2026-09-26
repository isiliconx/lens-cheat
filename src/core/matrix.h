// matrix.h - 4x4 float matrix, row-vector (DirectX) convention, and the
// world-to-screen projection every draw call goes through.
#pragma once
#include <cstdint>

namespace lens {

struct Vec2 {
  float x = 0, y = 0;
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

// m[row][col], row-vector: clip = v * M, v = (x, y, z, 1).
struct Mat4 {
  float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

Mat4 mat_mul(const Mat4& a, const Mat4& b);
Mat4 mat_perspective(float fov_y_rad, float aspect, float znear, float zfar);
Mat4 mat_view_from_pos(const Vec3& eye, const Vec3& target, const Vec3& up);
Mat4 mat_translate(const Vec3& t);

float vec_dot(const Vec3& a, const Vec3& b);
Vec3 vec_sub(const Vec3& a, const Vec3& b);
Vec3 vec_add(const Vec3& a, const Vec3& b);
Vec3 vec_scale(const Vec3& a, float s);
float vec_len(const Vec3& a);

// Project a world point. Returns false when the point is behind the near plane
// or the matrix is degenerate.
bool world_to_screen(const Mat4& viewproj, const Vec3& world, float screen_w, float screen_h,
                     Vec2& out);

// Camera basis extracted from a view matrix - used for the 3D box edges and for
// the "is this entity in front of me" test.
struct Basis {
  Vec3 right, up, forward;
};
Basis basis_from_view(const Mat4& view);

}  // namespace lens
