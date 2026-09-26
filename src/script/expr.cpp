// expr.cpp - recursive-descent compiler + VM for the script language.
#include "expr.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

namespace lens {
namespace {

enum class Tok {
  kNum, kIdent, kStr, kOp, kLParen, kRParen, kComma, kEnd,
};

struct Token {
  Tok kind = Tok::kEnd;
  double num = 0;
  std::string text;

  // Constructors rather than aggregate initialisers: with a default member
  // initialiser present the struct is not an aggregate for -Wextra's purposes,
  // and `{Tok::kOp}` would warn about every omitted member.
  Token() = default;
  Token(Tok k) : kind(k) {}
  Token(Tok k, std::string t) : kind(k), text(std::move(t)) {}
  Token(Tok k, double n, std::string t) : kind(k), num(n), text(std::move(t)) {}
};

std::vector<Token> lex(const std::string& s, std::string* err) {
  std::vector<Token> out;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t j = i;
      while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
      out.push_back(Token(Tok::kNum, std::strtod(s.substr(i, j - i).c_str(), nullptr), ""));
      i = j;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
      out.push_back(Token(Tok::kIdent, 0.0, s.substr(i, j - i)));
      i = j;
      continue;
    }
    if (c == '"' || c == '\'') {
      const char q = c;
      size_t j = i + 1;
      std::string v;
      while (j < s.size() && s[j] != q) v += s[j++];
      if (j >= s.size()) { *err = "unterminated string"; return {}; }
      out.push_back(Token(Tok::kStr, 0.0, v));
      i = j + 1;
      continue;
    }
    if (c == '(') { out.push_back(Token(Tok::kLParen)); ++i; continue; }
    if (c == ')') { out.push_back(Token(Tok::kRParen)); ++i; continue; }
    if (c == ',') { out.push_back(Token(Tok::kComma)); ++i; continue; }
    // Two-char operators first.
    if (i + 1 < s.size()) {
      const std::string two = s.substr(i, 2);
      if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||" ||
          two == "..") {
        out.push_back(Token(Tok::kOp, 0.0, two));
        i += 2;
        continue;
      }
    }
    if (std::strchr("+-*/%<>=!&|", c)) {
      out.push_back(Token(Tok::kOp, 0.0, std::string(1, c)));
      ++i;
      continue;
    }
    if (err) *err = std::string("unexpected character '") + c + "'";
    return {};
  }
  out.push_back(Token(Tok::kEnd));
  return out;
}

// A value is either a number or a string; the VM carries both.
struct Val {
  double n = 0;
  std::string s;
  bool is_str = false;
};

class Vm {
 public:
  Vm(const std::vector<Token>& t, const EvalCtx& ctx) : t_(t), ctx_(ctx) {}

  Val expr() {
    Val v = or_expr();
    return v;
  }

  const std::string& err() const { return err_; }

 private:
  bool eat_op(const char* op) {
    if (t_[i_].kind == Tok::kOp && t_[i_].text == op) { ++i_; return true; }
    return false;
  }
  bool eat_kw(const char* kw) {
    if (t_[i_].kind == Tok::kIdent && t_[i_].text == kw) { ++i_; return true; }
    return false;
  }

  double to_n(const Val& v) const { return v.is_str ? std::atof(v.s.c_str()) : v.n; }

  Val or_expr() {
    Val l = and_expr();
    while (eat_op("||")) {
      const Val r = and_expr();
      l = Val{(to_n(l) != 0 || to_n(r) != 0) ? 1.0 : 0.0, "", false};
    }
    return l;
  }
  Val and_expr() {
    Val l = cmp_expr();
    while (eat_op("&&")) {
      const Val r = cmp_expr();
      l = Val{(to_n(l) != 0  && to_n(r) != 0) ? 1.0 : 0.0, "", false};
    }
    return l;
  }
  Val cmp_expr() {
    Val l = add_expr();
    for (;;) {
      double res = 0;
      if (eat_op("==")) res = (to_n(l) == to_n(add_expr())) ? 1 : 0;
      else if (eat_op("!=")) res = (to_n(l) != to_n(add_expr())) ? 1 : 0;
      else if (eat_op("<=")) res = (to_n(l) <= to_n(add_expr())) ? 1 : 0;
      else if (eat_op(">=")) res = (to_n(l) >= to_n(add_expr())) ? 1 : 0;
      else if (eat_op("<")) res = (to_n(l) < to_n(add_expr())) ? 1 : 0;
      else if (eat_op(">")) res = (to_n(l) > to_n(add_expr())) ? 1 : 0;
      else break;
      l = Val{res, "", false};
    }
    return l;
  }
  Val add_expr() {
    Val l = mul_expr();
    for (;;) {
      if (eat_op("+")) { const Val r = mul_expr(); l = num(to_n(l) + to_n(r)); continue; }
      if (eat_op("..")) {
        const Val r = mul_expr();
        l = Val{0, (l.is_str ? l.s : std::to_string(l.n)) + (r.is_str ? r.s : std::to_string(r.n)),
                true};
        continue;
      }
      if (eat_op("-")) { const Val r = mul_expr(); l = num(to_n(l) - to_n(r)); continue; }
      break;
    }
    return l;
  }
  Val mul_expr() {
    Val l = unary();
    for (;;) {
      if (eat_op("*")) { const Val r = unary(); l = num(to_n(l) * to_n(r)); continue; }
      if (eat_op("/")) { const Val r = unary(); l = num(to_n(r) != 0 ? to_n(l) / to_n(r) : 0); continue; }
      if (eat_op("%")) { const Val r = unary(); l = num(std::fmod(to_n(l), to_n(r))); continue; }
      break;
    }
    return l;
  }
  Val num(double d) const { return Val{d, "", false}; }

