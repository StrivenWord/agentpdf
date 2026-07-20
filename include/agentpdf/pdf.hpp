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

bool rasterize_page_raw(const std::string& path, int page_index, int dpi,
                        std::vector<unsigned char>& raw_argb, int& width, int& height,
                        int& bytes_per_row, std::string& err);

void build_blocks_from_lines(DocumentDom& dom, const Heuristics& heuristics);
void rejoin_hyphenated_lines(std::vector<TextLine>& lines);
void isolate_footnotes(DocumentDom& dom, const Heuristics& heuristics);
void extract_and_validate_metadata(DocumentDom& dom, const MetadataSpec& spec);
std::string assemble_markdown(const DocumentDom& dom, const Heuristics& heuristics);
void write_run_stats(const DocumentDom& dom, const JobEntry& job, const std::string& report_dir);

// Layout / OCR helpers
bool leptonica_analyze_raw(const unsigned char* argb, int width, int height, int bytes_per_row,
                           double& skew_deg, std::vector<BBox>& content_regions, std::string& err);
bool tesseract_ocr_page(const unsigned char* argb, int width, int height, int bytes_per_row,
                        const Heuristics& heuristics, std::vector<TextLine>& lines_out,
                        std::string& err);

}  // namespace agentpdf
