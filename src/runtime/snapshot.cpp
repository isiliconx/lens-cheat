#include "snapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "core/output.h"

namespace lens {

// Pull every "quoted string" out of a JSON array blob. Used by the snapshot
// reader for the notes list; the schema is fixed so a real JSON parser is not
// worth the dependency.
static std::vector<std::string> split_quoted(const std::string& blob);

namespace {

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

std::string unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    switch (s[++i]) {
      case 'n': o += '\n'; break;
      case 'r': o += '\r'; break;
      case 't': o += '\t'; break;
      case '"': o += '"'; break;
      case '\\': o += '\\'; break;
      case 'u': {
        if (i + 4 < s.size()) {
          const int cp = std::stoi(s.substr(i + 1, 4), nullptr, 16);
          i += 4;
          if (cp < 0x80) o += static_cast<char>(cp);
        }
        break;
      }
      default: o += s[i];
    }
  }
  return o;
}

const char* health_name(Health h) {
  switch (h) {
    case Health::kOk: return "ok";
    case Health::kStale: return "stale";
    default: return "dead";
  }
}

Health health_from(const std::string& s) {
  if (s == "ok") return Health::kOk;
  if (s == "stale") return Health::kStale;
  return Health::kDead;
}

}  // namespace

std::string Snapshot::to_json(int indent) const {
  const std::string pad(indent * 2, ' ');
  const std::string pad2((indent + 1) * 2, ' ');
  const std::string pad3((indent + 2) * 2, ' ');
  const std::string pad4((indent + 3) * 2, ' ');
  std::ostringstream o;

  o << pad << "{\n";
  o << pad2 << "\"descriptor\": \"" << esc(descriptor) << "\",\n";
  o << pad2 << "\"descriptor_version\": \"" << esc(descriptor_version) << "\",\n";
  o << pad2 << "\"transport\": \"" << esc(transport) << "\",\n";
  o << pad2 << "\"module_name\": \"" << esc(module_name) << "\",\n";
  o << pad2 << "\"module_base\": \"0x" << std::hex << (unsigned long long)module_base
    << std::dec << "\",\n";
  o << pad2 << "\"taken_at\": " << taken_at << ",\n";
  o << pad2 << "\"entity_count\": " << entity_count << ",\n";

  o << pad2 << "\"field_health\": {";
  bool first = true;
  for (const auto& kv : field_health) {
    o << (first ? "\n" : ",\n") << pad3 << "\"" << esc(kv.first) << "\": \"" << health_name(kv.second)
      << "\"";
    first = false;
  }
  o << (first ? "" : "\n" + pad2) << "},\n";

  o << pad2 << "\"notes\": [";
  for (size_t i = 0; i < notes.size(); ++i)
    o << (i ? ", " : "") << "\"" << esc(notes[i]) << "\"";
  o << "],\n";

  o << pad2 << "\"lists\": {";
  first = true;
  for (const auto& lk : lists) {
    o << (first ? "\n" : ",\n") << pad3 << "\"" << esc(lk.first) << "\": [\n";
    for (size_t i = 0; i < lk.second.size(); ++i) {
      const EntitySample& e = lk.second[i];
      o << pad4 << "{\"index\": " << e.index << ", \"addr\": \"0x" << std::hex
        << (unsigned long long)e.addr << std::dec << "\", \"fields\": {";
      bool f2 = true;
      for (const auto& fk : e.fields) {
        o << (f2 ? "\n" : ",\n") << pad4 << "  \"" << esc(fk.first) << "\": {\"type\": \""
          << esc(fk.second.type) << "\", \"value\": \"" << esc(fk.second.value) << "\", \"addr\": \"0x"
          << std::hex << (unsigned long long)fk.second.addr << std::dec << "\", \"readable\": "
          << (fk.second.readable ? "true" : "false") << ", \"health\": \""
          << health_name(fk.second.health) << "\"}";
        f2 = false;
      }
      o << (f2 ? "" : "\n" + pad4) << "}}";
      if (i + 1 < lk.second.size()) o << ",";
    }
    o << "\n" << pad3 << "]";
    first = false;
  }
  o << (first ? "" : "\n" + pad2) << "}\n";
  o << pad << "}\n";
  return o.str();
}

