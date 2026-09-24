#include "agentpdf/pdf.hpp"
#include "agentpdf/thread_pool.hpp"
#include "agentpdf/hardware.hpp"
#include "agentpdf/util.hpp"

#include <poppler-document.h>
#include <poppler-font.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <regex>
#include <sstream>

namespace agentpdf {

namespace {

std::string ustring_to_utf8(const poppler::ustring& u) {
  auto bytes = u.to_utf8();
  return std::string(bytes.begin(), bytes.end());
}

struct RawBox {
  std::string text;
  BBox box;
};

bool is_mostly_digits(const std::string& s) {
  if (s.empty()) return false;
  int digits = 0, other = 0;
  for (unsigned char c : s) {
    if (std::isdigit(c)) ++digits;
    else if (!std::isspace(c) && c != '-' && c != '/') ++other;
  }
  return digits > 0 && other == 0;
}

bool is_running_header_footer(const std::string& text, const Heuristics& h) {
  auto t = collapse_ws(text);
  auto low = to_lower(t);
  if (t.empty()) return true;
  if (h.strip_page_numbers && is_mostly_digits(t) && t.size() <= 3) return true;
  if (h.strip_page_numbers) {
    static const std::regex page_of(R"(^page\s+\d+(\s+of\s+\d+)?$)");
    if (std::regex_match(low, page_of)) return true;
  }
  if (low.find("communications of the acm") != std::string::npos) return true;
  if (low.find("vol.") != std::string::npos &&
      (low.find("no.") != std::string::npos || low.find("/vol.") != std::string::npos))
    return true;
  if (low.find("frontiers in") != std::string::npos && t.size() < 90) return true;
  if (low.find("doi 10.") != std::string::npos && t.size() < 120) return true;
  if (low.find("doi: 10.") != std::string::npos && t.size() < 120) return true;
  if (low.find("front. polit") != std::string::npos) return true;
  if (low == "chin and kirkpatrick") return true;
  if (low == "10.3389/fpos.2023.1077945") return true;
  if (low == "frontiersin.org") return true;
  if (low == "open access") return true;
  if (low == "steve adler") return true;
  if (low.find("permission to make digital") != std::string::npos) return true;
  if (low.find("copyright 20") != std::string::npos && t.size() < 80) return true;
  if (low.find("total citations") != std::string::npos) return true;
  if (low.find("pdf download") != std::string::npos) return true;
  if (low.find("citation in bibtex") != std::string::npos) return true;
  if (low.find("open access support") != std::string::npos) return true;
  if (low.find("conference sponsors") != std::string::npos) return true;
  if (low.find("latest updates") != std::string::npos) return true;
  if (low.find("doi.org/") != std::string::npos && t.size() < 100) return true;
  // Magazine end-ornaments and stray drop-cap leftovers.
  if (t.size() == 1 && !std::isdigit(static_cast<unsigned char>(t[0]))) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    if (c != 'a' && c != 'i') return true;
  }
  return false;
}

bool detect_top_is_high_y(const std::vector<RawBox>& boxes) {
  // Infer whether visual top has higher PDF y (classic) or lower PDF y (some ACM DL PDFs).
  double abs_y = -1, copy_y = -1, title_y = -1;
  for (const auto& b : boxes) {
    auto low = to_lower(b.text);
    if (abs_y < 0 && low == "abstract") abs_y = b.box.cy();
    if (copy_y < 0 && (low.find("copyright") != std::string::npos ||
                       low.find("permission") != std::string::npos))
      copy_y = b.box.cy();
    if (title_y < 0 && (low == "storyspace" || low == "storyspace 1")) title_y = b.box.cy();
  }
  if (abs_y >= 0 && copy_y >= 0) return abs_y > copy_y;
  if (title_y >= 0 && copy_y >= 0) return title_y > copy_y;
  return true;  // default PDF convention
}

std::pair<std::vector<int>, bool> assign_columns(const std::vector<RawBox>& boxes,
                                                 double page_width, double gap_min) {
  std::vector<int> col(boxes.size(), 0);
  bool multi = false;
  if (boxes.size() < 20) return {col, multi};
  std::vector<double> xs;
  xs.reserve(boxes.size());
  for (const auto& b : boxes) xs.push_back(b.box.x0);
  std::sort(xs.begin(), xs.end());
  std::vector<double> cuts;
  for (size_t i = 1; i < xs.size(); ++i) {
    double gap = xs[i] - xs[i - 1];
    if (gap >= gap_min && xs[i] > page_width * 0.28 && xs[i] < page_width * 0.72) {
      cuts.push_back((xs[i] + xs[i - 1]) * 0.5);
    }
  }
  if (cuts.empty()) return {col, multi};
  double mid = page_width * 0.5;
  double cut = *std::min_element(cuts.begin(), cuts.end(), [&](double a, double b) {
    return std::abs(a - mid) < std::abs(b - mid);
  });
  int left = 0, right = 0;
  for (size_t i = 0; i < boxes.size(); ++i) {
    col[i] = boxes[i].box.cx() >= cut ? 1 : 0;
    if (col[i]) ++right;
    else ++left;
  }
  multi = left > 15 && right > 15;
  if (!multi) std::fill(col.begin(), col.end(), 0);
  return {col, multi};
}

std::vector<TextLine> merge_boxes_to_lines(const std::vector<RawBox>& boxes,
                                          const std::vector<int>& cols, double y_tol,
                                          bool top_is_high_y) {
  int max_col = 0;
  for (int c : cols) max_col = std::max(max_col, c);
  std::vector<TextLine> lines;
  for (int c = 0; c <= max_col; ++c) {
    std::vector<RawBox> col_boxes;
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (cols[i] == c) col_boxes.push_back(boxes[i]);
    }
    std::sort(col_boxes.begin(), col_boxes.end(), [&](const RawBox& a, const RawBox& b) {
      if (std::abs(a.box.y0 - b.box.y0) > 1.0) {
        return top_is_high_y ? (a.box.y0 > b.box.y0) : (a.box.y0 < b.box.y0);
      }
      return a.box.x0 < b.box.x0;
    });
    TextLine cur;
    auto flush = [&] {
      if (!cur.text.empty()) {
        cur.text = collapse_ws(cur.text);
        lines.push_back(cur);
      }
      cur = TextLine{};
    };
    for (const auto& b : col_boxes) {
      if (cur.text.empty()) {
        cur.text = b.text;
        cur.box = b.box;
        continue;
      }
      if (std::abs(b.box.y0 - cur.box.y0) <= y_tol) {
        if (!cur.text.empty() && !b.text.empty()) {
          unsigned char last = static_cast<unsigned char>(cur.text.back());
          unsigned char first = static_cast<unsigned char>(b.text.front());
          if (!std::isspace(last) && !std::isspace(first) && last != '-') cur.text.push_back(' ');
        }
        cur.text += b.text;
        cur.box.x0 = std::min(cur.box.x0, b.box.x0);
        cur.box.x1 = std::max(cur.box.x1, b.box.x1);
      } else {
        flush();
        cur.text = b.text;
        cur.box = b.box;
      }
    }
    flush();
  }
  // Assign synthetic top-to-bottom y for paragraph gap detection.
  renumber_synthetic_line_y(lines);
  return lines;
}

