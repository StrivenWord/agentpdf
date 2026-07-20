#pragma once

#include "agentpdf/types.hpp"

#include <map>
#include <string>
#include <vector>

namespace agentpdf {

namespace json_mini {
struct Value;
bool parse_object_file(const std::string& path, std::map<std::string, std::string>& scalars,
                       std::map<std::string, std::vector<std::string>>& arrays,
                       std::string& err);
bool write_object_file(const std::string& path,
                       const std::map<std::string, std::string>& scalars,
                       const std::map<std::string, std::vector<std::string>>& arrays,
                       const std::map<std::string, double>& numbers,
                       const std::map<std::string, bool>& booleans, std::string& err);
}  // namespace json_mini

std::string trim(const std::string& s);
std::string collapse_ws(const std::string& s);
std::string normalize_typography(const std::string& s);
std::string to_lower(const std::string& s);
std::vector<std::string> split_words(const std::string& s);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string u8_from_poppler(const std::string& maybe_utf8);
bool looks_like_doi(const std::string& s);
std::string today_iso_date();
bool ensure_parent_dir(const std::string& path);
bool write_text_file(const std::string& path, const std::string& content, std::string& err);
std::string read_text_file(const std::string& path, std::string& err);
std::string yaml_escape(const std::string& s);
std::string stem_filename(const std::string& path);
std::string replace_extension(const std::string& path, const std::string& new_ext);

}  // namespace agentpdf