namespace {
// Minimal reader: the schema is fixed, so a full JSON parser is not worth the
// dependency. Scans for keys and pulls balanced values.
struct Cur {
  const std::string& s;
  size_t i = 0;
  explicit Cur(const std::string& str) : s(str) {}
  void skip() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i;
  }
  bool eat(const char* lit) {
    skip();
    const size_t n = std::char_traits<char>::length(lit);
    if (s.compare(i, n, lit) != 0) return false;
    i += n;
    return true;
  }
  std::string string_val() {
    skip();
    if (i >= s.size() || s[i] != '"') return "";
    ++i;
    std::string o;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) { o += s[i]; o += s[i + 1]; i += 2; continue; }
      o += s[i++];
    }
    if (i < s.size()) ++i;
    return unesc(o);
  }
  long long num_val() {
    skip();
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'x' ||
                            (s[i] >= 'a' && s[i] <= 'f') || s[i] == 'X'))
      ++i;
    return std::strtoll(s.substr(start, i - start).c_str(), nullptr, 0);
  }
  uintptr_t hex_val() {
    const std::string v = string_val();
    return static_cast<uintptr_t>(std::strtoull(v.c_str(), nullptr, 0));
  }
  // Advance past a balanced {...} or [...] starting at the current '{' or '['.
  std::string balanced() {
    skip();
    if (i >= s.size() || (s[i] != '{' && s[i] != '[')) return "";
    const size_t start = i;
    int depth = 0;
    bool in_str = false;
    for (; i < s.size(); ++i) {
      if (in_str) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '"') in_str = false;
        continue;
      }
      if (s[i] == '"') { in_str = true; continue; }
      if (s[i] == '{' || s[i] == '[') ++depth;
      else if (s[i] == '}' || s[i] == ']') {
        if (--depth == 0) { ++i; return s.substr(start, i - start); }
      }
    }
    return s.substr(start);
  }
};
}  // namespace

bool Snapshot::from_json(const std::string& text, Snapshot& out, std::string* err) {
  Cur c(text);
  if (!c.eat("{")) { if (err) *err = "snapshot: not a JSON object"; return false; }
  while (true) {
    c.skip();
    if (c.i >= text.size()) break;
    if (c.eat("}")) break;
    const std::string key = c.string_val();
    if (!c.eat(":")) break;
    if (key == "descriptor") out.descriptor = c.string_val();
    else if (key == "descriptor_version") out.descriptor_version = c.string_val();
    else if (key == "transport") out.transport = c.string_val();
    else if (key == "module_name") out.module_name = c.string_val();
    else if (key == "module_base") out.module_base = c.hex_val();
    else if (key == "taken_at") out.taken_at = static_cast<uint64_t>(c.num_val());
    else if (key == "entity_count") out.entity_count = static_cast<int>(c.num_val());
    else if (key == "notes") {
      const std::string blob = c.balanced();
      for (const auto& n : split_quoted(blob)) out.notes.push_back(n);
    } else if (key == "field_health") {
      const std::string blob = c.balanced();
      Cur f(blob);
      f.eat("{");
      while (true) {
        f.skip();
        if (f.i >= blob.size() || f.eat("}")) break;
        const std::string fname = f.string_val();
        f.eat(":");
        out.field_health[fname] = health_from(f.string_val());
        f.eat(",");
      }
    } else if (key == "lists") {
      const std::string blob = c.balanced();
      Cur L(blob);
      L.eat("{");
      while (true) {
        L.skip();
        if (L.i >= blob.size() || L.eat("}")) break;
        const std::string lname = L.string_val();
        L.eat(":");
        const std::string arr = L.balanced();
        std::vector<EntitySample>& vec = out.lists[lname];
        vec.clear();
        Cur A(arr);
        A.eat("[");
        while (true) {
          A.skip();
          if (A.i >= arr.size() || A.eat("]")) break;
          const std::string eblob = A.balanced();
          A.eat(",");
          EntitySample es;
          Cur E(eblob);
          E.eat("{");
          while (true) {
            E.skip();
            if (E.i >= eblob.size() || E.eat("}")) break;
            const std::string k = E.string_val();
            E.eat(":");
            if (k == "index") es.index = static_cast<int>(E.num_val());
            else if (k == "addr") es.addr = E.hex_val();
            else if (k == "fields") {
              const std::string fb = E.balanced();
              Cur F(fb);
              F.eat("{");
              while (true) {
                F.skip();
                if (F.i >= fb.size() || F.eat("}")) break;
                const std::string fname = F.string_val();
                F.eat(":");
                const std::string vblob = F.balanced();
                F.eat(",");
                FieldSample fs;
                Cur V(vblob);
                V.eat("{");
                while (true) {
                  V.skip();
                  if (V.i >= vblob.size() || V.eat("}")) break;
                  const std::string vk = V.string_val();
                  V.eat(":");
                  if (vk == "type") fs.type = V.string_val();
                  else if (vk == "value") fs.value = V.string_val();
                  else if (vk == "addr") fs.addr = V.hex_val();
                  else if (vk == "readable") fs.readable = V.eat("true");
                  else if (vk == "health") fs.health = health_from(V.string_val());
                  V.eat(",");
                }
                es.fields[fname] = fs;
              }
            }
            E.eat(",");
          }
          vec.push_back(std::move(es));
        }
        L.eat(",");
      }
    } else {
      c.balanced();
    }
    c.eat(",");
  }
  return true;
}

