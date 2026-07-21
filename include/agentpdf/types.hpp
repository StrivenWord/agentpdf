#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agentpdf {

enum class RegionKind {
  Body,
  Header,
  Footer,
  MarginOverlay,
  Sidebar,
  Float,
  Footnote,
  AuthorNote,
  Wrapper,
  Metadata
};

enum class LayoutFamily {
  Generic,
  MagazineTwoColumn,
  AcmConferenceTwoColumn,
  FrontiersRail,
  ScanOcrTwoColumn
};

struct BBox {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double width() const { return x1 - x0; }
  double height() const { return y1 - y0; }
  double cx() const { return (x0 + x1) * 0.5; }
  double cy() const { return (y0 + y1) * 0.5; }
};

struct RegionOverride {
  RegionKind role = RegionKind::Float;
  BBox box;
  bool fractional = false;
  bool top_origin = true;
};

struct PageOverride {
  int page = 0;
  std::optional<double> column_cut;
  bool cut_fractional = false;
  bool cut_top_origin = true;
  bool keep_captions = false;
  std::vector<RegionOverride> regions;
};

struct Heuristics {
  double header_band_frac = 0.06;
  double footer_band_frac = 0.06;
  double column_gap_min_pts = 18.0;
  double skew_tolerance_deg = 2.0;
  double min_text_layer_chars_per_page = 40.0;
  double paragraph_gap_pts = 8.0;
  double line_merge_y_tol_pts = 2.5;
  double margin_overlay_max_x_frac = 0.045;
  double sidebar_max_width_frac = 0.30;
  double footnote_zone_start_frac = 0.82;
  double min_text_quality = 0.72;
  bool rejoin_hyphenation = true;
  bool strip_running_headers = true;
  bool strip_page_numbers = true;
  bool dedupe_title_from_body = true;
  bool abstract_as_h1 = true;
  bool keywords_as_h2 = true;
  bool nest_numeric_headings = true;
  bool footnotes_to_endnotes = true;
  bool ocr_when_scan_present = true;
  int ocr_dpi = 300;
  int ocr_workers = 2;
  std::string tesseract_lang = "eng";
  std::vector<std::string> page_overrides_raw;
  std::vector<PageOverride> page_overrides;
};

struct MetadataSpec {
  std::vector<std::string> required_fields{
      "title", "authors", "doi", "date-published", "source-format"};
  std::vector<std::string> optional_fields{
      "publisher",       "keywords",        "abstract",
      "date-received",   "date-accepted",   "pages",
      "type",            "object-url",      "published-formats",
      "date-extracted",  "date-accessed"};
};

struct DocumentMeta {
  std::string title;
  std::vector<std::string> authors;
  std::string doi;
  std::string date_published;
  std::string date_received;
  std::string date_accepted;
  std::string date_extracted;
  std::string date_accessed;
  std::string publisher;
  std::string source_format{"PDF"};
  std::string published_formats{"PDF"};
  std::string object_url;
  std::string type;
  std::string pages;
  std::vector<std::string> keywords;
  std::string abstract_text;
};

struct NormalizedTextBox {
  std::string text;
  BBox box;
  double font_size = 0;
  int rotation = 0;
  int column = 0;
  RegionKind region = RegionKind::Body;
};

enum class BlockKind {
  Paragraph,
  Heading,
  ListItem,
  Caption,
  Footnote,
  Table,
  Boilerplate,
  FigureRedaction
};

struct TextSpan {
  std::string text;
  BBox box;
  bool bold = false;
  bool italic = false;
};

struct TextLine {
  std::string text;
  BBox box;
  bool bold = false;
  bool italic = false;
};

struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int heading_level = 0;
  std::string text;
  BBox box;
  int column = 0;
  int page = 0;
  std::vector<std::vector<std::string>> table_rows;
};

struct PageDom {
  int index = 0;
  double width = 0;
  double height = 0;
  bool used_ocr = false;
  bool wrapper_page = false;
  bool keep_captions = false;
  bool has_region_overrides = false;
  LayoutFamily layout_family = LayoutFamily::Generic;
  double text_quality = 1.0;
  std::optional<double> column_cut_override;
  std::vector<NormalizedTextBox> normalized_boxes;
  std::vector<TextLine> lines;
  std::vector<Block> blocks;
};

struct DocumentDom {
  std::string source_path;
  DocumentMeta meta;
  std::vector<PageDom> pages;
  std::vector<std::string> endnotes;
  int heading_count = 0;
  int table_count = 0;
  int figure_count = 0;
  int footnote_count = 0;
};

struct JobEntry {
  std::string input_path;
  std::string relative_path;
  std::string filename;
  std::uintmax_t size_bytes = 0;
  std::string output_path;
};

struct CliOptions {
  bool interactive = false;
  bool show_help = false;
  std::string command;  // convert | help
  std::vector<std::string> inputs;
  std::string output_dir;
  std::string heuristics_path;
  std::string metadata_path;
  bool recursive = true;
};

}  // namespace agentpdf