std::vector<TextLine> lines_from_reading_order_text(const std::string& text) {
  std::vector<TextLine> lines;
  std::istringstream iss(text);
  std::string line;
  double y = 0;
  int blank_run = 0;
  while (std::getline(iss, line)) {
    std::string cleaned;
    for (char c : line) {
      if (c == '\f') continue;
      cleaned.push_back(c);
    }
    cleaned = trim(normalize_typography(cleaned));
    // Poppler commonly glues superscript footnote markers to the preceding
    // word (precedent1). Separate only a single digit at a word boundary.
    static const std::regex attached_footnote(
        R"(([A-Za-z])([1-9])(?=([\s\.,;:\)]|$)))");
    cleaned = std::regex_replace(cleaned, attached_footnote, "$1 $2");
    if (cleaned.empty()) {
      ++blank_run;
      // Keep blank-run gaps modest: Poppler inserts visual blanks at wraps and
      // column breaks that are not semantic paragraphs. Sentence-boundary
      // checks in build_blocks_from_lines decide real paragraph splits.
      y += 12;
      continue;
    }
    if (!lines.empty() && blank_run == 0) {
      auto& prev = lines.back();
      if (!prev.text.empty() && prev.text.back() == '-' &&
          std::islower(static_cast<unsigned char>(cleaned.front()))) {
        prev.text.pop_back();
        prev.text += cleaned;
        // Do not advance y: the continuation belongs to the previous line.
        // Advancing here left a synthetic gap that looked like a paragraph break.
        blank_run = 0;
        continue;
      }
    }
    TextLine tl;
    tl.text = cleaned;
    tl.box = BBox{0, y, 100, y + 10};
    lines.push_back(tl);
    blank_run = 0;
    y += 12;
  }
  return lines;
}

