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
  Metadata,
  Caption  // a float's caption: kept, set between paragraphs
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

// One canonical frontmatter field (see config/metadata.json and
// documentation/metadata-fields.md). Keys are lowercase kebab-case, named
// after the CSL variable Zotero exports the field to, or after the
// Pandoc/Obsidian property for the same concept.
struct FieldSpec {
  std::string key;            // YAML property name
  std::string obsidian_type;  // text | multitext | number | date | aliases | tags
  std::string zotero;         // Zotero field ("—" when none)
  std::string csl;            // CSL variable ("—" when none)
  // Where a value can come from today: "pdf" (identifiable from the PDF),
  // "agentpdf" (run provenance), "zotero" or "caller" (not available from a
  // PDF: listed in the template but never emitted until such an input exists).
  std::string source;
  bool emitted() const { return source == "pdf" || source == "agentpdf"; }
};

struct MetadataSpec {
  std::vector<FieldSpec> fields;  // output order
  std::string origin = "built-in";
};

// Every value agentpdf can derive from a PDF. Empty strings, zero numbers and
// empty lists mean "not found": such keys are omitted from the frontmatter.
struct DocumentMeta {
  std::string title;
  std::string title_short;
  std::vector<std::string> authors;       // "Given Family", as printed
  std::string date;                       // ISO 8601 at known precision: YYYY[-MM[-DD]]
  int year = 0;
  std::string genre;                      // printed article type ("Original Research")
  std::string container_title;
  std::string container_title_short;
  std::string publisher;
  std::string volume;
  std::string issue;
  std::string page;                       // range "331-339" or article number
  int number_of_pages = 0;
  std::string language;                   // BCP 47 primary tag, detected from body text
  std::string abstract_text;
  std::vector<std::string> keywords;
  std::string license;                    // canonical Creative Commons URL
  std::string doi;
  std::vector<std::string> issn;
  std::vector<std::string> isbn;
  std::string arxiv;
  std::string url;
  std::string available_date;             // YYYY-MM-DD only
  std::string date_received;
  std::string date_accepted;
  std::vector<std::string> aliases;
  std::string agentpdf_extracted;
  std::string agentpdf_version;
  std::string agentpdf_source;
};

struct NormalizedTextBox {
  std::string text;
  BBox box;
  double font_size = 0;
  bool bold = false;    // from the font name; only collected for front-matter evidence
  bool italic = false;  // likewise
  int rotation = 0;
  int column = 0;
  RegionKind region = RegionKind::Body;
  // The word's type as set. Unlike font_size/bold/italic above, which body
  // classification runs without, these are kept for every page: line
  // typography and sub/superscript joins read them.
  double type_size = 0;
  bool type_heavy = false;
  bool type_italic = false;
  bool space_after = true;  // Poppler's own word spacing after this word
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

// Typographic evidence for one text-layer word, kept for every page: heading
// detection compares a line's type with the body text's. `folded` is the
// word's lowercase alphanumerics (fold_alnum), the key that aligns any line
// stream (flow, raw, geometry) back to its words.
struct StyledWord {
  std::string folded;
  double font_size = 0;
  bool heavy = false;   // bold, semibold, medium, black… weight
  bool italic = false;
};

struct TextLine {
  std::string text;
  BBox box;
  // Typography of the words the line was located at (annotate_line_typography).
  // font_size 0 means no evidence: OCR text, or a line not found in the text
  // layer; text cues alone then decide.
  double font_size = 0;      // character-weighted mean
  double font_size_max = 0;
  bool bold = false;
  bool italic = false;
  // Lines rebuilt from their own text-layer words (flow_box_lines) carry the
  // words' real page geometry here; `box` holds stream coordinates.
  BBox geom;
  bool has_geom = false;
  // Page geometry says a paragraph begins here: an indented first line, or
  // a line after a short sentence-final one.
  bool para_start = false;
  bool block_start = false;  // first line of one of Poppler's text blocks
  // A footnote line that opens a note: the note's printed key ("3", "*").
  std::string note_key;
  // Every word heavy; the smallest word size; and the byte length of a bold
  // phrase the line opens with before regular text (a run-in heading), 0
  // when there is none.
  bool bold_all = false;
  double font_size_min = 0;
  size_t runin_len = 0;
  bool gapped = false;  // words set apart by gaps wider than two ems (table cells)
  bool equation = false;  // a display equation, set apart from the text
  bool caption = false;   // a figure's or table's caption
};

struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int heading_level = 0;
  std::string text;
  BBox box;
  int column = 0;
  int page = 0;
  // Opens a numbered bibliography entry or list item: never joined to the
  // paragraph before it, however that one ends.
  bool entry_start = false;
  // Page geometry: the block's first line opens a paragraph (never joined to
  // the one before), or visibly continues the previous page's paragraph.
  bool para_start = false;
  bool continues = false;
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
  double detected_skew_deg = 0.0;  // Detected skew from Leptonica analysis (OCR pages only)
  std::optional<double> column_cut_override;
  std::vector<BBox> ocr_content_regions;  // Content bounding boxes from Leptonica
  // The document's body type (DocumentDom::body_font_size), known before
  // region classification; 0 for scans, whose text layer has one size.
  double body_font_size = 0;
  bool body_font_heavy = false;
  // x of each gutter between text columns (typed pages), left to right;
  // none for a page set in one column. Decided for the whole document
  // before classification.
  std::vector<double> gutters;
  bool gutter_decided = false;
  // A table runs to this page's foot / continues from the previous page.
  bool table_open_at_foot = false;
  bool table_continues = false;
  std::vector<NormalizedTextBox> normalized_boxes;
  std::vector<StyledWord> styled_words;  // text-layer order
  std::vector<TextLine> lines;
  std::vector<TextLine> footnote_lines;  // the page's footnotes, in reading order
  // Every region's words as reading-order lines (flow_box_lines): the
  // front-matter evidence metadata reads (rails, history, licence lines).
  std::vector<std::string> evidence_lines;
  std::vector<Block> blocks;
};

struct DocumentDom {
  std::string source_path;
  DocumentMeta meta;
  // Raw PDF Info-dictionary strings. They are evidence, not truth: metadata
  // extraction cross-checks them against the text layer before use.
  std::string info_title;
  std::string info_author;
  std::string info_subject;   // Elsevier/IEEE put a structured citation here
  std::string info_keywords;
  // Front-matter evidence: every text box (all regions, font sizes included)
  // of the first pages, used only for metadata. Body classification keeps
  // using PageDom::normalized_boxes.
  std::vector<std::vector<NormalizedTextBox>> front_boxes;
  // Body text type: the character-weighted most common word size, and
  // whether its face is itself a heavy weight (bold is then no cue). 0 when
  // the document has no usable typographic evidence (scans).
  double body_font_size = 0;
  bool body_font_heavy = false;
  std::vector<PageDom> pages;
  std::vector<std::string> endnotes;
  // Pandoc label of each endnote ("3", "3-p7"); empty means its position.
  std::vector<std::string> endnote_labels;
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
