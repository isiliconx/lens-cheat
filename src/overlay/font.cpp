// font.cpp - layout over the generated 6x8 cell set.
//
// The glyph shapes live in font_data.cpp, produced by tools/gen_font.py, so a
// glyph can never ship as a hand-typing mistake. `size` is the cap height in
// pixels: one font unit is size/8, and a glyph advances 6 units.
#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "font_data.h"

namespace lens {

Font::Font() {
  for (int i = 0; i < 96; ++i) {
    glyphs_[i].w = 6;
    glyphs_[i].h = 8;
    for (int r = 0; r < 8; ++r) glyphs_[i].rows[r] = kGlyphRows[i][r];
  }
}

const Font& Font::instance() {
  static Font f;
  return f;
}

const FontGlyph& Font::glyph(char c) const {
  const int i = static_cast<int>(static_cast<unsigned char>(c)) - 0x20;
  if (i < 0 || i >= 96) return glyphs_[0];
  return glyphs_[i];
}

// One font unit in pixels. The unit must be a whole number: a glyph pixel is
// drawn as a unit x unit block and each glyph advances 6 units, so a fractional
// unit makes consecutive blocks overlap and the line turns into a smear.
static float unit_for(float size) {
  return std::max(1.f, static_cast<float>(std::lround(size / 8.f)));
}

float Font::text_width(const std::string& s, float size) const {
  return static_cast<float>(s.size()) * 6.f * unit_for(size);
}

float Font::text_height(float size) const { return 8.f * unit_for(size); }

void Font::layout(const std::string& s, float x, float y, float size,
                  std::vector<float>& out) const {
  const float unit = unit_for(size);
  float pen = x;
  for (char c : s) {
    const FontGlyph& g = glyph(c);
    for (int row = 0; row < 8; ++row) {
      for (int col = 0; col < 6; ++col) {
        if (!(g.rows[row] & (1 << (5 - col)))) continue;
        const float px = pen + col * unit;
        const float py = y + row * unit;
        out.push_back(px);
        out.push_back(py);
        out.push_back(px + unit);
        out.push_back(py);
        out.push_back(px + unit);
        out.push_back(py + unit);
        out.push_back(px);
        out.push_back(py + unit);
      }
    }
    pen += 6.f * unit;
  }
}

}  // namespace lens