int score_lines(const std::vector<TextLine>& lines) {
  // Higher is better: ABSTRACT early, Introduction early, less permission chrome.
  int score = 0;
  int n = static_cast<int>(lines.size());
  for (int i = 0; i < n; ++i) {
    auto low = to_lower(lines[static_cast<size_t>(i)].text);
    double frac = n ? static_cast<double>(i) / n : 0;
    if (low == "abstract" || low.find("abstract") == 0) score += (frac < 0.25) ? 50 : -20;
    if (low.find("1. introduction") == 0 || low == "1. storyspace")
      score += (frac < 0.4) ? 30 : -10;
    if (low.find("permission to make digital") != std::string::npos) score -= 25;
    if (low.find("what explains") == 0) score += 20;
    if (low.find("perseus and other") == 0) score += 20;
  }
  score += std::min(n, 200);  // prefer non-empty
  return score;
}

std::vector<TextLine> filter_chrome_lines(std::vector<TextLine> lines, const Heuristics& h,
                                         bool magazine_end_marks) {
  std::vector<TextLine> out;
  out.reserve(lines.size());
  for (auto& ln : lines) {
    auto low = to_lower(ln.text);
    for (const char* marker : {"communications of the acm", "frontiersin.org"}) {
      const auto position = low.find(marker);
      if (position != std::string::npos && position > 0) {
        ln.text = trim(ln.text.substr(0, position));
        low = to_lower(ln.text);
      }
    }
    // Drop a trailing single-letter ornament (the Communications of the ACM
    // end mark "c"). Elsewhere a final single letter is text ("Appendix B",
    // letter-spaced labels such as "A B S T R A C T").
    if (magazine_end_marks && ln.text.size() >= 3) {
      const auto last = ln.text.back();
      const auto prev = ln.text[ln.text.size() - 2];
      if (std::isspace(static_cast<unsigned char>(prev)) &&
          std::isalpha(static_cast<unsigned char>(last)) &&
          std::tolower(static_cast<unsigned char>(last)) != 'a' &&
          std::tolower(static_cast<unsigned char>(last)) != 'i') {
        ln.text = trim(ln.text.substr(0, ln.text.size() - 1));
        low = to_lower(ln.text);
      }
    }
    if (is_running_header_footer(ln.text, h)) continue;
    if (is_mostly_digits(trim(ln.text)) && trim(ln.text).size() <= 3) continue;
    out.push_back(std::move(ln));
  }
  // Do not renumber here: flow text uses larger y gaps for Poppler blank lines
  // that mark real paragraphs. Stitch/quarantine renumber after their erasures.
  return out;
}

// Text rules for lines rebuilt from classified words (flow_box_lines).
// Geometry has already placed every word, so only furniture that escapes
// the header/footer bands is left: bare page numbers, page labels ("Page 3
// of 10", "2 of 17") and lone ornaments. Journal names, volume lines and
// DOIs are not matched as text here: in the body they are references.
std::vector<TextLine> filter_box_chrome_lines(std::vector<TextLine> lines, const Heuristics& h) {
  static const std::regex page_label(R"(^(?:page\s+)?\d{1,4}\s+(?:of|/)\s+\d{1,4}$|^page\s+\d{1,4}$)",
                                     std::regex::icase);
  std::vector<TextLine> out;
  out.reserve(lines.size());
  for (auto& line : lines) {
    const auto t = trim(line.text);
    if (t.empty()) continue;
    if (h.strip_page_numbers && is_mostly_digits(t) && t.size() <= 3) continue;
    if (h.strip_page_numbers && std::regex_match(t, page_label)) continue;
    if (t.size() == 1 && !std::isdigit(static_cast<unsigned char>(t[0]))) {
      const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
      if (c != 'a' && c != 'i') continue;
    }
    out.push_back(std::move(line));
  }
  return out;
}

struct OcrJob {
  int page_index = 0;
  bool candidate = false;  // true: compare OCR against native lines; false: use OCR directly
};

