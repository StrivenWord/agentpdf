#pragma once

#include "agentpdf/types.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace poppler {
class document;
}

namespace agentpdf {

struct ExtractResult {
  DocumentDom dom;
  bool ok = false;
  std::string err;
};

ExtractResult extract_pdf_dom(const std::string& path, const Heuristics& heuristics);

// Content signals for layout-family detection (first pages only).
struct LayoutSignals {
  std::vector<std::string> page_text;  // lowercase text of the first pages
  bool scanned_with_text_layer = false;
};

LayoutFamily detect_layout_family(const LayoutSignals& signals);
// Repository cover sheets and pictorial title pages carry no article body.
bool is_cover_page(const PageDom& page, const Heuristics& heuristics);
// Mark header/footer-band lines that recur across pages as chrome.
void mark_repeated_page_chrome(std::vector<PageDom>& pages, const Heuristics& heuristics);
double score_text_quality(const std::vector<NormalizedTextBox>& boxes);
void classify_page_regions(PageDom& page, const Heuristics& heuristics);
// After a References section, quarantine full-width / form-like back matter
// that appears once two-column bibliography structure collapses (Approach C).
void mark_post_references_ancillary(PageDom& page, bool references_active);
std::vector<TextLine> linearize_page(const PageDom& page, const Heuristics& heuristics);
std::vector<TextLine> quarantine_stream_lines(
    const PageDom& page, std::vector<TextLine> lines,
    const Heuristics& heuristics);
void stitch_document_lines(DocumentDom& dom, const Heuristics& heuristics);
void renumber_synthetic_line_y(std::vector<TextLine>& lines);

// Words the document prints whole (lowercase; compounds keep their inner
// hyphen), the evidence for line-end hyphen decisions.
struct Vocabulary {
  std::unordered_set<std::string> words;
};
Vocabulary build_vocabulary(const std::vector<PageDom>& pages);
// "first-" ending a line and "second" opening the next: a hyphenated
// compound (keep the hyphen) or one word broken by the typesetter?
bool keep_line_end_hyphen(const std::string& first, const std::string& second,
                          const Vocabulary& vocab);
// Poppler's reading-order text rebuilt from the page's own text-layer
// words: each word keeps the region its box was classified into, so
// non-body text leaves the stream word by word, and every line carries its
// words' real geometry and type. False when the flow text does not align
// with the text layer (the caller then uses string quarantine).
// With `notes`, the page's footnote-region words are rebuilt the same way
// (each note's printed key in TextLine::note_key).
bool flow_box_lines(const std::string& flow_text, const PageDom& page, const Vocabulary& vocab,
                    std::vector<TextLine>& out, std::vector<TextLine>* notes = nullptr);
// Footnote lines of a page by geometry alone (streams without flow
// alignment): column by column, top to bottom.
std::vector<TextLine> footnote_lines_by_geometry(const PageDom& page);
// Group every page's footnote lines into notes (DocumentDom::endnotes),
// and turn the body's superscript markers that name one into Pandoc note
// references ("[^3]"); other markers become plain numbers again.
void link_note_markers(DocumentDom& dom);
// Column measure of each line (its left and right edges, and whether the
// column is set justified), from the lines sharing its horizontal span.
struct LineColumns {
  std::vector<double> left, right;
  std::vector<bool> justified;
};
LineColumns measure_line_columns(const std::vector<TextLine>& lines);
// Paragraph starts from page geometry (TextLine::para_start).
void mark_paragraph_starts(std::vector<TextLine>& lines);
// A page's first line opens a paragraph when the previous page's last line
// stopped short after a finished sentence.
void mark_page_turn_paragraphs(std::vector<PageDom>& pages);

bool rasterize_page_raw(const std::string& path, int page_index, int dpi,
                        std::vector<unsigned char>& raw_argb, int& width, int& height,
                        int& bytes_per_row, std::string& err);

void build_blocks_from_lines(DocumentDom& dom, const Heuristics& heuristics);
void rejoin_hyphenated_lines(std::vector<TextLine>& lines);
void isolate_footnotes(DocumentDom& dom, const Heuristics& heuristics);
// Title, authors and DOI from front-matter evidence; runs before block
// building because body assembly needs the title.
void extract_front_matter_metadata(DocumentDom& dom);
void extract_and_validate_metadata(DocumentDom& dom, const MetadataSpec& spec);
std::string assemble_markdown(const DocumentDom& dom, const Heuristics& heuristics,
                              const MetadataSpec& spec);

// Canonical frontmatter fields (src/meta/fields.cpp).
MetadataSpec default_metadata_spec();
bool parse_field_spec(const std::string& line, FieldSpec& out);
// YAML frontmatter block for Obsidian: emitted fields only, in template
// order, empty values omitted, text double-quoted, lists as block lists.
std::string render_frontmatter(const DocumentMeta& meta, const MetadataSpec& spec);
void write_run_stats(const DocumentDom& dom, const JobEntry& job, const std::string& report_dir);

// Layout / OCR helpers
bool leptonica_analyze_raw(const unsigned char* argb, int width, int height, int bytes_per_row,
                           double& skew_deg, std::vector<BBox>& content_regions, std::string& err);
bool tesseract_ocr_page(const unsigned char* argb, int width, int height, int bytes_per_row,
                        const Heuristics& heuristics, std::vector<TextLine>& lines_out,
                        std::string& err);

}  // namespace agentpdf

namespace tesseract {
class TessBaseAPI;
}

namespace agentpdf {

bool tesseract_ocr_page_with_api(tesseract::TessBaseAPI& api, const unsigned char* argb,
                                 int width, int height, int bytes_per_row,
                                 const Heuristics& heuristics,
                                 std::vector<TextLine>& lines_out, std::string& err);
bool tesseract_ocr_page_thread_local(const unsigned char* argb, int width, int height,
                                     int bytes_per_row, const Heuristics& heuristics,
                                     std::vector<TextLine>& lines_out, std::string& err);

}  // namespace agentpdf
