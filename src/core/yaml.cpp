#include "yaml.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace lens {
namespace {

struct Line {
  int indent = 0;
  std::string body;
  int number = 0;
};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

std::string strip_comment(const std::string& s) {
  bool sq = false, dq = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'' && !dq) sq = !sq;
    else if (s[i] == '"' && !sq) dq = !dq;
    else if (s[i] == '#' && !sq && !dq) {
      if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') return s.substr(0, i);
    }
  }
  return s;
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

class Parser {
 public:
  explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

  // Parse the block that starts at the current cursor. `indent` is the level
  // every line of that block sits at; the kind is decided by the line under the
  // cursor, not by indexing with the level.
  YNode parse_block(int indent) {
    if (pos_ >= static_cast<int>(lines_.size())) {
      YNode empty;
      return empty;
    }
    const std::string& body = lines_[pos_].body;
    if (body.size() >= 1 && body[0] == '-' &&
        (body.size() == 1 || body[1] == ' ' || body[1] == '\0')) {
      return parse_seq(indent);
    }
    return parse_map(indent);
  }

  int depth() const { return static_cast<int>(lines_.size()) - pos_; }

 private:
  YNode parse_map(int indent) {
    YNode node;
    node.kind = YNode::Kind::kMap;
    while (pos_ < static_cast<int>(lines_.size())) {
      const Line& ln = lines_[pos_];
      if (ln.indent < indent) break;
      if (ln.indent > indent) throw std::runtime_error("yaml: bad indent at line " +
                                                       std::to_string(ln.number));
      if (ln.body[0] == '-' && (ln.body.size() == 1 || ln.body[1] == ' ')) break;

      const size_t colon = ln.body.find(':');
      if (colon == std::string::npos)
        throw std::runtime_error("yaml: expected 'key:' at line " + std::to_string(ln.number));
      const std::string key = trim(ln.body.substr(0, colon));
      const std::string rest = trim(ln.body.substr(colon + 1));
      ++pos_;

      if (rest == "|" || rest == ">" || rest == "|-" || rest == ">-") {
        std::string blob;
        const int base = lines_[pos_ - 1].indent;
        while (pos_ < static_cast<int>(lines_.size()) && lines_[pos_].indent > base) {
          std::string b = lines_[pos_].body;
          if (rest[0] == '>') {
            if (!blob.empty()) blob += ' ';
            blob += b;
          } else {
            if (!blob.empty()) blob += '\n';
            blob += b;
          }
          ++pos_;
        }
        node.map.emplace_back(key, YNode{YNode::Kind::kScalar, blob, {}, {}});
      } else if (rest.empty()) {
        // Nested block, a sequence at the same indent, or an empty value.
        if (pos_ < static_cast<int>(lines_.size()) && lines_[pos_].indent > indent) {
          node.map.emplace_back(key, parse_block(lines_[pos_].indent));
        } else if (pos_ < static_cast<int>(lines_.size()) && lines_[pos_].indent == indent &&
                   lines_[pos_].body[0] == '-') {
          node.map.emplace_back(key, parse_block(indent));
        } else {
          node.map.emplace_back(key, YNode{});
        }
      } else if (rest[0] == '[') {
        node.map.emplace_back(key, flow_seq(rest));
      } else {
        node.map.emplace_back(key, YNode{YNode::Kind::kScalar, unquote(rest), {}, {}});
      }
    }
    return node;
  }

