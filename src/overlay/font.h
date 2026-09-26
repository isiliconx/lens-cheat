// font.h - a built-in 6x8 bitmap font.
//
// Shipping a font file would make the repo a binary dependency and would make
// the offline backend unable to render headless on a bare checkout. 96 printable
// ASCII glyphs at 6x8 is enough for a debug overlay, and every backend can use
// it without a rasteriser.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/matrix.h"

namespace lens {

struct FontGlyph {
  uint8_t w = 6, h = 8;
  // 8 rows, one byte per row, bit 0 = leftmost pixel.
  uint8_t rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

class Font {
 public:
  static const Font& instance();

  const FontGlyph& glyph(char c) const;
  float text_width(const std::string& s, float size) const;
  float text_height(float size) const;
  // Fills a flat x,y list of the glyph quads, in order.
  void layout(const std::string& s, float x, float y, float size, std::vector<float>& out) const;

 private:
  Font();
  FontGlyph glyphs_[96];
};

}  // namespace lens