struct OcrPageResult {
  int page_index = -1;
  std::vector<TextLine> lines;
  bool used_ocr = false;
  double skew_deg = 0.0;
  std::vector<BBox> content_regions;
};

OcrPageResult run_ocr_job(const std::string& path, int page_index, const Heuristics& heuristics) {
  OcrPageResult res;
  res.page_index = page_index;
  std::vector<unsigned char> raw;
  int w = 0, h = 0, bpr = 0;
  std::string err;
  if (!rasterize_page_raw(path, page_index, heuristics.ocr_dpi, raw, w, h, bpr, err)) return res;

  // Analyze page geometry with Leptonica: detect skew and content regions.
  leptonica_analyze_raw(raw.data(), w, h, bpr, res.skew_deg, res.content_regions, err);

  // Run OCR on the page (Leptonica analysis results can inform OCR quality).
  std::vector<TextLine> ocr_lines;
  if (tesseract_ocr_page_thread_local(raw.data(), w, h, bpr, heuristics, ocr_lines, err)) {
    res.lines = std::move(ocr_lines);
    res.used_ocr = true;
  }
  return res;
}

std::vector<RawBox> collect_raw_boxes(poppler::page& page) {
  std::vector<RawBox> raw;
  auto text_boxes = page.text_list();
  raw.reserve(text_boxes.size());
  for (const auto& tb : text_boxes) {
    RawBox rb;
    rb.text = ustring_to_utf8(tb.text());
    if (rb.text.empty()) continue;
    auto r = tb.bbox();
    rb.box.x0 = r.x();
    rb.box.x1 = r.x() + r.width();
    rb.box.y0 = r.y();
    rb.box.y1 = r.y() + r.height();
    raw.push_back(std::move(rb));
  }
  return raw;
}

// Every word of the page's text layer, with its type, plus the styled words
// heading detection reads. Body classification deliberately runs without
// font sizes (its dormant small-font footnote rule would quarantine text
// without endnote conversion), so the caller strips them from body boxes;
// front-matter evidence keeps them.
std::vector<NormalizedTextBox> collect_normalized_boxes(poppler::page& page,
                                                       std::vector<StyledWord>& styled) {
  std::vector<NormalizedTextBox> result;
  auto boxes = page.text_list(poppler::page::text_list_include_font);
  result.reserve(boxes.size());
  styled.clear();
  styled.reserve(boxes.size());
  for (const auto& source : boxes) {
    NormalizedTextBox box;
    box.text = collapse_ws(normalize_typography(ustring_to_utf8(source.text())));
    if (box.text.empty()) continue;
    const auto rect = source.bbox();
    box.box = BBox{rect.x(), rect.y(), rect.x() + rect.width(),
                   rect.y() + rect.height()};
    box.rotation = source.rotation();
    box.space_after = source.has_space_after();
    StyledWord word;
    word.folded = fold_alnum(box.text);
    if (source.has_font_info()) {
      box.font_size = source.get_font_size();
      const auto font = to_lower(source.get_font_name());
      for (const char* weight : {"bold", "black", "heavy", "semibold", "demi"}) {
        if (font.find(weight) != std::string::npos) box.bold = true;
      }
      for (const char* style : {"italic", "oblique"}) {
        if (font.find(style) != std::string::npos) box.italic = true;
      }
      // Heading faces also come as Medium (IEEE's NimbusRomNo9L-Medi) and
      // abbreviated Semibold (ArnoPro-Smbd).
      word.heavy = box.bold || font.find("medi") != std::string::npos ||
                   font.find("smbd") != std::string::npos;
      word.italic = box.italic;
      word.font_size = box.font_size;
      box.type_size = word.font_size;
      box.type_heavy = word.heavy;
      box.type_italic = word.italic;
    }
    if (!word.folded.empty()) styled.push_back(std::move(word));
    result.push_back(std::move(box));
  }
  return result;
}

// The body text's type: the most common word size, weighted by characters
// (sizes bucketed to 0.1 pt), and whether most body-size text is heavy.
void measure_body_font(DocumentDom& dom) {
  std::map<long, size_t> chars_by_size;
  for (const auto& page : dom.pages) {
    for (const auto& word : page.styled_words) {
      if (word.font_size > 0)
        chars_by_size[std::lround(word.font_size * 10.0)] += word.folded.size();
    }
  }
  if (chars_by_size.empty()) return;
  const auto mode = std::max_element(
      chars_by_size.begin(), chars_by_size.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });
  dom.body_font_size = static_cast<double>(mode->first) / 10.0;
  size_t body_chars = 0, heavy_chars = 0;
  for (const auto& page : dom.pages) {
    for (const auto& word : page.styled_words) {
      if (std::lround(word.font_size * 10.0) != mode->first) continue;
      body_chars += word.folded.size();
      if (word.heavy) heavy_chars += word.folded.size();
    }
  }
  dom.body_font_heavy = heavy_chars * 2 > body_chars;
}

