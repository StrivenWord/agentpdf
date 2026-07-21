#include "agentpdf/config.hpp"
#include "agentpdf/util.hpp"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace agentpdf {

namespace {

double as_double(const std::map<std::string, std::string>& scalars, const std::string& key,
                 double fallback) {
  auto it = scalars.find(key);
  if (it == scalars.end()) return fallback;
  try {
    return std::stod(it->second);
  } catch (...) {
    return fallback;
  }
}

int as_int(const std::map<std::string, std::string>& scalars, const std::string& key, int fallback) {
  auto it = scalars.find(key);
  if (it == scalars.end()) return fallback;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return fallback;
  }
}

bool as_bool(const std::map<std::string, std::string>& scalars, const std::string& key,
             bool fallback) {
  auto it = scalars.find(key);
  if (it == scalars.end()) return fallback;
  auto v = to_lower(it->second);
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return fallback;
}

}  // namespace

std::string default_config_dir() {
  if (const char* env = std::getenv("AGENTPDF_CONFIG_DIR")) {
    return env;
  }
  // Prefer config next to the binary's CWD build tree, then source-relative.
  if (fs::exists("config/heuristics.json")) return "config";
  if (fs::exists("../config/heuristics.json")) return "../config";
  return "config";
}

std::string resolve_config_path(const std::string& explicit_path, const std::string& filename) {
  if (!explicit_path.empty()) return explicit_path;
  return (fs::path(default_config_dir()) / filename).string();
}

bool load_heuristics(const std::string& path, Heuristics& out, std::string& err) {
  std::map<std::string, std::string> scalars;
  std::map<std::string, std::vector<std::string>> arrays;
  if (!json_mini::parse_object_file(path, scalars, arrays, err)) return false;
  out.header_band_frac = as_double(scalars, "header_band_frac", out.header_band_frac);
  out.footer_band_frac = as_double(scalars, "footer_band_frac", out.footer_band_frac);
  out.column_gap_min_pts = as_double(scalars, "column_gap_min_pts", out.column_gap_min_pts);
  out.skew_tolerance_deg = as_double(scalars, "skew_tolerance_deg", out.skew_tolerance_deg);
  out.min_text_layer_chars_per_page =
      as_double(scalars, "min_text_layer_chars_per_page", out.min_text_layer_chars_per_page);
  out.paragraph_gap_pts = as_double(scalars, "paragraph_gap_pts", out.paragraph_gap_pts);
  out.line_merge_y_tol_pts = as_double(scalars, "line_merge_y_tol_pts", out.line_merge_y_tol_pts);
  out.margin_overlay_max_x_frac =
      as_double(scalars, "margin_overlay_max_x_frac", out.margin_overlay_max_x_frac);
  out.sidebar_max_width_frac =
      as_double(scalars, "sidebar_max_width_frac", out.sidebar_max_width_frac);
  out.footnote_zone_start_frac =
      as_double(scalars, "footnote_zone_start_frac", out.footnote_zone_start_frac);
  out.min_text_quality = as_double(scalars, "min_text_quality", out.min_text_quality);
  out.rejoin_hyphenation = as_bool(scalars, "rejoin_hyphenation", out.rejoin_hyphenation);
  out.strip_running_headers = as_bool(scalars, "strip_running_headers", out.strip_running_headers);
  out.strip_page_numbers = as_bool(scalars, "strip_page_numbers", out.strip_page_numbers);
  out.dedupe_title_from_body = as_bool(scalars, "dedupe_title_from_body", out.dedupe_title_from_body);
  out.abstract_as_h1 = as_bool(scalars, "abstract_as_h1", out.abstract_as_h1);
  out.keywords_as_h2 = as_bool(scalars, "keywords_as_h2", out.keywords_as_h2);
  out.nest_numeric_headings = as_bool(scalars, "nest_numeric_headings", out.nest_numeric_headings);
  out.footnotes_to_endnotes = as_bool(scalars, "footnotes_to_endnotes", out.footnotes_to_endnotes);
  out.ocr_when_scan_present =
      as_bool(scalars, "ocr_when_scan_present", out.ocr_when_scan_present);
  out.ocr_dpi = as_int(scalars, "ocr_dpi", out.ocr_dpi);
  out.ocr_workers = as_int(scalars, "ocr_workers", out.ocr_workers);
  if (auto it = scalars.find("tesseract_lang"); it != scalars.end()) out.tesseract_lang = it->second;
  return true;
}

bool load_metadata_spec(const std::string& path, MetadataSpec& out, std::string& err) {
  std::map<std::string, std::string> scalars;
  std::map<std::string, std::vector<std::string>> arrays;
  if (!json_mini::parse_object_file(path, scalars, arrays, err)) return false;
  if (auto it = arrays.find("required_fields"); it != arrays.end()) out.required_fields = it->second;
  if (auto it = arrays.find("optional_fields"); it != arrays.end()) out.optional_fields = it->second;
  return true;
}

bool save_heuristics(const std::string& path, const Heuristics& h, std::string& err) {
  std::map<std::string, std::string> scalars{{"tesseract_lang", h.tesseract_lang}};
  std::map<std::string, std::vector<std::string>> arrays;
  std::map<std::string, double> numbers{
      {"header_band_frac", h.header_band_frac},
      {"footer_band_frac", h.footer_band_frac},
      {"column_gap_min_pts", h.column_gap_min_pts},
      {"skew_tolerance_deg", h.skew_tolerance_deg},
      {"min_text_layer_chars_per_page", h.min_text_layer_chars_per_page},
      {"paragraph_gap_pts", h.paragraph_gap_pts},
      {"line_merge_y_tol_pts", h.line_merge_y_tol_pts},
      {"margin_overlay_max_x_frac", h.margin_overlay_max_x_frac},
      {"sidebar_max_width_frac", h.sidebar_max_width_frac},
      {"footnote_zone_start_frac", h.footnote_zone_start_frac},
      {"min_text_quality", h.min_text_quality},
      {"ocr_dpi", static_cast<double>(h.ocr_dpi)},
      {"ocr_workers", static_cast<double>(h.ocr_workers)},
  };
  std::map<std::string, bool> booleans{
      {"rejoin_hyphenation", h.rejoin_hyphenation},
      {"strip_running_headers", h.strip_running_headers},
      {"strip_page_numbers", h.strip_page_numbers},
      {"dedupe_title_from_body", h.dedupe_title_from_body},
      {"abstract_as_h1", h.abstract_as_h1},
      {"keywords_as_h2", h.keywords_as_h2},
      {"nest_numeric_headings", h.nest_numeric_headings},
      {"footnotes_to_endnotes", h.footnotes_to_endnotes},
      {"ocr_when_scan_present", h.ocr_when_scan_present},
  };
  return json_mini::write_object_file(path, scalars, arrays, numbers, booleans, err);
}

}  // namespace agentpdf