std::vector<std::string> split_quoted(const std::string& blob) {
  std::vector<std::string> out;
  Cur c(blob);
  while (true) {
    c.skip();
    if (c.i >= blob.size() || c.s[c.i] != '"') break;
    out.push_back(c.string_val());
    c.eat(",");
  }
  return out;
}

bool Snapshot::save(const std::string& path) const {
  ensure_parent_dir(path);
  std::ofstream f(path, std::ios::binary);
  if (!f) { LERROR("cannot write %s", path.c_str()); return false; }
  f << to_json();
  return f.good();
}

bool Snapshot::load(const std::string& path, Snapshot& out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (err) *err = "cannot open " + path; return false; }
  std::stringstream ss;
  ss << f.rdbuf();
  return from_json(ss.str(), out, err);
}

Snapshot capture(Runtime& rt) {
  Snapshot s;
  const Descriptor& d = rt.descriptor();
  s.descriptor = d.name;
  s.transport = rt.mem().kind();
  s.taken_at = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  const Resolution& r = rt.resolution();
  for (const auto& m : r.modules) {
    if (!s.module_name.empty()) s.module_name += ", ";
    s.module_name += m.name;
    if (m.base) s.module_base = m.base;
  }

  for (const auto& m : r.modules)
    if (m.health != Health::kOk)
      s.notes.push_back("module " + m.name + ": " + m.note);
  for (const auto& l : r.lists) {
    if (l.health != Health::kOk) s.notes.push_back("list " + l.name + ": " + l.note);
    for (const auto& kv : l.fields) {
      s.field_health[l.name + "." + kv.first] = kv.second.health;
      if (kv.second.health != Health::kOk)
        s.notes.push_back(l.name + "." + kv.first + ": " + kv.second.note);
    }
  }

  for (const auto& ls : d.lists) {
    const std::vector<Entity>& ents = rt.list(ls.name);
    std::vector<EntitySample>& vec = s.lists[ls.name];
    vec.reserve(ents.size());
    for (const auto& e : ents) {
      EntitySample es;
      es.index = e.index;
      es.addr = e.addr;
      for (const auto& kv : e.fields) {
        FieldSample fs;
        fs.index = e.index;
        fs.type = kv.first;
        fs.value = kv.second.to_string();
        fs.addr = e.addr;
        fs.readable = kv.second.t != Value::T::kNone;
        for (const auto& rl : r.lists) {
          auto it = rl.fields.find(kv.first);
          if (it != rl.fields.end() && rl.name == ls.name) {
            fs.addr = it->second.addr + static_cast<uintptr_t>(e.index * 0);
            fs.health = it->second.health;
          }
        }
        es.fields[kv.first] = fs;
      }
      vec.push_back(std::move(es));
    }
    s.entity_count += static_cast<int>(ents.size());
  }
  return s;
}

int SnapshotDiff::broken_count() const {
  int n = 0;
  for (const auto& l : lists)
    for (const auto& f : l.fields)
      if (f.delta == FieldDelta::kVanished || f.delta == FieldDelta::kHealthWorse) ++n;
  return n;
}