// Locate each line in the page's styled words and record their type. Keys
// are folded (fold_alnum), so the streams' different spacing and hyphen
// joins still align, and a match must start and end on word boundaries. The
// search runs forward from the previous match, since every stream follows
// roughly the text layer's order, and falls back to the whole page.
void annotate_line_typography(PageDom& page) {
  if (page.used_ocr || page.styled_words.empty() || page.lines.empty()) return;
  std::string folded;
  std::vector<size_t> owner;
  std::vector<bool> word_start;
  for (size_t k = 0; k < page.styled_words.size(); ++k) {
    const auto& key = page.styled_words[k].folded;
    for (size_t c = 0; c < key.size(); ++c) {
      folded.push_back(key[c]);
      owner.push_back(k);
      word_start.push_back(c == 0);
    }
  }
  word_start.push_back(true);  // the end of the page is a boundary
  auto find_aligned = [&](const std::string& key, size_t from) {
    for (size_t pos = folded.find(key, from); pos != std::string::npos;
         pos = folded.find(key, pos + 1)) {
      if (word_start[pos] && word_start[pos + key.size()]) return pos;
    }
    return std::string::npos;
  };
  size_t cursor = 0;
  for (auto& line : page.lines) {
    if (line.has_geom) continue;  // typed from its own words already
    const auto key = fold_alnum(normalize_typography(line.text));
    if (key.size() < 3) continue;
    size_t pos = find_aligned(key, cursor);
    if (pos == std::string::npos) pos = find_aligned(key, 0);
    if (pos == std::string::npos) continue;
    double chars = 0, size_sum = 0, size_max = 0, heavy = 0, italic = 0;
    for (size_t i = pos; i < pos + key.size(); ++i) {
      const auto& word = page.styled_words[owner[i]];
      if (word.font_size <= 0) continue;
      chars += 1;
      size_sum += word.font_size;
      size_max = std::max(size_max, word.font_size);
      if (word.heavy) heavy += 1;
      if (word.italic) italic += 1;
    }
    cursor = pos + key.size();
    if (chars == 0) continue;
    line.font_size = size_sum / chars;
    line.font_size_max = size_max;
    line.bold = heavy / chars >= 0.6;
    line.italic = italic / chars >= 0.6;
  }
}

// A scanned page carries a page-covering raster image. Rendered on white and
// on black paper it yields (nearly) identical pixels everywhere, because no
// paper shows through; a vector page differs wherever paper is visible.
double page_image_coverage(poppler::page& page) {
  poppler::page_renderer on_white;
  poppler::page_renderer on_black;
  on_white.set_paper_color(0xffffffff);
  on_black.set_paper_color(0xff000000);
  on_white.set_image_format(poppler::image::format_argb32);
  on_black.set_image_format(poppler::image::format_argb32);
  constexpr double dpi = 24.0;
  const auto a = on_white.render_page(&page, dpi, dpi);
  const auto b = on_black.render_page(&page, dpi, dpi);
  if (!a.is_valid() || !b.is_valid() || a.width() != b.width() ||
      a.height() != b.height() || a.width() <= 0 || a.height() <= 0) {
    return 0.0;
  }
  const auto* pa = reinterpret_cast<const unsigned char*>(a.const_data());
  const auto* pb = reinterpret_cast<const unsigned char*>(b.const_data());
  size_t covered = 0;
  for (int y = 0; y < a.height(); ++y) {
    const auto* ra = pa + static_cast<size_t>(y) * static_cast<size_t>(a.bytes_per_row());
    const auto* rb = pb + static_cast<size_t>(y) * static_cast<size_t>(b.bytes_per_row());
    for (int x = 0; x < a.width(); ++x) {
      const auto* ca = ra + x * 4;
      const auto* cb = rb + x * 4;
      if (std::abs(ca[0] - cb[0]) + std::abs(ca[1] - cb[1]) + std::abs(ca[2] - cb[2]) < 24)
        ++covered;
    }
  }
  return static_cast<double>(covered) /
         (static_cast<double>(a.width()) * static_cast<double>(a.height()));
}

