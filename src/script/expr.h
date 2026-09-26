// expr.h - a small expression language over the live frame.
//
// Scripts are how a non-specialist customises lens without touching C++. The
// grammar is deliberately tiny: read fields off the current entity, do maths,
// and return a number that a feature draws.
//
//   if health < 40 then color red
//   draw_box when distance < 30 and team != 3
//   text name .. " " .. health
//
// See docs/scripting.md for the full grammar and examples.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace lens {

struct EvalCtx {
  // Field name -> value, for the current entity. Flat floats/ints and strings.
  std::map<std::string, double> nums;
  std::map<std::string, std::string> strs;
  double dt = 0.0;
  unsigned frame = 0;
};

class Expr {
 public:
  // Compiles a program. Returns false with a message on a syntax error.
  static bool compile(const std::string& src, std::string* program, std::string* err);
  // Evaluates a compiled program against a context. Returns the last value.
  static bool run(const std::string& program, const EvalCtx& ctx, double* out, std::string* err);
};

}  // namespace lens