  YNode parse_seq(int indent) {
    YNode node;
    node.kind = YNode::Kind::kSeq;
    while (pos_ < static_cast<int>(lines_.size())) {
      const Line& ln = lines_[pos_];
      if (ln.indent < indent) break;
      if (ln.indent > indent)
        throw std::runtime_error("yaml: bad indent in sequence at line " +
                                 std::to_string(ln.number));
      if (!(ln.body[0] == '-' && (ln.body.size() == 1 || ln.body[1] == ' '))) break;

      // "- key: v" carries a nested map. Rewrite the dash into two spaces and
      // treat the item's content as a block at indent+2, so the map parser can
      // be reused unchanged. The item is complete when the next line dedents
      // back to (or below) the dash's own indent.
      const int item_indent = indent + 2;
      lines_[pos_].body = "  " + ln.body.substr(1);
      lines_[pos_].indent = item_indent;

      // Collect just this item's lines.
      std::vector<Line> item;
      item.push_back(lines_[pos_]);
      size_t j = pos_ + 1;
      while (j < lines_.size() && lines_[j].indent >= item_indent) item.push_back(lines_[j++]);
      pos_ = j;

      Parser sub(std::move(item));
      node.seq.push_back(sub.parse_block(item_indent));
    }
    return node;
  }

  static YNode flow_seq(const std::string& s) {
    YNode n;
    n.kind = YNode::Kind::kSeq;
    size_t a = s.find('[');
    size_t b = s.rfind(']');
    if (a == std::string::npos || b == std::string::npos || b <= a) return n;
    std::string inner = s.substr(a + 1, b - a - 1);
    std::stringstream ss(inner);
    std::string item;
    while (std::getline(ss, item, ',')) {
      item = trim(item);
      if (!item.empty()) n.seq.push_back(YNode{YNode::Kind::kScalar, unquote(item), {}, {}});
    }
    return n;
  }

  std::vector<Line> lines_;
  int pos_ = 0;
};

}  // namespace

const YNode* YNode::find(const std::string& key) const {
  for (const auto& kv : map)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string YNode::str(const std::string& key, const std::string& def) const {
  const YNode* n = find(key);
  if (!n) return def;
  return n->is_scalar() ? n->scalar : def;
}

long long YNode::i64(const std::string& key, long long def) const {
  const YNode* n = find(key);
  if (!n || !n->is_scalar() || n->scalar.empty()) return def;
  char* end = nullptr;
  const long long v = std::strtoll(n->scalar.c_str(), &end, 0);
  return end == n->scalar.c_str() ? def : v;
}

double YNode::f64(const std::string& key, double def) const {
  const YNode* n = find(key);
  if (!n || !n->is_scalar() || n->scalar.empty()) return def;
  char* end = nullptr;
  const double v = std::strtod(n->scalar.c_str(), &end);
  return end == n->scalar.c_str() ? def : v;
}

bool YNode::boolean(const std::string& key, bool def) const {
  const YNode* n = find(key);
  if (!n || !n->is_scalar()) return def;
  const std::string& s = n->scalar;
  if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
  if (s == "false" || s == "no" || s == "off" || s == "0") return false;
  return def;
}

std::vector<std::string> YNode::str_list(const std::string& key) const {
  std::vector<std::string> out;
  const YNode* n = find(key);
  if (!n) return out;
  if (n->is_seq()) {
    for (const auto& e : n->seq)
      if (e.is_scalar()) out.push_back(e.scalar);
  } else if (n->is_scalar()) {
    std::stringstream ss(n->scalar);
    std::string item;
    while (std::getline(ss, item, ',')) {
      item = trim(item);
      if (!item.empty()) out.push_back(item);
    }
  }
  return out;
}

YNode YNode::parse(const std::string& text) {
  std::vector<Line> lines;
  std::stringstream ss(text);
  std::string raw;
  int num = 0;
  while (std::getline(ss, raw)) {
    ++num;
    std::string body = strip_comment(raw);
    if (trim(body).empty()) continue;
    if (body.rfind("---", 0) == 0 || body.rfind("...", 0) == 0) continue;
    Line ln;
    ln.number = num;
    size_t ind = 0;
    while (ind < body.size() && body[ind] == ' ') ++ind;
    ln.indent = static_cast<int>(ind);
    ln.body = trim(body);
    if (ln.body.empty()) continue;
    lines.push_back(ln);
  }
  Parser p(std::move(lines));
  return p.parse_block(0);
}

bool YNode::try_parse(const std::string& text, YNode& out, std::string* err) {
  try {
    out = YNode::parse(text);
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace lens