SnapshotDiff diff(const Snapshot& a, const Snapshot& b) {
  SnapshotDiff d;

  if (a.module_name != b.module_name || a.module_base != b.module_base) {
    d.module_changes.push_back("module moved: \"" + a.module_name + "\"@0x" +
                               std::to_string((unsigned long long)a.module_base) + " -> \"" +
                               b.module_name + "\"@0x" +
                               std::to_string((unsigned long long)b.module_base));
  }
  for (const auto& kv : a.field_health) {
    auto it = b.field_health.find(kv.first);
    if (it == b.field_health.end()) {
      d.module_changes.push_back("field removed from descriptor: " + kv.first);
      continue;
    }
    if (it->second != kv.second) {
      d.module_changes.push_back("field health " + kv.first + ": " +
                                 std::string(kv.second == Health::kOk ? "ok" : "broken") +
                                 " -> " + (it->second == Health::kOk ? "ok" : "broken"));
    }
  }

  for (const auto& bk : b.lists) {
    const std::string& lname = bk.first;
    auto ait = a.lists.find(lname);
    ListDiff ld;
    ld.list = lname;
    ld.count_after = static_cast<int>(bk.second.size());
    if (ait == a.lists.end()) {
      ld.count_before = 0;
      // bk.second is the loop variable's list and is non-empty by construction
      // (the outer range-for only yields keys with at least one entity), but a
      // defensive check keeps this correct if that ever changes.
      if (!bk.second.empty()) {
        for (const auto& fk : bk.second.front().fields) {
          FieldDiff fd;
          fd.field = fk.first;
          fd.delta = FieldDelta::kAppeared;
          fd.after = fk.second.value;
          fd.sample_count = static_cast<int>(bk.second.size());
          ld.fields.push_back(std::move(fd));
        }
      }
      d.lists.push_back(std::move(ld));
      continue;
    }
    ld.count_before = static_cast<int>(ait->second.size());

    // The list resolved before and does not resolve now. That is a hard break,
    // and it is the single most common outcome after a patch - report it as
    // such rather than as a pile of field-level "vanished" lines, which read as
    // if each field had independently drifted.
    if (bk.second.empty()) {
      FieldDiff fd;
      fd.field = "*";
      fd.delta = FieldDelta::kVanished;
      fd.before = "list " + lname + " resolved with " +
                 std::to_string(ait->second.size()) + " entities";
      fd.after = "list " + lname + " resolves to nothing";
      fd.sample_count = static_cast<int>(ait->second.size());
      ld.fields.push_back(std::move(fd));
      d.lists.push_back(std::move(ld));
      continue;
    }

    // Union of field names across both sides. Either list can be empty - a
    // descriptor that resolved before and lost its schema after the patch is
    // the whole point of this tool - so .front() is only legal when non-empty.
    std::vector<std::string> names;
    if (!ait->second.empty())
      for (const auto& fk : ait->second.front().fields) names.push_back(fk.first);
    if (!bk.second.empty()) {
      for (const auto& fk : bk.second.front().fields)
        if (std::find(names.begin(), names.end(), fk.first) == names.end()) names.push_back(fk.first);
    }

    for (const auto& name : names) {
      FieldDiff fd;
      fd.field = name;
      const bool in_a = !ait->second.empty() && ait->second.front().fields.count(name);
      const bool in_b = !bk.second.empty() && bk.second.front().fields.count(name);
      if (!in_a && in_b) {
        // Carry the value across so the report can show what appeared, not just
        // that something did.
        fd.delta = FieldDelta::kAppeared;
        auto y = bk.second.front().fields.find(name);
        if (y != bk.second.front().fields.end()) {
          fd.after = y->second.value;
          fd.addr_after = y->second.addr;
        }
        fd.sample_count = static_cast<int>(bk.second.size());
      } else if (in_a && !in_b) {
        // The field is declared but no longer reads. That is a drifted offset,
        // not a missing declaration, so the report leads with what it last read
        // and says plainly that it is gone now.
        fd.delta = FieldDelta::kVanished;
        auto x = ait->second.front().fields.find(name);
        if (x != ait->second.front().fields.end()) {
          fd.before = x->second.value.empty() ? "(unreadable at last capture)"
                                              : x->second.value;
          fd.addr_before = x->second.addr;
        }
        fd.after = "field no longer resolves";
        fd.sample_count = static_cast<int>(ait->second.size());
      } else {
        int changed = 0, moved = 0, worse = 0, n = 0;
        std::string before, after;
        for (size_t i = 0; i < std::max(ait->second.size(), bk.second.size()); ++i) {
          if (i >= ait->second.size() || i >= bk.second.size()) continue;
          const FieldSample* fa = nullptr;
          const FieldSample* fb = nullptr;
          auto x = ait->second[i].fields.find(name);
          auto y = bk.second[i].fields.find(name);
          if (x != ait->second[i].fields.end()) fa = &x->second;
          if (y != bk.second[i].fields.end()) fb = &y->second;
          if (!fa || !fb) continue;
          ++n;
          if (fa->value != fb->value) {
            ++changed;
            if (before.empty()) before = fa->value;
            if (after.empty()) after = fb->value;
          }
          if (fa->addr != fb->addr) {
            ++moved;
            fd.addr_before = fa->addr;
            fd.addr_after = fb->addr;
          }
          if (fb->health != Health::kOk && fb->health != fa->health) ++worse;
        }
        fd.sample_count = n;
        fd.before = before;
        fd.after = after;
        if (worse > 0) fd.delta = FieldDelta::kHealthWorse;
        else if (moved > 0) fd.delta = FieldDelta::kAddressMoved;
        else if (changed > 0) fd.delta = FieldDelta::kChanged;
        else fd.delta = FieldDelta::kSame;
      }
      if (fd.delta != FieldDelta::kSame) ld.fields.push_back(std::move(fd));
    }
    std::sort(ld.fields.begin(), ld.fields.end(), [](const FieldDiff& x, const FieldDiff& y) {
      auto rank = [](FieldDelta d) {
        switch (d) {
          case FieldDelta::kVanished: return 0;
          case FieldDelta::kHealthWorse: return 1;
          case FieldDelta::kAppeared: return 2;
          case FieldDelta::kAddressMoved: return 3;
          case FieldDelta::kChanged: return 4;
          default: return 5;
        }
      };
      return rank(x.delta) < rank(y.delta);
    });
    d.lists.push_back(std::move(ld));
  }
  return d;
}

