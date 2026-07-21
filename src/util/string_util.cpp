#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace agentpdf {

std::string trim(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string collapse_ws(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      if (!in_ws && !out.empty()) out.push_back(' ');
      in_ws = true;
    } else {
      out.push_back(static_cast<char>(c));
      in_ws = false;
    }
  }
  return trim(out);
}

std::string normalize_typography(const std::string& s) {
  // Map common UTF-8 punctuation to ASCII so word-order compares cleanly.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    // UTF-8 multi-byte
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
      unsigned cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
      if (cp == 0x00A0) out.push_back(' ');  // nbsp
      else {
        out.push_back(s[i]);
        out.push_back(s[i + 1]);
      }
      i += 2;
      continue;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
      unsigned cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(s[i + 2]) & 0x3F);
      if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) out.push_back('\'');
      else if (cp == 0x201C || cp == 0x201D) out.push_back('"');
      else if (cp == 0x2013 || cp == 0x2014) {
        out += " - ";  // en/em dash as spaced hyphen so words stay separate
      } else if (cp == 0x2026) {
        out += "...";
      } else if (cp == 0x00AD) {
        // soft hyphen: drop
      } else {
        out.push_back(s[i]);
        out.push_back(s[i + 1]);
        out.push_back(s[i + 2]);
      }
      i += 3;
      continue;
    }
    out.push_back(static_cast<char>(c));
    ++i;
  }
  for (size_t pos = 0; (pos = out.find("''", pos)) != std::string::npos;) {
    out.replace(pos, 2, "\"");
    ++pos;
  }
  for (size_t pos = 0; (pos = out.find("``", pos)) != std::string::npos;) {
    out.replace(pos, 2, "\"");
    ++pos;
  }
  return out;
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> words;
  std::string cur;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '\'' || c == '-' || static_cast<unsigned char>(c) >= 128) {
      cur.push_back(static_cast<char>(c));
    } else {
      if (!cur.empty()) {
        words.push_back(cur);
        cur.clear();
      }
    }
  }
  if (!cur.empty()) words.push_back(cur);
  return words;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::ostringstream oss;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) oss << sep;
    oss << parts[i];
  }
  return oss.str();
}

std::string u8_from_poppler(const std::string& maybe_utf8) { return maybe_utf8; }

bool looks_like_doi(const std::string& s) {
  auto t = to_lower(trim(s));
  return t.find("10.") == 0 || t.find("doi:") != std::string::npos ||
         t.find("doi.org/") != std::string::npos;
}

std::string today_iso_date() {
  using clock = std::chrono::system_clock;
  auto t = clock::to_time_t(clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday);
  return buf;
}

bool ensure_parent_dir(const std::string& path) {
  fs::path p(path);
  if (!p.has_parent_path()) return true;
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  return !ec;
}

bool write_text_file(const std::string& path, const std::string& content, std::string& err) {
  if (!ensure_parent_dir(path)) {
    err = "failed to create parent directory for " + path;
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    err = "failed to open for write: " + path;
    return false;
  }
  out << content;
  if (!out) {
    err = "failed while writing: " + path;
    return false;
  }
  return true;
}