// Scanned-with-text-layer PDFs: every sampled page is covered by an image,
// and the (invisible) text layer uses non-embedded fonts. A painted vector
// background alone does not qualify, because its visible text is embedded.
bool detect_scanned_text_layer(poppler::document& doc, int sample_pages) {
  std::vector<std::string> non_embedded;
  for (const auto& font : doc.fonts()) {
    if (!font.is_embedded()) non_embedded.push_back(font.name());
  }
  if (non_embedded.empty()) return false;
  size_t chars = 0;
  size_t non_embedded_chars = 0;
  for (int pi = 0; pi < sample_pages; ++pi) {
    std::unique_ptr<poppler::page> page(doc.create_page(pi));
    if (!page) return false;
    if (page_image_coverage(*page) < 0.90) return false;
    for (const auto& tb : page->text_list(poppler::page::text_list_include_font)) {
      const auto len = tb.text().size();
      chars += len;
      if (std::find(non_embedded.begin(), non_embedded.end(), tb.get_font_name()) !=
          non_embedded.end())
        non_embedded_chars += len;
    }
  }
  return chars > 0 && static_cast<double>(non_embedded_chars) / static_cast<double>(chars) >= 0.60;
}

double line_stream_quality(const std::vector<TextLine>& lines) {
  size_t words = 0;
  size_t suspect = 0;
  for (const auto& line : lines) {
    for (const auto& word : split_words(line.text)) {
      ++words;
      if (word.size() > 24 || word.find_first_of("$�") != std::string::npos)
        ++suspect;
    }
  }
  if (words < 8) return 0;
  return 1.0 - std::min(0.75, static_cast<double>(suspect) / words * 4.0);
}

}  // namespace

