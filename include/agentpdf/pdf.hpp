#pragma once

#include "agentpdf/types.hpp"

#include <string>
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