std::string read_text_file(const std::string& path, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "failed to open: " + path;
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

std::string yaml_escape(const std::string& s) {
  bool needs_quotes = s.empty() || s.find_first_of(":\"'#{}[],&*?|>!%@`\\") != std::string::npos ||
                      s.front() == ' ' || s.back() == ' ' || s.find('\n') != std::string::npos;
  if (!needs_quotes) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string stem_filename(const std::string& path) {
  return fs::path(path).stem().string();
}

std::string replace_extension(const std::string& path, const std::string& new_ext) {
  fs::path p(path);
  p.replace_extension(new_ext);
  return p.string();
}

namespace json_mini {
namespace {

std::string skip_ws(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return {};
}

bool parse_string(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  out.clear();
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return true;
    if (c == '\\' && i < s.size()) {
      char e = s[i++];
      if (e == 'n') out.push_back('\n');
      else if (e == 't') out.push_back('\t');
      else out.push_back(e);
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool parse_number_or_bool(const std::string& s, size_t& i, std::string& out) {
  size_t start = i;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != ',' &&
         s[i] != '}' && s[i] != ']') {
    ++i;
  }
  out = s.substr(start, i - start);
  return !out.empty();
}

}  // namespace

bool parse_object_file(const std::string& path, std::map<std::string, std::string>& scalars,
                       std::map<std::string, std::vector<std::string>>& arrays,
                       std::string& err) {
  std::string raw = read_text_file(path, err);
  if (!err.empty() && raw.empty()) return false;
  err.clear();
  size_t i = 0;
  skip_ws(raw, i);
  if (i >= raw.size() || raw[i] != '{') {
    err = "expected JSON object in " + path;
    return false;
  }
  ++i;
  while (true) {
    skip_ws(raw, i);
    if (i < raw.size() && raw[i] == '}') {
      ++i;
      break;
    }
    std::string key;
    if (!parse_string(raw, i, key)) {
      err = "bad key in " + path;
      return false;
    }
    skip_ws(raw, i);
    if (i >= raw.size() || raw[i] != ':') {
      err = "expected ':' after key " + key;
      return false;
    }
    ++i;
    skip_ws(raw, i);
    if (i < raw.size() && raw[i] == '"') {
      std::string val;
      if (!parse_string(raw, i, val)) {
        err = "bad string value for " + key;
        return false;
      }
      scalars[key] = val;
    } else if (i < raw.size() && raw[i] == '[') {
      ++i;
      std::vector<std::string> arr;
      while (true) {
        skip_ws(raw, i);
        if (i < raw.size() && raw[i] == ']') {
          ++i;
          break;
        }
        std::string val;
        if (i < raw.size() && raw[i] == '"') {
          if (!parse_string(raw, i, val)) {
            err = "bad array string in " + key;
            return false;
          }
        } else {
          if (!parse_number_or_bool(raw, i, val)) {
            err = "bad array value in " + key;
            return false;
          }
        }
        arr.push_back(val);
        skip_ws(raw, i);
        if (i < raw.size() && raw[i] == ',') ++i;
      }
      arrays[key] = std::move(arr);
    } else {
      std::string val;
      if (!parse_number_or_bool(raw, i, val)) {
        err = "bad value for " + key;
        return false;
      }
      scalars[key] = val;
    }
    skip_ws(raw, i);
    if (i < raw.size() && raw[i] == ',') ++i;
  }
  return true;
}

bool write_object_file(const std::string& path,
                       const std::map<std::string, std::string>& scalars,
                       const std::map<std::string, std::vector<std::string>>& arrays,
                       const std::map<std::string, double>& numbers,
                       const std::map<std::string, bool>& booleans, std::string& err) {
  std::ostringstream oss;
  oss << "{\n";
  bool first = true;
  auto comma = [&] {
    if (!first) oss << ",\n";
    first = false;
  };
  for (const auto& [k, v] : numbers) {
    comma();
    oss << "  \"" << k << "\": " << v;
  }
  for (const auto& [k, v] : booleans) {
    comma();
    oss << "  \"" << k << "\": " << (v ? "true" : "false");
  }
  for (const auto& [k, v] : scalars) {
    comma();
    oss << "  \"" << k << "\": \"" << v << "\"";
  }
  for (const auto& [k, arr] : arrays) {
    comma();
    oss << "  \"" << k << "\": [";
    for (size_t i = 0; i < arr.size(); ++i) {
      if (i) oss << ", ";
      oss << "\"" << arr[i] << "\"";
    }
    oss << "]";
  }
  oss << "\n}\n";
  return write_text_file(path, oss.str(), err);
}

}  // namespace json_mini

}  // namespace agentpdf