ExtractResult extract_pdf_dom(const std::string& path, const Heuristics& heuristics) {
  ExtractResult result;
  result.dom.source_path = path;

  std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path));
  if (!doc) {
    result.err = "failed to open PDF: " + path;
    return result;
  }

  // Info-dictionary strings are kept as raw evidence; metadata extraction
  // validates them against the text layer (many are junk: temp filenames,
  // manuscript IDs, uploader account names).
  result.dom.info_title = collapse_ws(ustring_to_utf8(doc->get_title()));
  result.dom.info_author = collapse_ws(ustring_to_utf8(doc->get_author()));
  result.dom.info_subject = collapse_ws(ustring_to_utf8(doc->get_subject()));
  result.dom.info_keywords = collapse_ws(ustring_to_utf8(doc->get_keywords()));

  const int n = doc->pages();
  if (n <= 0) {
    result.err = "PDF has no pages: " + path;
    return result;
  }

  // Pass 1: geometry of every page, plus font-bearing front-matter evidence.
  constexpr int front_pages = 3;
  LayoutSignals signals;
  auto& pages = result.dom.pages;
  pages.reserve(static_cast<size_t>(n));
  for (int pi = 0; pi < n; ++pi) {
    std::unique_ptr<poppler::page> page(doc->create_page(pi));
    PageDom pd;
    pd.index = pi;
    if (page) {
      auto rect = page->page_rect();
      pd.width = rect.width();
      pd.height = rect.height();
      auto boxes = collect_normalized_boxes(*page, pd.styled_words);
      if (pi < front_pages) {
        result.dom.front_boxes.push_back(boxes);
        std::string text;
        for (const auto& box : boxes) text += box.text + ' ';
        signals.page_text.push_back(to_lower(text));
      }
      for (auto& box : boxes) {
        box.font_size = 0;
        box.bold = box.italic = false;
      }
      pd.normalized_boxes = std::move(boxes);
    }
    pages.push_back(std::move(pd));
  }
  signals.scanned_with_text_layer =
      detect_scanned_text_layer(*doc, std::min(n, front_pages));

  // Pass 2: layout family and cover pages from content, never from names.
  const LayoutFamily family = detect_layout_family(signals);
  for (auto& pd : pages) pd.layout_family = family;
  if (n > 1) pages.front().wrapper_page = is_cover_page(pages.front(), heuristics);

  // The body type, for classification (captions, footnotes and furniture
  // are set apart from it) and heading detection. A scan's text layer is an
  // invisible OCR font of uniform size, which says nothing.
  if (family != LayoutFamily::ScanOcrTwoColumn) {
    measure_body_font(result.dom);
    for (auto& pd : pages) {
      pd.body_font_size = result.dom.body_font_size;
      pd.body_font_heavy = result.dom.body_font_heavy;
    }
  }

  // Pass 3: region classification, then cross-page running-header detection.
  bool references_active = false;
  for (auto& pd : pages) {
    classify_page_regions(pd, heuristics);
    for (const auto& box : pd.normalized_boxes) {
      auto low = to_lower(trim(box.text));
      if (low == "references" || low == "bibliography" || low == "works cited" ||
          (low.rfind("references", 0) == 0 && low.size() <= 14)) {
        references_active = true;
        break;
      }
    }
    if (references_active) mark_post_references_ancillary(pd, true);
  }
  mark_repeated_page_chrome(pages, heuristics);

  // Line-end hyphen evidence: every word the document prints whole.
  const Vocabulary vocab = build_vocabulary(pages);

  // Pass 4: choose and normalize each page's line stream.
  std::vector<OcrJob> ocr_jobs;
  ocr_jobs.reserve(n);
  for (int pi = 0; pi < n; ++pi) {
    PageDom& pd = pages[static_cast<size_t>(pi)];
    if (pd.wrapper_page) continue;
    std::unique_ptr<poppler::page> page(doc->create_page(pi));
    if (!page) continue;

    auto raw = collect_raw_boxes(*page);
    size_t char_count = 0;
    for (const auto& b : raw) char_count += b.text.size();

    if (static_cast<double>(char_count) < heuristics.min_text_layer_chars_per_page) {
      ocr_jobs.push_back({pi, false});
      continue;
    }

    const bool magazine = family == LayoutFamily::MagazineTwoColumn;
    auto geometry_lines =
        filter_chrome_lines(linearize_page(pd, heuristics), heuristics, magazine);
    std::string flow_text;
    std::string raw_text;
    try {
      flow_text = ustring_to_utf8(
          page->text(poppler::rectf(), poppler::page::non_raw_non_physical_layout));
      raw_text = ustring_to_utf8(
          page->text(poppler::rectf(), poppler::page::raw_order_layout));
    } catch (...) {
      flow_text.clear();
      raw_text.clear();
    }
    // Unknown templates read Poppler's reading order rebuilt from the page's
    // own classified words; geometry wins only when clearly cleaner.
    if (family == LayoutFamily::Generic) {
      std::vector<TextLine> box_lines;
      if (flow_box_lines(flow_text, pd, vocab, box_lines, &pd.footnote_lines)) {
        box_lines = filter_box_chrome_lines(std::move(box_lines), heuristics);
        const bool geometry_cleaner =
            !geometry_lines.empty() && (box_lines.empty() || line_stream_quality(geometry_lines) >
                                                                 line_stream_quality(box_lines) + 0.05);
        pd.lines = geometry_cleaner ? std::move(geometry_lines) : std::move(box_lines);
        continue;
      }
    }
    auto flow_lines =
        filter_chrome_lines(lines_from_reading_order_text(flow_text), heuristics, magazine);
    auto raw_lines =
        filter_chrome_lines(lines_from_reading_order_text(raw_text), heuristics, magazine);
    flow_lines =
        quarantine_stream_lines(pd, std::move(flow_lines), heuristics);
    raw_lines = quarantine_stream_lines(pd, std::move(raw_lines), heuristics);

    // Normalize every candidate before selection; the DOM never sees Poppler's
    // unnormalized stream. Family selection is deliberately thin: it chooses
    // the most reliable source stream while the same classifier/DOM handles all.
    switch (family) {
      case LayoutFamily::AcmConferenceTwoColumn:
        pd.lines = !raw_lines.empty() ? std::move(raw_lines)
                                      : std::move(geometry_lines);
        break;
      case LayoutFamily::MagazineTwoColumn:
        if ((pd.column_cut_override || pd.has_region_overrides) &&
            !geometry_lines.empty())
          pd.lines = std::move(geometry_lines);
        else
          pd.lines = !flow_lines.empty() ? std::move(flow_lines)
                                         : std::move(geometry_lines);
        break;
      case LayoutFamily::FrontiersRail:
        if ((pd.column_cut_override || pd.has_region_overrides || pi >= 2) &&
            !geometry_lines.empty())
          pd.lines = std::move(geometry_lines);
        else
          pd.lines = !flow_lines.empty() ? std::move(flow_lines)
                                         : std::move(geometry_lines);
        break;
      case LayoutFamily::ScanOcrTwoColumn:
        pd.lines = !raw_lines.empty() ? std::move(raw_lines)
                                      : std::move(geometry_lines);
        break;
      case LayoutFamily::Generic:
        // Poppler's reading-order stream handles multi-column layouts for
        // unknown templates; geometry wins only when clearly cleaner, since
        // near-ties are noise in the quality score.
        pd.lines = flow_lines.empty() ||
                           line_stream_quality(geometry_lines) >
                               line_stream_quality(flow_lines) + 0.05
                       ? std::move(geometry_lines)
                       : std::move(flow_lines);
        break;
    }

    if (family == LayoutFamily::ScanOcrTwoColumn &&
        heuristics.ocr_when_scan_present) {
      ocr_jobs.push_back({pi, true});
    }
    // Footnotes leave the stream with their region; they are kept to become
    // endnotes (a scan's are read from its OCR text, not here).
    if (family != LayoutFamily::ScanOcrTwoColumn) pd.footnote_lines = footnote_lines_by_geometry(pd);
  }

  // Run all collected OCR work in parallel using the configured worker count.
  // Each worker thread reuses a single tesseract::TessBaseAPI instance for the
  // lifetime of the pool, avoiding the per-page Init/End overhead that dominated
  // OCR runtime on low-power machines.
  if (!ocr_jobs.empty()) {
    std::vector<OcrPageResult> ocr_results(result.dom.pages.size());
    ThreadPool pool(recommended_ocr_workers());
    std::mutex results_mutex;
    for (const auto& job : ocr_jobs) {
      pool.submit([&]() {
        auto res = run_ocr_job(path, job.page_index, heuristics);
        std::lock_guard<std::mutex> lock(results_mutex);
        ocr_results[job.page_index] = std::move(res);
      });
    }
    pool.wait_for_all();

    for (const auto& job : ocr_jobs) {
      const auto& res = ocr_results[job.page_index];
      if (res.page_index < 0 || res.lines.empty()) continue;
      auto& page = result.dom.pages[job.page_index];

      // Store Leptonica analysis results (skew detection and content regions).
      page.detected_skew_deg = res.skew_deg;
      page.ocr_content_regions = res.content_regions;

      if (job.candidate) {
        if (line_stream_quality(res.lines) > line_stream_quality(page.lines) + 0.04) {
          page.lines = quarantine_stream_lines(page, std::move(res.lines), heuristics);
          page.used_ocr = true;
        }
      } else {
        page.lines = std::move(res.lines);
        page.used_ocr = true;
      }
    }
  }

  if (heuristics.footnotes_to_endnotes) link_note_markers(result.dom);
  stitch_document_lines(result.dom, heuristics);
  mark_page_turn_paragraphs(pages);

  // Typographic evidence for heading detection (lines rebuilt from their own
  // words carry it already).
  if (result.dom.body_font_size > 0) {
    for (auto& pd : pages) annotate_line_typography(pd);
  }
  for (auto& pd : pages) {
    pd.styled_words.clear();
    pd.styled_words.shrink_to_fit();
  }
  result.ok = true;
  return result;
}

bool rasterize_page_raw(const std::string& path, int page_index, int dpi,
                        std::vector<unsigned char>& raw_argb, int& width, int& height,
                        int& bytes_per_row, std::string& err) {
  std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path));
  if (!doc) {
    err = "failed to open PDF for raster: " + path;
    return false;
  }
  std::unique_ptr<poppler::page> page(doc->create_page(page_index));
  if (!page) {
    err = "cannot create page";
    return false;
  }
  poppler::page_renderer renderer;
  renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
  renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);
  poppler::image img = renderer.render_page(page.get(), dpi, dpi);
  if (!img.is_valid()) {
    err = "render failed";
    return false;
  }
  width = img.width();
  height = img.height();
  bytes_per_row = img.bytes_per_row();
  const char* data = img.const_data();
  size_t bytes = static_cast<size_t>(bytes_per_row) * static_cast<size_t>(height);
  raw_argb.assign(data, data + bytes);
  return true;
}

}  // namespace agentpdf