  Val unary() {
    if (eat_op("-")) { const Val v = unary(); return num(-to_n(v)); }
    if (eat_op("!")) { const Val v = unary(); return num(to_n(v) == 0 ? 1 : 0); }
    return primary();
  }

  Val primary() {
    if (t_[i_].kind == Tok::kNum) { return num(t_[i_].num); }
    if (t_[i_].kind == Tok::kStr) { return Val{0, t_[i_].text, true}; }
    if (t_[i_].kind == Tok::kLParen) {
      ++i_;
      const Val v = expr();
      if (t_[i_].kind == Tok::kRParen) ++i_;
      else err_ = "expected )";
      return v;
    }
    if (t_[i_].kind == Tok::kIdent) {
      const std::string id = t_[i_].text;
      ++i_;
      // Function calls.
      if (t_[i_].kind == Tok::kLParen) {
        ++i_;
        std::vector<Val> args;
        if (t_[i_].kind != Tok::kRParen) {
          args.push_back(expr());
          while (t_[i_].kind == Tok::kComma) { ++i_; args.push_back(expr()); }
        }
        if (t_[i_].kind == Tok::kRParen) ++i_;
        else err_ = "expected ) after args";
        return call(id, args);
      }
      // Context lookup.
      auto n = ctx_.nums.find(id);
      if (n != ctx_.nums.end()) return num(n->second);
      auto s = ctx_.strs.find(id);
      if (s != ctx_.strs.end()) return Val{0, s->second, true};
      err_ = "unknown identifier \"" + id + "\"";
      return num(0);
    }
    err_ = "unexpected end of expression";
    return num(0);
  }

  Val call(const std::string& fn, const std::vector<Val>& a) {
    auto n = [&](size_t i) { return i < a.size() ? to_n(a[i]) : 0.0; };
    if (fn == "abs") return num(std::fabs(n(0)));
    if (fn == "min") return num(std::min(n(0), n(1)));
    if (fn == "max") return num(std::max(n(0), n(1)));
    if (fn == "floor") return num(std::floor(n(0)));
    if (fn == "ceil") return num(std::ceil(n(0)));
    if (fn == "round") return num(std::round(n(0)));
    if (fn == "sqrt") return num(std::sqrt(n(0)));
    if (fn == "sin") return num(std::sin(n(0)));
    if (fn == "cos") return num(std::cos(n(0)));
    if (fn == "clamp") return num(std::clamp(n(0), n(1), n(2)));
    if (fn == "int") return num(static_cast<double>(static_cast<long long>(n(0))));
    if (fn == "str") return Val{0, a.empty() ? "" : (a[0].is_str ? a[0].s : std::to_string(a[0].n)), true};
    err_ = "unknown function \"" + fn + "\"";
    return num(0);
  }

  const std::vector<Token>& t_;
  const EvalCtx& ctx_;
  size_t i_ = 0;
  std::string err_;
};

}  // namespace

bool Expr::compile(const std::string& src, std::string* program, std::string* err) {
  // "compile" is a validation pass: lex + one full parse. The program text is
  // the source itself, re-lexed per frame, which is fast enough at these sizes
  // and keeps a script reloadable with no cache invalidation.
  std::string lex_err;
  const auto toks = lex(src, &lex_err);
  if (!lex_err.empty()) {
    if (err) *err = lex_err;
    return false;
  }
  EvalCtx empty;
  Vm vm(toks, empty);
  vm.expr();
  if (!vm.err().empty()) {
    // A missing context field is expected at validate time; anything else is not.
    if (vm.err().rfind("unknown identifier", 0) != 0) {
      if (err) *err = vm.err();
      return false;
    }
  }
  if (program) *program = src;
  return true;
}

bool Expr::run(const std::string& program, const EvalCtx& ctx, double* out, std::string* err) {
  std::string lex_err;
  const auto toks = lex(program, &lex_err);
  if (!lex_err.empty()) {
    if (err) *err = lex_err;
    return false;
  }
  Vm vm(toks, ctx);
  const Val v = vm.expr();
  if (!vm.err().empty()) {
    if (err) *err = vm.err();
    return false;
  }
  if (out) *out = v.is_str ? std::atof(v.s.c_str()) : v.n;
  return true;
}

}  // namespace lens
