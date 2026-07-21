#include "agentpdf/config.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

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

RegionKind region_kind_from_name(const std::string& name) {
  const auto low = to_lower(name);
  if (low == "header") return RegionKind::Header;
  if (low == "footer") return RegionKind::Footer;
  if (low == "sidebar") return RegionKind::Sidebar;
  if (low == "footnote") return RegionKind::Footnote;
  if (low == "float") return RegionKind::Float;
  if (low == "margin" || low == "marginoverlay" || low == "margin_overlay")
    return RegionKind::MarginOverlay;
  if (low == "metadata") return RegionKind::Metadata;
  if (low == "wrapper") return RegionKind::Wrapper;
  if (low == "authornote" || low == "author_note") return RegionKind::AuthorNote;
  return RegionKind::Float;
}

std::vector<std::string> split_semi(const std::string& value) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : value) {
    if (c == ';') {
      auto piece = trim(cur);
      if (!piece.empty()) parts.push_back(piece);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  auto piece = trim(cur);
  if (!piece.empty()) parts.push_back(piece);
  return parts;
}

bool parse_box(const std::string& text, BBox& out) {
  std::stringstream ss(text);
  std::string token;
  double vals[4];
  int n = 0;
  while (std::getline(ss, token, ',') && n < 4) {
    try {
      vals[n++] = std::stod(trim(token));
    } catch (...) {
      return false;
    }
  }
  if (n != 4) return false;
  out = BBox{vals[0], vals[1], vals[2], vals[3]};
  return true;
}

bool parse_page_override_entry(const std::string& raw, PageOverride& out,
                               std::string& err) {
  out = PageOverride{};
  bool have_page = false;
  bool fractional = false;
  bool top_origin = true;
  RegionKind pending_role = RegionKind::Float;
  bool have_region = false;
  BBox pending_box;
  bool have_box = false;

  for (const auto& part : split_semi(raw)) {
    const auto eq = part.find('=');
    if (eq == std::string::npos) {
      err = "page_overrides entry missing '=': " + part;
      return false;
    }
    const auto key = to_lower(trim(part.substr(0, eq)));
    const auto value = trim(part.substr(eq + 1));
    if (key == "page") {
      try {
        out.page = std::stoi(value);
        have_page = true;
      } catch (...) {
        err = "invalid page in page_overrides: " + value;
        return false;
      }
    } else if (key == "units") {
      const auto low = to_lower(value);
      if (low == "frac" || low == "fraction" || low == "relative")
        fractional = true;
      else if (low == "pt" || low == "points" || low == "point")
        fractional = false;
      else {
        err = "invalid units in page_overrides: " + value;
        return false;
      }
    } else if (key == "origin") {
      const auto low = to_lower(value);
      if (low == "topleft" || low == "top-left" || low == "tl")
        top_origin = true;
      else if (low == "bottomleft" || low == "bottom-left" || low == "bl")
        top_origin = false;
      else {
        err = "invalid origin in page_overrides: " + value;
        return false;
      }
    } else if (key == "region") {
      pending_role = region_kind_from_name(value);
      have_region = true;
    } else if (key == "box") {
      if (!parse_box(value, pending_box)) {
        err = "invalid box in page_overrides: " + value;
        return false;
      }
      have_box = true;
    } else if (key == "column_cut" || key == "column-cut") {
      try {
        out.column_cut = std::stod(value);
      } catch (...) {
        err = "invalid column_cut in page_overrides: " + value;
        return false;
      }
    } else if (key == "keep_captions" || key == "keep-captions") {
      const auto low = to_lower(value);
      out.keep_captions = (low == "true" || low == "1" || low == "yes");
    } else {
      err = "unknown page_overrides key: " + key;
      return false;
    }
  }

  if (!have_page) {
    err = "page_overrides entry requires page=: " + raw;
    return false;
  }
  out.cut_fractional = fractional;
  out.cut_top_origin = top_origin;
  if (have_box || have_region) {
    if (!have_box) {
      err = "page_overrides region requires box=: " + raw;
      return false;
    }
    RegionOverride region;
    region.role = have_region ? pending_role : RegionKind::Float;
    region.box = pending_box;
    region.fractional = fractional;
    region.top_origin = top_origin;
    out.regions.push_back(region);
  }
  return true;
}

void parse_page_overrides(Heuristics& out) {
  out.page_overrides.clear();
  for (const auto& raw : out.page_overrides_raw) {
    PageOverride parsed;
    std::string err;
    if (!parse_page_override_entry(raw, parsed, err)) {
      std::cerr << "warning: skipping page_overrides entry (" << err << ")\n";
      continue;
    }
    auto existing = std::find_if(out.page_overrides.begin(), out.page_overrides.end(),
                                 [&](const PageOverride& p) { return p.page == parsed.page; });
    if (existing == out.page_overrides.end()) {
      out.page_overrides.push_back(std::move(parsed));
      continue;
    }
    if (parsed.column_cut) {
      existing->column_cut = parsed.column_cut;
      existing->cut_fractional = parsed.cut_fractional;
      existing->cut_top_origin = parsed.cut_top_origin;
    }
    if (parsed.keep_captions) existing->keep_captions = true;
    for (auto& region : parsed.regions) existing->regions.push_back(std::move(region));
  }
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
  if (auto it = arrays.find("page_overrides"); it != arrays.end()) {
    out.page_overrides_raw = it->second;
  }
  parse_page_overrides(out);
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
  std::map<std::string, std::vector<std::string>> arrays{
      {"page_overrides", h.page_overrides_raw},
  };
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