std::string SnapshotDiff::report() const {
  if (identical() && module_changes.empty()) return "no change: every field resolved and matched\n";

  std::string o;
  o += "=== descriptor diff ===\n";
  for (const auto& m : module_changes) o += "  ! " + m + "\n";
  for (const auto& l : lists) {
    if (l.count_before != l.count_after)
      o += "  ~ " + l.list + ": entity count " + std::to_string(l.count_before) + " -> " +
           std::to_string(l.count_after) + "\n";
    for (const auto& f : l.fields) {
      // Tags are padded to one width so the "list.field" column lines up and a
      // scan down the report reads as a status column rather than a ragged wall.
      const char* tag = "  same   ";
      switch (f.delta) {
        case FieldDelta::kVanished: tag = "  BROKEN "; break;
        case FieldDelta::kHealthWorse: tag = "  WEAKER "; break;
        case FieldDelta::kAppeared: tag = "  NEW    "; break;
        case FieldDelta::kAddressMoved: tag = "  MOVED  "; break;
        case FieldDelta::kChanged: tag = "  CHANGED"; break;
        default: break;
      }
      o += std::string(tag) + " " + l.list + "." + f.field + ": ";
      switch (f.delta) {
        case FieldDelta::kVanished:
          o += f.before.empty() ? "resolved before, dead now"
                                : f.before + " -> " + f.after;
          break;
        case FieldDelta::kAppeared:
          o += "dead before, resolves now (" + f.after + ")";
          break;
        case FieldDelta::kAddressMoved:
          o += "address 0x" + std::to_string((unsigned long long)f.addr_before) + " -> 0x" +
               std::to_string((unsigned long long)f.addr_after) + " over " +
               std::to_string(f.sample_count) + " samples";
          break;
        case FieldDelta::kChanged:
          o += "\"" + f.before + "\" -> \"" + f.after + "\" (" + std::to_string(f.sample_count) +
               " samples differ)";
          break;
        case FieldDelta::kHealthWorse:
          o += "health degraded (" + std::to_string(f.sample_count) + " samples)";
          break;
        default:
          o += "unchanged";
      }
      o += "\n";
    }
  }

  // Two counts, because they answer different questions. A hard break is a
  // field that stopped resolving (a patch landed). A value change with no hard
  // break is the descriptor still working and the target's data moving — which
  // is what a live game looks like when nothing is broken.
  int changed = 0, appeared = 0, moved = 0;
  for (const auto& l : lists) {
    for (const auto& f : l.fields) {
      switch (f.delta) {
        case FieldDelta::kChanged: ++changed; break;
        case FieldDelta::kAppeared: ++appeared; break;
        case FieldDelta::kAddressMoved: ++moved; break;
        default: break;
      }
    }
  }
  o += "=== end: " + std::to_string(broken_count()) + " hard break(s), " +
       std::to_string(changed) + " value change(s), " + std::to_string(appeared) +
       " appeared, " + std::to_string(moved) + " moved ===\n";
  return o;
}

}  // namespace lens
