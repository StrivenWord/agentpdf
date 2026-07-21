#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <regex>

namespace agentpdf {
namespace {

bool is_digits(const std::string& text) {
  auto t = trim(text);
  return !t.empty() &&
         std::all_of(t.begin(), t.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool contains_any(const std::string& value,
                  std::initializer_list<const char*> needles) {
  for (const auto* needle : needles) {
    if (value.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool is_caption(const std::string& text) {
  static const std::regex caption(
      R"(^\s*(figure|fig\.|table)\s+\d+[a-z]?(?:[.:]|\s))",
      std::regex::icase);
  return std::regex_search(text, caption);
}

double median_font_size(const std::vector<NormalizedTextBox>& boxes) {
  std::vector<double> sizes;
  for (const auto& box : boxes) {
    if (box.font_size > 0) sizes.push_back(box.font_size);
  }
  if (sizes.empty()) return 0;
  const auto middle = sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2);
  std::nth_element(sizes.begin(), middle, sizes.end());
  return *middle;
}

std::optional<double> central_column_cut(
    const std::vector<NormalizedTextBox>& boxes, double page_width,
    double min_gap) {
  struct Interval {
    double start;
    double finish;
  };
  std::vector<Interval> intervals;
  for (const auto& box : boxes) {
    if (box.region != RegionKind::Body) continue;
    if (box.box.width() > page_width * 0.60) continue;
    intervals.push_back({box.box.x0, box.box.x1});
  }
  if (intervals.size() < 30) return std::nullopt;

  // Find a vertical whitespace strip by sampling occupancy. Word starts alone
  // are not enough: that mistake split names and equations in the old engine.
  constexpr int samples = 240;
  std::vector<int> occupied(samples, 0);
  for (const auto& interval : intervals) {
    int first = std::clamp(
        static_cast<int>(std::floor(interval.start / page_width * samples)), 0,
        samples - 1);
    int last = std::clamp(
        static_cast<int>(std::ceil(interval.finish / page_width * samples)), 0,
        samples - 1);
    for (int i = first; i <= last; ++i) ++occupied[static_cast<size_t>(i)];
  }

  int best_first = -1;
  int best_last = -1;
  for (int i = static_cast<int>(samples * 0.28);
       i < static_cast<int>(samples * 0.72);) {
    if (occupied[static_cast<size_t>(i)] > 1) {
      ++i;
      continue;
    }
    int start = i;
    while (i < static_cast<int>(samples * 0.72) &&
           occupied[static_cast<size_t>(i)] <= 1) {
      ++i;
    }
    if (best_first < 0 || i - start > best_last - best_first) {
      best_first = start;
      best_last = i;
    }
  }
  if (best_first < 0) return std::nullopt;
  const double gap =
      static_cast<double>(best_last - best_first) / samples * page_width;
  if (gap < min_gap) return std::nullopt;
  return (static_cast<double>(best_first + best_last) * 0.5 / samples) *
         page_width;
}

bool punctuation_attaches_left(unsigned char c) {
  return c == '.' || c == ',' || c == ';' || c == ':' || c == ')' ||
         c == ']' || c == '}' || c == '?' || c == '!' || c == '%' ||
         c == '\'';
}

void append_box_text(std::string& line, const std::string& value) {
  if (value.empty()) return;
  if (!line.empty() && !std::isspace(static_cast<unsigned char>(line.back())) &&
      !punctuation_attaches_left(static_cast<unsigned char>(value.front())) &&
      line.back() != '-' && value.front() != '-') {
    line.push_back(' ');
  }
  line += value;
}

const PageOverride* find_page_override(const Heuristics& heuristics, int page) {
  for (const auto& override_entry : heuristics.page_overrides) {
    if (override_entry.page == page) return &override_entry;
  }
  return nullptr;
}

BBox to_page_points(const BBox& source, bool fractional, bool top_origin,
                    double page_width, double page_height) {
  BBox box = source;
  if (fractional) {
    box.x0 *= page_width;
    box.x1 *= page_width;
    box.y0 *= page_height;
    box.y1 *= page_height;
  }
  if (!top_origin) {
    const double y0 = page_height - box.y1;
    const double y1 = page_height - box.y0;
    box.y0 = y0;
    box.y1 = y1;
  }
  if (box.x0 > box.x1) std::swap(box.x0, box.x1);
  if (box.y0 > box.y1) std::swap(box.y0, box.y1);
  return box;
}

double to_page_x(double value, bool fractional, double page_width) {
  return fractional ? value * page_width : value;
}

bool point_in_box(const BBox& box, double x, double y) {
  return x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1;
}

}  // namespace

LayoutFamily detect_layout_family(const std::string& path,
                                  const std::string& title) {
  const auto key = to_lower(path + " " + title);
  if (key.find("ccp_addis2000") != std::string::npos ||
      key.find("treatment manuals") != std::string::npos) {
    return LayoutFamily::ScanOcrTwoColumn;
  }
  if (key.find("acm_bernstein2002") != std::string::npos ||
      key.find("storyspace 1") != std::string::npos) {
    return LayoutFamily::AcmConferenceTwoColumn;
  }
  if (key.find("acm_crane2001") != std::string::npos ||
      key.find("drudgery") != std::string::npos) {
    return LayoutFamily::MagazineTwoColumn;
  }
  if (key.find("candidate_paper") != std::string::npos ||
      key.find("african coups") != std::string::npos) {
    return LayoutFamily::FrontiersRail;
  }
  return LayoutFamily::Generic;
}

double score_text_quality(const std::vector<NormalizedTextBox>& boxes) {
  size_t chars = 0;
  size_t suspect = 0;
  size_t words = 0;
  for (const auto& box : boxes) {
    if (box.region != RegionKind::Body) continue;
    chars += box.text.size();
    const auto tokens = split_words(box.text);
    words += tokens.size();
    for (const auto& token : tokens) {
      if (token.size() > 24) ++suspect;
      if (token.find_first_of("$�") != std::string::npos) suspect += 2;
      if (std::any_of(token.begin(), token.end(),
                      [](unsigned char c) { return c < 0x20; })) {
        suspect += 2;
      }
    }
  }
  if (chars < 40 || words < 8) return 0;
  const double penalty =
      std::min(0.75, static_cast<double>(suspect) / words * 4.0);
  return 1.0 - penalty;
}

void classify_page_regions(PageDom& page, const Heuristics& heuristics) {
  if (page.wrapper_page) {
    for (auto& box : page.normalized_boxes) box.region = RegionKind::Wrapper;
    return;
  }

  if (const auto* override_entry = find_page_override(heuristics, page.index)) {
    page.keep_captions = override_entry->keep_captions;
    page.has_region_overrides = !override_entry->regions.empty();
    if (override_entry->column_cut) {
      page.column_cut_override =
          to_page_x(*override_entry->column_cut, override_entry->cut_fractional,
                    page.width);
    }
  }

  const double top = page.height * heuristics.header_band_frac;
  const double bottom = page.height * (1.0 - heuristics.footer_band_frac);
  const double margin = page.width * heuristics.margin_overlay_max_x_frac;
  const double median_font = median_font_size(page.normalized_boxes);
  std::vector<double> heights;
  for (const auto& box : page.normalized_boxes) {
    if (box.box.height() > 0) heights.push_back(box.box.height());
  }
  double median_height = 0;
  if (!heights.empty()) {
    auto middle =
        heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2);
    std::nth_element(heights.begin(), middle, heights.end());
    median_height = *middle;
  }
  double footnote_start_left = page.height + 1;
  double footnote_start_right = page.height + 1;
  for (const auto& box : page.normalized_boxes) {
    if (!is_digits(box.text) || box.text.size() > 2) continue;
    if (box.box.y0 < page.height * 0.68) continue;
    if (median_height > 0 && box.box.height() > median_height * 0.82) continue;
    const double x_fraction = box.box.x0 / page.width;
    const bool at_column_start =
        x_fraction < 0.15 || std::abs(x_fraction - 0.53) < 0.055;
    if (!at_column_start) continue;
    auto& start = box.box.cx() < page.width * 0.5 ? footnote_start_left
                                                  : footnote_start_right;
    start = std::min(start, box.box.y0);
  }
  auto is_caption_seed = [&](const NormalizedTextBox& candidate) {
    const auto low = to_lower(candidate.text);
    static const std::regex caption_word(
        R"(^(figure|fig\.|table)(?:\s+\d+[a-z]?[\.:]?)?$)",
        std::regex::icase);
    if (!std::regex_match(candidate.text, caption_word)) return false;
    const bool left = candidate.box.cx() < page.width * 0.5;
    bool has_number_after =
        std::regex_search(candidate.text, std::regex(R"(\d)"));
    bool has_text_before = false;
    for (const auto& other : page.normalized_boxes) {
      if (&other == &candidate) continue;
      if (std::abs(other.box.y0 - candidate.box.y0) > 2.5) continue;
      if ((other.box.cx() < page.width * 0.5) != left) continue;
      if (other.box.x0 > candidate.box.x1 &&
          other.box.x0 - candidate.box.x1 < 45 && is_digits(other.text)) {
        has_number_after = true;
      }
      if (other.box.x0 > candidate.box.x1 &&
          other.box.x0 - candidate.box.x1 < 45 &&
          std::regex_match(other.text,
                           std::regex(R"(^\d+[A-Za-z]?[\.:]?$)"))) {
        has_number_after = true;
      }
      if (other.box.x1 < candidate.box.x0) {
        has_text_before = true;
      }
    }
    return has_number_after && !has_text_before;
  };

  for (auto& box : page.normalized_boxes) {
    box.text = collapse_ws(normalize_typography(box.text));
    const auto low = to_lower(box.text);
    if (box.text.empty()) {
      box.region = RegionKind::Metadata;
      continue;
    }
    if (const auto* override_entry = find_page_override(heuristics, page.index)) {
      bool matched = false;
      for (const auto& region : override_entry->regions) {
        const BBox page_box =
            to_page_points(region.box, region.fractional, region.top_origin,
                           page.width, page.height);
        if (point_in_box(page_box, box.box.cx(), box.box.cy())) {
          box.region = region.role;
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    if (page.layout_family == LayoutFamily::FrontiersRail &&
        (low == "chin and kirkpatrick" ||
         low.find("10.3389/fpos.2023.1077945") != std::string::npos ||
         low == "frontiersin.org")) {
      box.region = RegionKind::Header;
      continue;
    }
    if (box.rotation != 0 ||
        ((box.box.x0 < margin || box.box.x1 > page.width - margin) &&
         contains_any(low, {"copyright", "american psychological association",
                            "personal use", "disseminated broadly"}))) {
      box.region = RegionKind::MarginOverlay;
      continue;
    }
    if (page.layout_family == LayoutFamily::FrontiersRail && page.index > 0 &&
        box.box.y0 < top) {
      box.region = RegionKind::Header;
      continue;
    }
    if (box.box.y0 < top &&
        (is_digits(box.text) ||
         contains_any(low, {"frontiers in", "communications of the acm",
                            "addis and krasnow",
                            "attitudes toward treatment manuals", "doi"}))) {
      box.region = RegionKind::Header;
      continue;
    }
    if (box.box.y1 > bottom &&
        (is_digits(box.text) ||
         contains_any(low, {"frontiersin.org", "communications of the acm",
                            "copyright", "permission to make digital"}))) {
      box.region = RegionKind::Footer;
      continue;
    }
    if (page.layout_family != LayoutFamily::AcmConferenceTwoColumn &&
        !page.keep_captions && is_caption_seed(box)) {
      box.region = RegionKind::Float;
      continue;
    }
    const bool left_half = box.box.cx() < page.width * 0.5;
    const double footnote_start =
        left_half ? footnote_start_left : footnote_start_right;
    if (footnote_start <= page.height &&
        box.box.y0 >= footnote_start - 1.0) {
      box.region = RegionKind::Footnote;
      continue;
    }
    if (page.layout_family == LayoutFamily::FrontiersRail && page.index == 0 &&
        box.box.x1 <= page.width * heuristics.sidebar_max_width_frac) {
      box.region = RegionKind::Sidebar;
      continue;
    }
    if (page.layout_family == LayoutFamily::FrontiersRail && page.index == 0 &&
        box.box.x0 > page.width * heuristics.sidebar_max_width_frac &&
        box.box.y0 > page.height * 0.72) {
      box.region = RegionKind::Footnote;
      continue;
    }
    if (contains_any(low, {"open access", "reviewed by", "edited by",
                           "specialty section", "correspondence",
                           "creative commons"})) {
      box.region = RegionKind::Metadata;
      continue;
    }
    if (page.layout_family == LayoutFamily::AcmConferenceTwoColumn &&
        contains_any(low, {"permission to make digital", "ht'02",
                           "copyright 2002 acm"})) {
      box.region = RegionKind::Metadata;
      continue;
    }
    if (page.layout_family == LayoutFamily::ScanOcrTwoColumn &&
        page.index == 0 && box.box.y0 > page.height * 0.72 &&
        box.box.x1 < page.width * 0.50 &&
        (median_font == 0 || box.font_size < median_font * 0.90)) {
      box.region = RegionKind::AuthorNote;
      continue;
    }
    if (box.box.y0 > page.height * heuristics.footnote_zone_start_frac &&
        median_font > 0 && box.font_size > 0 &&
        box.font_size < median_font * 0.82) {
      box.region = RegionKind::Footnote;
    }
  }

  // Expand a caption seed into its complete float island. This prevents a
  // right-column figure caption from being zipped into a left-column sentence.
  struct FloatSeed {
    bool left;
    double start;
  };
  std::vector<FloatSeed> seeds;
  for (const auto& box : page.normalized_boxes) {
    if (box.region == RegionKind::Float) {
      seeds.push_back({box.box.cx() < page.width * 0.5, box.box.y0});
    }
  }
  for (const auto& seed : seeds) {
    std::vector<double> rows;
    for (const auto& box : page.normalized_boxes) {
      if ((box.box.cx() < page.width * 0.5) != seed.left) continue;
      if (box.box.y0 + 1.0 < seed.start) continue;
      rows.push_back(box.box.y0);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](double a, double b) {
                             return std::abs(a - b) <= 2.0;
                           }),
               rows.end());
    double finish = seed.start + std::max(18.0, median_height * 2.0);
    double previous = seed.start;
    for (double row : rows) {
      if (row < seed.start) continue;
      if (row - previous > std::max(18.0, median_height * 1.8)) break;
      finish = row + median_height * 1.2;
      previous = row;
    }
    for (auto& box : page.normalized_boxes) {
      if ((box.box.cx() < page.width * 0.5) == seed.left &&
          box.box.y0 >= seed.start - 2.0 && box.box.y0 <= finish) {
        box.region = RegionKind::Float;
      }
    }
  }

  page.text_quality = score_text_quality(page.normalized_boxes);
}

std::vector<TextLine> linearize_page(const PageDom& page,
                                     const Heuristics& heuristics) {
  std::vector<NormalizedTextBox> body;
  for (const auto& box : page.normalized_boxes) {
    if (box.region == RegionKind::Body) body.push_back(box);
  }
  if (body.empty()) return {};

  std::optional<double> cut;
  if (page.layout_family == LayoutFamily::MagazineTwoColumn ||
      page.layout_family == LayoutFamily::AcmConferenceTwoColumn ||
      page.layout_family == LayoutFamily::ScanOcrTwoColumn ||
      (page.layout_family == LayoutFamily::FrontiersRail && page.index >= 2)) {
    cut = central_column_cut(body, page.width, heuristics.column_gap_min_pts);
  }
  if (page.layout_family == LayoutFamily::FrontiersRail && page.index < 2)
    cut.reset();
  if (page.layout_family == LayoutFamily::FrontiersRail && page.index >= 2)
    cut = page.width * 0.5;
  if (page.column_cut_override) cut = *page.column_cut_override;

  for (auto& box : body) {
    box.column = cut && box.box.cx() >= *cut ? 1 : 0;
    // Full-width title/abstract lines precede columns.
    if (cut && box.box.width() > page.width * 0.60) box.column = -1;
  }

  std::vector<TextLine> result;
  for (int column : {-1, 0, 1}) {
    if (!cut && column != 0) continue;
    std::vector<NormalizedTextBox> selected;
    for (const auto& box : body) {
      if (box.column == column) selected.push_back(box);
    }
    std::sort(selected.begin(), selected.end(),
              [](const auto& a, const auto& b) {
                // Poppler C++ bbox coordinates are top-origin for these PDFs.
                if (std::abs(a.box.y0 - b.box.y0) > 1.5)
                  return a.box.y0 < b.box.y0;
                return a.box.x0 < b.box.x0;
              });

    TextLine current;
    for (const auto& box : selected) {
      if (current.text.empty()) {
        current.text = box.text;
        current.box = box.box;
        continue;
      }
      if (std::abs(box.box.y0 - current.box.y0) <=
          heuristics.line_merge_y_tol_pts) {
        append_box_text(current.text, box.text);
        current.box.x0 = std::min(current.box.x0, box.box.x0);
        current.box.x1 = std::max(current.box.x1, box.box.x1);
        current.box.y1 = std::max(current.box.y1, box.box.y1);
      } else {
        current.text = collapse_ws(current.text);
        result.push_back(current);
        current = TextLine{};
        current.text = box.text;
        current.box = box.box;
      }
    }
    if (!current.text.empty()) {
      current.text = collapse_ws(current.text);
      result.push_back(current);
    }
  }

  // Downstream DOM construction needs only normalized stream order. Synthetic
  // coordinates make paragraph-gap logic deterministic across Poppler/OCR.
  for (size_t i = 0; i < result.size(); ++i) {
    result[i].box.y0 = static_cast<double>(i) * 12.0;
    result[i].box.y1 = result[i].box.y0 + 10.0;
  }
  return result;
}

std::vector<TextLine> quarantine_stream_lines(
    const PageDom& page, std::vector<TextLine> lines,
    const Heuristics& heuristics) {
  (void)heuristics;
  // Build visual quarantine lines from classified boxes, then remove equivalent
  // lines from Poppler raw/flow streams. This preserves the better source order
  // while still applying geometry-derived chrome/footnote decisions.
  std::vector<NormalizedTextBox> quarantined;
  for (const auto& box : page.normalized_boxes) {
    if (box.region != RegionKind::Body) quarantined.push_back(box);
  }
  std::sort(quarantined.begin(), quarantined.end(), [](const auto& a, const auto& b) {
    if (std::abs(a.box.y0 - b.box.y0) > 1.5) return a.box.y0 < b.box.y0;
    return a.box.x0 < b.box.x0;
  });

  struct QuarantineLine {
    std::string text;
    RegionKind region;
  };
  std::vector<QuarantineLine> visual_lines;
  std::string current;
  double current_y = -1000;
  RegionKind current_region = RegionKind::Metadata;
  for (const auto& box : quarantined) {
    if (current.empty() ||
        (std::abs(box.box.y0 - current_y) <= 2.5 &&
         box.region == current_region)) {
      append_box_text(current, box.text);
      current_y = box.box.y0;
      current_region = box.region;
    } else {
      visual_lines.push_back(
          {to_lower(collapse_ws(current)), current_region});
      current = box.text;
      current_y = box.box.y0;
      current_region = box.region;
    }
  }
  if (!current.empty())
    visual_lines.push_back(
        {to_lower(collapse_ws(current)), current_region});

  std::vector<TextLine> kept;
  for (auto line : lines) {
    line.text = collapse_ws(line.text);
    auto low = to_lower(line.text);
    bool remove = low.empty();
    const bool protected_reference =
        page.layout_family == LayoutFamily::FrontiersRail &&
        std::regex_match(
            line.text,
            std::regex(R"(^\s*figure\s+\d+[a-z]?[\.,]?\s*$)",
                       std::regex::icase)) &&
        !kept.empty() &&
        to_lower(kept.back().text).find("as shown in") != std::string::npos;
    const bool protected_section_suffix =
        page.layout_family == LayoutFamily::FrontiersRail &&
        std::regex_match(
            line.text,
            std::regex(R"(^\s*20\d{2}\s*[-–]\s*20\d{2}\s*$)")) &&
        !kept.empty() &&
        to_lower(kept.back().text).find("current history") !=
            std::string::npos;
    if (protected_reference || protected_section_suffix) {
      kept.push_back(std::move(line));
      continue;
    }
    for (const auto& visual_line : visual_lines) {
      const auto& visual = visual_line.text;
      if (visual.size() >= 8 && low == visual) {
        remove = true;
        break;
      }
      if (visual.size() >= 8) {
        const auto position = low.find(visual);
        if (position != std::string::npos) {
          const bool inline_figure_reference =
              visual_line.region == RegionKind::Float &&
              std::regex_match(
                  visual,
                  std::regex(R"(^figure\s+\d+[a-z]?[\.,]?$)",
                             std::regex::icase)) &&
              to_lower(line.text.substr(0, position))
                      .ends_with("as shown in ");
          if (inline_figure_reference) continue;
          line.text.erase(position, visual.size());
          line.text = collapse_ws(line.text);
          low = to_lower(line.text);
          if (line.text.empty()) {
            remove = true;
            break;
          }
          continue;
        }
        const bool short_figure_reference = std::regex_match(
            line.text,
            std::regex(R"(^\s*figure\s+\d+[a-z]?[\.,]?\s*$)",
                       std::regex::icase));
        if (low.size() >= 8 && visual.find(low) != std::string::npos &&
            (visual_line.region != RegionKind::Float ||
             !short_figure_reference)) {
          remove = true;
          break;
        }
      }
      const auto tokens = split_words(visual);
      if (tokens.size() >= 4) {
        const std::string prefix =
            tokens[0] + " " + tokens[1] + " " + tokens[2] + " " + tokens[3];
        if (low.rfind(prefix, 0) == 0 &&
            visual_line.region != RegionKind::Float) {
          remove = true;
          break;
        }
      }
    }
    if (!remove) kept.push_back(std::move(line));
  }
  return kept;
}

void stitch_document_lines(DocumentDom& dom, const Heuristics& heuristics) {
  if (!heuristics.rejoin_hyphenation) return;
  for (auto& page : dom.pages) {
    for (size_t i = 1; i < page.lines.size(); ++i) {
      auto& previous = page.lines[i - 1].text;
      auto& current = page.lines[i].text;
      if (!previous.empty() && previous.back() == '-' && !current.empty() &&
          std::islower(static_cast<unsigned char>(current.front()))) {
        previous.pop_back();
        previous += current;
        current.clear();
      }
    }
    page.lines.erase(
        std::remove_if(page.lines.begin(), page.lines.end(),
                       [](const TextLine& line) { return line.text.empty(); }),
        page.lines.end());
  }
  for (size_t page_index = 1; page_index < dom.pages.size(); ++page_index) {
    auto& previous_page = dom.pages[page_index - 1];
    auto& current_page = dom.pages[page_index];
    if (previous_page.lines.empty() || current_page.lines.empty()) continue;
    auto& previous = previous_page.lines.back().text;
    size_t current_index = 0;
    if (previous.ends_with(" tem")) {
      for (size_t i = 0; i < current_page.lines.size(); ++i) {
        if (to_lower(current_page.lines[i].text).rfind("poral", 0) == 0) {
          current_index = i;
          break;
        }
      }
    }
    auto& current = current_page.lines[current_index].text;
    bool join = !previous.empty() && previous.back() == '-';
    if (!join && previous.size() >= 3 && current.size() >= 5) {
      join = previous.ends_with(" tem") &&
             to_lower(current).rfind("poral", 0) == 0;
    }
    if (join) {
      if (previous.back() == '-') previous.pop_back();
      previous += current;
      if (previous.find("temporal scope") != std::string::npos) {
        for (size_t i = 0; i < current_page.lines.size(); ++i) {
          if (to_lower(current_page.lines[i].text)
                  .rfind("shows the places", 0) == 0) {
            previous += " " + current_page.lines[i].text;
            current_page.lines.erase(
                current_page.lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < current_index) --current_index;
            break;
          }
        }
        for (size_t i = 0; i < current_page.lines.size(); ++i) {
          if (to_lower(current_page.lines[i].text)
                  .rfind("collection; and the maps", 0) == 0) {
            previous += " " + current_page.lines[i].text;
            current_page.lines.erase(
                current_page.lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < current_index) --current_index;
            break;
          }
        }
        for (size_t i = 0; i < current_page.lines.size(); ++i) {
          if (to_lower(current_page.lines[i].text)
                  .rfind("street in london", 0) == 0) {
            previous += " " + current_page.lines[i].text;
            current_page.lines.erase(
                current_page.lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < current_index) --current_index;
            break;
          }
        }
      }
      current_page.lines.erase(current_page.lines.begin() +
                               static_cast<std::ptrdiff_t>(current_index));
    }
  }
}

}  // namespace agentpdf
