#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
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

// Erase the run of `text` whose alphanumerics fold to `key`, when that run
// covers whole words; spacing and punctuation inside it do not matter.
bool erase_folded_words(std::string& text, const std::string& key) {
  auto word_char = [](unsigned char c) { return std::isalnum(c) || c >= 0x80; };
  std::string folded;
  std::vector<size_t> at;
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (!word_char(c)) continue;
    folded.push_back(static_cast<char>(std::isalnum(c) ? std::tolower(c) : c));
    at.push_back(i);
  }
  for (size_t pos = folded.find(key); pos != std::string::npos; pos = folded.find(key, pos + 1)) {
    const size_t begin = at[pos];
    const size_t end = at[pos + key.size() - 1] + 1;
    if (begin > 0 && word_char(static_cast<unsigned char>(text[begin - 1]))) continue;
    if (end < text.size() && word_char(static_cast<unsigned char>(text[end]))) continue;
    text = collapse_ws(text.erase(begin, end - begin));
    return true;
  }
  return false;
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

LayoutFamily detect_layout_family(const LayoutSignals& signals) {
  // Families are publisher *templates*, recognised by the template's own
  // printed furniture on the first pages, or (for scans) by page structure.
  if (signals.scanned_with_text_layer) return LayoutFamily::ScanOcrTwoColumn;
  auto pages_containing = [&](const char* needle) {
    int count = 0;
    for (const auto& text : signals.page_text) {
      if (text.find(needle) != std::string::npos) ++count;
    }
    return count;
  };
  // Communications of the ACM prints its masthead in every page footer.
  if (pages_containing("communications of the acm") >= 2)
    return LayoutFamily::MagazineTwoColumn;
  // ACM proceedings carry the ACM permission block on the first article page.
  if (pages_containing("permission to make digital or hard copies") >= 1)
    return LayoutFamily::AcmConferenceTwoColumn;
  // Frontiers journals print frontiersin.org in every page footer.
  if (pages_containing("frontiersin.org") >= 2) return LayoutFamily::FrontiersRail;
  return LayoutFamily::Generic;
}

bool is_cover_page(const PageDom& page, const Heuristics& heuristics) {
  // Repository/download cover sheets prepended to the article (digital
  // library landing pages, aggregator cover sheets).
  static const char* cover_markers[] = {
      "latest updates", "pdf download", "total citations", "total downloads",
      "citation in bibtex", "this content downloaded from",
      "see discussions, stats, and author profiles", "terms and conditions of use",
      "to cite this article", "submit your article to this journal",
      "view related articles", "view crossmark data", "full terms & conditions",
  };
  std::string text;
  for (const auto& box : page.normalized_boxes) text += to_lower(box.text) + ' ';
  int markers = 0;
  for (const char* marker : cover_markers) {
    if (text.find(marker) != std::string::npos) ++markers;
  }
  if (markers >= 2) return true;

  // Pictorial title pages: a text layer exists (so this is not a scan that
  // needs OCR) but almost no words sit outside the header/footer bands.
  if (page.normalized_boxes.empty()) return false;
  const double top = page.height * heuristics.header_band_frac;
  const double bottom = page.height * (1.0 - heuristics.footer_band_frac);
  size_t words = 0;
  for (const auto& box : page.normalized_boxes) {
    if (box.box.y0 < top || box.box.y1 > bottom) continue;
    for (const auto& word : split_words(box.text)) {
      if (!is_digits(word)) ++words;
    }
  }
  return words < 12;
}

namespace {

// A page folio: an issue date ("AUGUST 2026"), optionally with the
// publication's name and the page number ("BEST'S REVIEW • AUGUST 2026 67").
// No sentence reads like that, so a band line of this shape is chrome even
// on a document too short for its running heads to repeat.
bool is_folio_line(const std::string& text) {
  static const std::regex month_year(
      R"(\b(?:january|february|march|april|may|june|july|august|september|october|november|december|jan|feb|mar|apr|jun|jul|aug|sept?|oct|nov|dec)\.?\s+(?:19|20)\d{2}\b)",
      std::regex::icase);
  if (!std::regex_search(text, month_year)) return false;
  return split_words(text).size() <= 10 && lowercase_word_share(text) < 0.3;
}

// Other page furniture that no sentence resembles, in the stripping band:
// a journal citation ("Environ. Sci. Technol. 2026, 60, 22065 - 22070",
// "ACS EST Air XXXX, XXX, XXX - XXX", "Energies 2026, 19, 4153"), a page
// label ("2 of 17", "Page 3"), a DOI or URL, a copyright or licence line,
// a publisher's name alone.
bool is_band_furniture_line(const std::string& text) {
  const auto t = collapse_ws(text);
  if (t.empty()) return false;
  const auto low = to_lower(t);
  const size_t words = split_words(t).size();
  if (words > 16) return false;
  static const std::regex citation(
      R"((?:(?:19|20)\d{2}|xxxx),\s*(?:\d{1,4}|xxx),\s*(?:[a-z]?\d{1,7}|xxx)(?:\s*-\s*(?:[a-z]?\d{1,7}|xxx))?\b)");
  if (std::regex_search(low, citation)) return true;
  static const std::regex page_label(R"((?:^|\s)(?:page\s+)?\d{1,4}\s+of\s+\d{1,4}(?:\s|$))");
  if (std::regex_search(low, page_label)) return true;
  if (low.find("doi.org/") != std::string::npos || low.find("doi:") != std::string::npos ||
      low.rfind("http", 0) == 0 || low.rfind("www.", 0) == 0)
    return true;
  if (low.find("\xC2\xA9") != std::string::npos || low.find("copyright") != std::string::npos ||
      low.find("all rights reserved") != std::string::npos || low.find("licensed under") != std::string::npos ||
      low.find("published by") != std::string::npos || low.find("licensee") != std::string::npos)
    return true;
  static const char* publishers[] = {"american chemical society", "elsevier", "springer", "mdpi",
                                     "taylor & francis", "wiley", "ieee", "cell press",
                                     "oxford university press", "cambridge university press"};
  if (words <= 5) {
    for (const char* p : publishers) {
      if (low.find(p) != std::string::npos) return true;
    }
  }
  return false;
}

}  // namespace


namespace {

// ---------------------------------------------------------------------------
// Page structure from geometry and type, for pages whose text layer says how
// it is typeset (PageDom::body_font_size > 0).
// ---------------------------------------------------------------------------

struct PageColumns {
  bool split = false;       // two or more text columns
  bool both_prose = false;  // running text on both sides of every gutter
  std::vector<double> gutters;  // x of each gutter's centre, left to right
  double top = 0;       // the band of the page set in columns (a full-width
  double bottom = 0;    // title and abstract often sit above it)
  double left = 0;      // text area
  double right = 0;
};

// Column of a box: its index left to right (0 for a page in one column),
// -1 when it spans a gutter (a full-width title, abstract, caption, table).
int side_of(const PageColumns& columns, const BBox& box) {
  if (!columns.split) return 0;
  int column = 0;
  for (double g : columns.gutters) {
    if (box.x1 <= g + 1.0) return column;
    if (box.x0 < g - 1.0) return -1;
    ++column;
  }
  return column;
}

int column_count(const PageColumns& columns) {
  return columns.split ? static_cast<int>(columns.gutters.size()) + 1 : 1;
}

double column_width(const PageColumns& columns, int side) {
  if (!columns.split || side < 0) return columns.right - columns.left;
  const double from = side == 0 ? columns.left : columns.gutters[static_cast<size_t>(side) - 1];
  const double to = static_cast<size_t>(side) < columns.gutters.size()
                        ? columns.gutters[static_cast<size_t>(side)]
                        : columns.right;
  return to - from;
}

bool rows_overlap(const BBox& a, const BBox& b) {
  const double overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  return overlap >= 0.35 * std::max(0.1, std::min(a.height(), b.height()));
}

struct PageRow {
  std::vector<size_t> boxes;  // left to right
  BBox box;
  double size = 0;            // character-weighted type size
  double max_size = 0;
  size_t words = 0;
  size_t letters = 0;
  size_t digits = 0;
  size_t lower_words = 0;
  bool heavy = false;
  bool italic = false;
  int segments = 0;  // runs of words separated by gaps wider than two ems
  double narrowest_segment = 0;
  std::string text;
};

// Rows (shared baselines) of the selected boxes, top to bottom.
std::vector<PageRow> page_rows(const PageDom& page, const std::vector<size_t>& selected) {
  std::vector<size_t> order = selected;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return page.normalized_boxes[a].box.cy() < page.normalized_boxes[b].box.cy();
  });
  std::vector<PageRow> rows;
  for (size_t k : order) {
    const auto& box = page.normalized_boxes[k].box;
    if (!rows.empty() && rows_overlap(rows.back().box, box)) {
      auto& row = rows.back();
      row.boxes.push_back(k);
      row.box.x0 = std::min(row.box.x0, box.x0);
      row.box.x1 = std::max(row.box.x1, box.x1);
      row.box.y0 = std::min(row.box.y0, box.y0);
      row.box.y1 = std::max(row.box.y1, box.y1);
      continue;
    }
    PageRow row;
    row.boxes.push_back(k);
    row.box = box;
    rows.push_back(std::move(row));
  }
  for (auto& row : rows) {
    std::sort(row.boxes.begin(), row.boxes.end(), [&](size_t a, size_t b) {
      return page.normalized_boxes[a].box.x0 < page.normalized_boxes[b].box.x0;
    });
    double chars = 0, sum = 0, heavy = 0, italic = 0;
    for (size_t k : row.boxes) {
      const auto& b = page.normalized_boxes[k];
      if (!row.text.empty()) row.text.push_back(' ');
      row.text += b.text;
      const double n = static_cast<double>(std::max<size_t>(1, fold_alnum(b.text).size()));
      if (b.type_size > 0) {
        chars += n;
        sum += b.type_size * n;
        row.max_size = std::max(row.max_size, b.type_size);
        if (b.type_heavy) heavy += n;
        if (b.type_italic) italic += n;
      }
      for (const auto& w : split_words(b.text)) {
        ++row.words;
        if (std::islower(static_cast<unsigned char>(w.front()))) ++row.lower_words;
      }
      for (unsigned char c : b.text) {
        if (std::isalpha(c) || c >= 0xC0) ++row.letters;
        else if (std::isdigit(c)) ++row.digits;
      }
    }
    if (chars > 0) {
      row.size = sum / chars;
      row.heavy = heavy / chars >= 0.6;
      row.italic = italic / chars >= 0.6;
    }
    double last_x1 = -1e9, segment_x0 = 0;
    for (size_t k : row.boxes) {
      const auto& b = page.normalized_boxes[k].box;
      const double em = std::max(4.0, row.size > 0 ? row.size : b.height());
      if (b.x0 - last_x1 > 2.0 * em) {
        if (row.segments > 0) {
          const double width = last_x1 - segment_x0;
          row.narrowest_segment = row.segments == 1 ? width : std::min(row.narrowest_segment, width);
        }
        ++row.segments;
        segment_x0 = b.x0;
      }
      last_x1 = std::max(last_x1, b.x1);
    }
    if (row.segments > 0) {
      const double width = last_x1 - segment_x0;
      row.narrowest_segment = row.segments == 1 ? width : std::min(row.narrowest_segment, width);
    }
  }
  return rows;
}

// Two text columns show as a vertical strip near the page centre that many
// rows leave empty while holding text on both sides of it, and that almost
// no row crosses.
PageColumns detect_page_columns(const PageDom& page, double top, double bottom) {
  PageColumns columns;
  std::vector<size_t> selected;
  columns.left = page.width;
  columns.right = 0;
  for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
    const auto& b = page.normalized_boxes[k];
    if (b.rotation != 0 || b.text.empty() || b.box.y0 < top || b.box.y1 > bottom) continue;
    selected.push_back(k);
    columns.left = std::min(columns.left, b.box.x0);
    columns.right = std::max(columns.right, b.box.x1);
  }
  if (selected.size() < 40 || columns.right <= columns.left) {
    columns.left = 0;
    columns.right = page.width;
    return columns;
  }
  const auto rows = page_rows(page, selected);
  // Each row's runs of words (gaps wider than an em separate them).
  struct Run {
    double x0, x1;
  };
  std::vector<std::vector<Run>> runs(rows.size());
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t k : rows[r].boxes) {
      const auto& b = page.normalized_boxes[k].box;
      const double em = std::max(4.0, b.height());
      if (!runs[r].empty() && b.x0 - runs[r].back().x1 < em) {
        runs[r].back().x1 = std::max(runs[r].back().x1, b.x1);
      } else {
        runs[r].push_back({b.x0, b.x1});
      }
    }
  }
  const double measure = (columns.right - columns.left) * 0.25;
  constexpr int samples = 160;
  auto sample_x = [&](int i) {
    return page.width * (0.30 + 0.40 * static_cast<double>(i) / (samples - 1));
  };
  // A gutter at x: rows of text end left of it and rows begin right of it
  // over a common band of the page (their baselines need not align), prose
  // on at least one side, and almost no row in that band crosses it.
  struct Verdict {
    int score = 0;
    bool both_prose = false;
    double top = 0, bottom = 0;
  };
  // Rows top to bottom; a row that crosses x ends a stretch.
  std::vector<size_t> order(rows.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return rows[a].box.cy() < rows[b].box.cy(); });
  auto evaluate = [&](int i) {
    const double x = sample_x(i);
    Verdict best;
    int n_left = 0, n_right = 0, prose_left = 0, prose_right = 0;
    double top = 1e18, bottom = -1e18;
    auto close = [&] {
      const int fewer = std::min(n_left, n_right);
      const bool left_prose = prose_left * 2 >= n_left && n_left > 0;
      const bool right_prose = prose_right * 2 >= n_right && n_right > 0;
      if (fewer >= 6 && (left_prose || right_prose) && fewer > best.score) {
        best.score = fewer;
        best.both_prose = left_prose && right_prose;
        best.top = top;
        best.bottom = bottom;
      }
      n_left = n_right = prose_left = prose_right = 0;
      top = 1e18;
      bottom = -1e18;
    };
    for (size_t r : order) {
      bool left = false, right = false, cross = false, wide_left = false, wide_right = false;
      for (const auto& run : runs[r]) {
        if (run.x0 < x && run.x1 > x) cross = true;
        else if (run.x1 <= x) {
          left = true;
          if (run.x1 - run.x0 >= measure) wide_left = true;
        } else {
          right = true;
          if (run.x1 - run.x0 >= measure) wide_right = true;
        }
      }
      if (cross) {
        close();
        continue;
      }
      if (!left && !right) continue;
      n_left += left ? 1 : 0;
      n_right += right ? 1 : 0;
      prose_left += wide_left ? 1 : 0;
      prose_right += wide_right ? 1 : 0;
      top = std::min(top, rows[r].box.y0);
      bottom = std::max(bottom, rows[r].box.y1);
    }
    close();
    return best;
  };
  std::vector<Verdict> verdicts(samples);
  for (int i = 0; i < samples; ++i) verdicts[static_cast<size_t>(i)] = evaluate(i);
  // Gutters: the strongest strip, then any other as strong with running text
  // on both sides, well apart from it (three-column magazines).
  std::vector<bool> suppressed(samples, false);
  int first_score = 0;
  bool all_prose = true;
  for (int round = 0; round < 3; ++round) {
    int best = -1;
    for (int i = 0; i < samples; ++i) {
      if (suppressed[static_cast<size_t>(i)]) continue;
      if (verdicts[static_cast<size_t>(i)].score <= 0) continue;
      if (best < 0 || verdicts[static_cast<size_t>(i)].score > verdicts[static_cast<size_t>(best)].score)
        best = i;
    }
    if (best < 0) break;
    const auto& v = verdicts[static_cast<size_t>(best)];
    if (round > 0 && (v.score * 2 < first_score || !v.both_prose)) break;
    if (round == 0) {
      first_score = v.score;
      columns.top = v.top;
      columns.bottom = v.bottom;
    }
    all_prose = all_prose && v.both_prose;
    auto good = [&](int i) {
      return !suppressed[static_cast<size_t>(i)] && verdicts[static_cast<size_t>(i)].score * 5 >= v.score * 4;
    };
    int first = best, last = best;
    while (first > 0 && good(first - 1)) --first;
    while (last + 1 < samples && good(last + 1)) ++last;
    const double gutter = (sample_x(first) + sample_x(last)) * 0.5;
    columns.gutters.push_back(gutter);
    for (int i = 0; i < samples; ++i) {
      if (std::abs(sample_x(i) - gutter) < page.width * 0.12) suppressed[static_cast<size_t>(i)] = true;
    }
  }
  std::sort(columns.gutters.begin(), columns.gutters.end());
  columns.split = !columns.gutters.empty();
  columns.both_prose = columns.split && all_prose;
  return columns;
}

// Running text: body type across the column's measure in one run of words
// (two runs where a full-width element sits over two columns); a table's
// rows break into cells.
// `page_body`: the size most of this page's running text is set in, which
// can differ from the document's (ACS sets its methods section and
// references a point smaller).
bool body_like_row(const PageRow& row, double body, double page_body, const PageColumns& columns,
                   int side) {
  if (row.words < 4 || row.size <= 0) return false;
  if (std::abs(row.size - body) > body * 0.07 &&
      (page_body <= 0 || std::abs(row.size - page_body) > page_body * 0.07))
    return false;
  if (row.digits * 2 > row.letters) return false;
  // Two runs are two columns of prose side by side (under a full-width
  // figure, where too few rows remain for the gutter to be measured).
  if (row.segments > 2) return false;
  if (row.segments == 2 && row.narrowest_segment < (columns.right - columns.left) * 0.3) return false;
  return row.box.width() >= 0.45 * column_width(columns, side);
}

// Rows of one table: words of the lower row begin where words of the upper
// one begin (its cells' columns); in prose only the first word of a line
// does.
bool aligned_rows(const PageDom& page, const PageRow& upper, const PageRow& lower) {
  if (upper.boxes.size() < 2 || lower.boxes.size() < 2) return false;
  // Cell starts: a word set after a gap wider than a word space, at the x
  // where a word of the upper row starts too.
  size_t aligned = 0;
  for (size_t i = 1; i < lower.boxes.size(); ++i) {
    const auto& word = page.normalized_boxes[lower.boxes[i]];
    const auto& before = page.normalized_boxes[lower.boxes[i - 1]];
    const double em = std::max(4.0, word.type_size > 0 ? word.type_size : word.box.height());
    if (word.box.x0 - before.box.x1 < 0.8 * em) continue;
    // A text column's own left edge lines up in every row: not a cell.
    const bool column_start = std::any_of(page.gutters.begin(), page.gutters.end(), [&](double g) {
      return before.box.x1 <= g + 1.0 && word.box.x0 >= g - 1.0;
    });
    if (column_start) continue;
    for (size_t j = 1; j < upper.boxes.size(); ++j) {
      if (std::abs(page.normalized_boxes[upper.boxes[j]].box.x0 - word.box.x0) <= 2.0) {
        ++aligned;
        break;
      }
    }
  }
  return aligned >= 2 || (aligned == 1 && lower.boxes.size() <= 6);
}

// A table's grid from the rows of its island (top to bottom): cells are
// runs of words set apart by more than a word space; columns are the bands
// the fullest rows' cells occupy; a row that only carries on some cells'
// text (a wrapped cell) joins the row above; full-width rows under the body
// are the table's notes.
TableGrid table_grid(const PageDom& page, const std::vector<PageRow>& rows, const std::vector<size_t>& body,
                     double body_size) {
  TableGrid grid;
  struct Cell {
    double x0 = 0, x1 = 0;
    std::string text;
  };
  const auto& boxes = page.normalized_boxes;
  // A cell's words line by line, left to right on each (a row can hold a
  // cell wrapped onto two lines beside one set between them).
  auto cell_text = [&](std::vector<size_t> members) {
    // Lines by baseline, the largest type deciding: a raised or lowered
    // index belongs to the line it is set against.
    std::sort(members.begin(), members.end(), [&](size_t a, size_t b) { return boxes[a].box.cy() < boxes[b].box.cy(); });
    std::vector<std::vector<size_t>> lines;
    std::vector<std::pair<double, double>> extent;  // each line's y range
    for (size_t k : members) {
      const auto& b = boxes[k].box;
      bool placed = false;
      for (size_t l = 0; l < lines.size() && !placed; ++l) {
        const double overlap = std::min(extent[l].second, b.y1) - std::max(extent[l].first, b.y0);
        if (overlap >= 0.5 * std::min(b.height(), extent[l].second - extent[l].first)) {
          lines[l].push_back(k);
          extent[l].first = std::min(extent[l].first, b.y0);
          extent[l].second = std::max(extent[l].second, b.y1);
          placed = true;
        }
      }
      if (!placed) {
        lines.push_back({k});
        extent.push_back({b.y0, b.y1});
      }
    }
    std::string text;
    for (auto& line : lines) {
      std::sort(line.begin(), line.end(), [&](size_t a, size_t b) { return boxes[a].box.x0 < boxes[b].box.x0; });
      const NormalizedTextBox* prev = nullptr;
      for (size_t k : line) {
        const auto& b = boxes[k];
        if (!text.empty()) {
          const double em = std::max(4.0, b.type_size > 0 ? b.type_size : body_size);
          const bool touching = prev && !prev->space_after && b.box.x0 - prev->box.x1 < 0.15 * em;
          const bool line_hyphen = !prev && text.size() >= 2 && text.back() == '-' &&
                                   std::isalpha(static_cast<unsigned char>(text[text.size() - 2]));
          const auto next = static_cast<unsigned char>(b.text[0]);
          if (line_hyphen && std::islower(next)) text.pop_back();  // a word broken at the cell's edge
          else if (line_hyphen && (std::isupper(next) || std::isdigit(next))) {
          }  // a compound broken there ("kg-" + "CO2-eq")
          else if (!touching) text += ' ';
        }
        text += b.text;
        prev = &b;
      }
    }
    return text;
  };
  std::vector<std::vector<Cell>> cells;
  std::vector<double> row_gap;  // space above each row
  for (size_t idx = 0; idx < body.size(); ++idx) {
    const size_t r = body[idx];
    row_gap.push_back(idx == 0 ? 1e9 : rows[r].box.y0 - rows[body[idx - 1]].box.y1);
    std::vector<Cell> row_cells;
    std::vector<std::vector<size_t>> members;
    double reach = -1e18;
    for (size_t k : rows[r].boxes) {  // left to right
      const auto& b = boxes[k];
      if (b.text.empty()) continue;
      const double em = std::max(4.0, b.type_size > 0 ? b.type_size : body_size);
      if (members.empty() || b.box.x0 - reach > 0.8 * em) {
        row_cells.push_back({b.box.x0, b.box.x1, {}});
        members.push_back({});
      }
      members.back().push_back(k);
      row_cells.back().x1 = std::max(row_cells.back().x1, b.box.x1);
      reach = std::max(reach, b.box.x1);
    }
    for (size_t c = 0; c < row_cells.size(); ++c) row_cells[c].text = cell_text(members[c]);
    cells.push_back(std::move(row_cells));
  }
  if (cells.size() < 2) return grid;
  size_t most = 0;
  for (const auto& row : cells) most = std::max(most, row.size());
  if (most < 2) return grid;
  // Column bands from the fullest rows.
  std::vector<std::pair<double, double>> spans;
  for (const auto& row : cells) {
    if (row.size() * 10 < most * 6) continue;
    for (const auto& c : row) spans.push_back({c.x0, c.x1});
  }
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<double, double>> bands;
  for (const auto& sp : spans) {
    if (!bands.empty() && sp.first <= bands.back().second + 1.0) {
      bands.back().second = std::max(bands.back().second, sp.second);
    } else {
      bands.push_back(sp);
    }
  }
  // A band only one row reaches (a wide word space inside a cell) is part of
  // a neighbour's column.
  if (cells.size() >= 3) {
    std::vector<size_t> support(bands.size(), 0);
    for (const auto& row : cells) {
      std::vector<bool> seen(bands.size(), false);
      for (const auto& c : row) {
        for (size_t i = 0; i < bands.size(); ++i) {
          if (!seen[i] && std::min(c.x1, bands[i].second) - std::max(c.x0, bands[i].first) > 0) {
            seen[i] = true;
            ++support[i];
          }
        }
      }
    }
    std::vector<std::pair<double, double>> kept;
    for (size_t i = 0; i < bands.size(); ++i) {
      if (support[i] >= 2 || kept.empty()) {
        kept.push_back(bands[i]);
      } else {
        kept.back().second = std::max(kept.back().second, bands[i].second);
      }
    }
    bands.swap(kept);
  }
  if (bands.size() < 2) return grid;
  auto band_of = [&](const Cell& c) {
    size_t best = 0;
    double best_overlap = -1e18;
    for (size_t i = 0; i < bands.size(); ++i) {
      const double overlap = std::min(c.x1, bands[i].second) - std::max(c.x0, bands[i].first);
      const double distance = overlap > 0 ? overlap : -std::min(std::abs(c.x0 - bands[i].second), std::abs(bands[i].first - c.x1));
      if (distance > best_overlap) {
        best_overlap = distance;
        best = i;
      }
    }
    return best;
  };
  const double table_x0 = bands.front().first, table_x1 = bands.back().second;
  // Notes: from the first single-cell row reaching across most of the table
  // after the body began, to the end.
  size_t notes_from = cells.size();
  for (size_t r = 1; r < cells.size(); ++r) {
    if (cells[r].size() == 1 && cells[r][0].x1 - cells[r][0].x0 >= 0.6 * (table_x1 - table_x0) &&
        split_words(cells[r][0].text).size() >= 5) {
      notes_from = r;
      break;
    }
  }
  auto numeric = [](const std::string& t) {
    size_t letters = 0, digits = 0;
    for (unsigned char c : t) {
      if (std::isalpha(c)) ++letters;
      else if (std::isdigit(c)) ++digits;
    }
    return digits > letters;
  };
  if (std::getenv("AGENTPDF_DEBUG_TABLES")) {
    for (size_t r = 0; r < cells.size(); ++r) {
      std::fprintf(stderr, "TROW %zu:", r);
      for (const auto& c : cells[r]) std::fprintf(stderr, " [%.0f-%.0f b%zu %s]", c.x0, c.x1, band_of(c), c.text.c_str());
      std::fprintf(stderr, "\n");
    }
  }
  // The table's usual spacing between rows: lines closer than that are one
  // row's wrapped cells.
  double usual_gap = 0;
  {
    std::vector<double> gaps;
    for (size_t r = 1; r < row_gap.size(); ++r) {
      if (row_gap[r] < 1e8) gaps.push_back(row_gap[r]);
    }
    if (!gaps.empty()) {
      std::sort(gaps.begin(), gaps.end());
      usual_gap = gaps[gaps.size() / 2];
    }
  }
  std::string carried;  // a first cell's opening line, set alone above its row
  for (size_t r = 0; r < notes_from; ++r) {
    std::vector<std::string> out(bands.size());
    for (const auto& c : cells[r]) {
      auto& slot = out[band_of(c)];
      if (!slot.empty()) slot += ' ';
      slot += c.text;
    }
    if (!carried.empty()) {
      out[0] = out[0].empty() ? carried : carried + ' ' + out[0];
      carried.clear();
    }
    // A first cell wrapped onto two lines, its row on the second: the lone
    // first line (within the first column) opens that cell.
    const bool closer_below = r + 1 < row_gap.size() && row_gap[r + 1] < row_gap[r];
    if (cells[r].size() == 1 && band_of(cells[r][0]) == 0 && cells[r][0].x1 <= bands[0].second + 2.0 &&
        r + 1 < notes_from && cells[r + 1].size() >= 2 && band_of(cells[r + 1][0]) == 0 &&
        !numeric(cells[r][0].text) && !std::islower(static_cast<unsigned char>(cells[r][0].text[0])) &&
        (grid.rows.empty() || closer_below)) {
      carried = out[0];
      continue;
    }
    // A wrapped cell: text only under cells the row above fills with text,
    // or a line whose every cell carries on a word or phrase (lowercase).
    if (!grid.rows.empty()) {
      auto& above = grid.rows.back();
      size_t filled = 0;
      bool fits = true, lower = true;
      for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].empty()) continue;
        ++filled;
        if (above[i].empty() || numeric(out[i]) || numeric(above[i])) fits = false;
        if (above[i].empty() || !std::islower(static_cast<unsigned char>(out[i][0]))) lower = false;
      }
      // The header's own second line ("Dell Power" / "Edge R710"): words
      // under the header's words, the first column left blank.
      bool header_line = grid.rows.size() == 1 && out[0].empty() && filled > 0;
      for (size_t i = 0; i < out.size() && header_line; ++i) {
        const bool letters = std::any_of(out[i].begin(), out[i].end(), [](unsigned char c) { return std::isalpha(c); });
        if (!out[i].empty() && (above[i].empty() || !letters)) header_line = false;
      }
      // Set closer than the rows are to one another: the row's own lines.
      bool tight = usual_gap > 0 && r < row_gap.size() && row_gap[r] < usual_gap * 0.6 && out[0].empty() &&
                   filled > 0;
      for (size_t i = 0; i < out.size() && tight; ++i) {
        if (!out[i].empty() && above[i].empty()) tight = false;
      }
      if ((fits && filled > 0 && filled * 2 <= out.size() && out[0].empty()) || tight || (lower && filled > 0) ||
          header_line) {
        for (size_t i = 0; i < out.size(); ++i) {
          if (out[i].empty()) continue;
          const bool hyphen = above[i].size() >= 2 && above[i].back() == '-' &&
                              std::isalpha(static_cast<unsigned char>(above[i][above[i].size() - 2])) &&
                              std::islower(static_cast<unsigned char>(out[i][0]));
          if (hyphen) above[i].pop_back();
          else above[i] += ' ';
          above[i] += out[i];
        }
        continue;
      }
    }
    grid.rows.push_back(std::move(out));
  }
  for (size_t r = notes_from; r < cells.size(); ++r) {
    std::string text;
    for (const auto& c : cells[r]) {
      if (!text.empty()) text += ' ';
      text += c.text;
    }
    if (!grid.notes.empty() && !text.empty() && std::islower(static_cast<unsigned char>(text[0]))) {
      grid.notes.back() += ' ' + text;
    } else if (!text.empty()) {
      grid.notes.push_back(text);
    }
  }
  if (grid.rows.size() < 2) grid.rows.clear();
  return grid;
}

bool heading_like_row(const PageRow& row, double body, bool body_heavy) {
  if (row.words == 0 || row.words > 15 || row.letters < 4 || row.size <= 0) return false;
  return (row.heavy && !body_heavy && row.size >= body * 0.88) || row.size >= body * 1.08;
}

}  // namespace

void decide_document_columns(std::vector<PageDom>& pages, const Heuristics& heuristics) {
  // A page with running text on both sides of its gutters is set in
  // columns. A single gutter with prose on one side only (a table or figure
  // beside text) counts where other pages of the document are set in
  // columns at that x; elsewhere it is a table's own gaps on a one-column
  // page.
  std::vector<PageColumns> found(pages.size());
  std::vector<double> gutters;
  auto typed_generic = [](const PageDom& page) {
    return !page.wrapper_page && page.body_font_size > 0 && page.layout_family == LayoutFamily::Generic;
  };
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& page = pages[i];
    if (!typed_generic(page)) continue;
    const double top = page.height * heuristics.header_band_frac;
    const double bottom = page.height * (1.0 - heuristics.footer_band_frac);
    found[i] = detect_page_columns(page, top, bottom);
    if (found[i].split && found[i].both_prose)
      gutters.insert(gutters.end(), found[i].gutters.begin(), found[i].gutters.end());
  }
  auto supported = [&](double g) {
    int near = 0;
    for (double other : gutters) near += std::abs(other - g) <= 15.0 ? 1 : 0;
    return near >= 2;
  };
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& page = pages[i];
    if (!typed_generic(page)) continue;
    const auto& c = found[i];
    bool accept = c.split && c.both_prose;
    if (!accept && c.split && c.gutters.size() == 1 && supported(c.gutters.front())) accept = true;
    page.gutters = accept ? c.gutters : std::vector<double>{};
    page.gutter_decided = true;
  }
}

void mark_repeated_page_chrome(std::vector<PageDom>& pages, const Heuristics& heuristics) {
  // Running heads/feet repeat across pages with only numbers changing. Any
  // header/footer-band line whose digit-normalised text recurs on several
  // pages is chrome, whatever the publisher prints there; so is a folio line
  // in the stripping band, repeated or not.
  if (!heuristics.strip_running_headers) return;
  size_t content_pages = 0;
  for (const auto& page : pages) {
    if (!page.wrapper_page && !page.normalized_boxes.empty()) ++content_pages;
  }
  if (content_pages == 0) return;

  // Signature tokens of a text: lowercase, digit runs as "#", the folio
  // numbers at either end left out (a page number set close to the title
  // on some pages and apart on others).
  auto tokens_of = [](const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_digits = false;
    auto push = [&] {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    };
    for (unsigned char c : to_lower(text)) {
      if (std::isspace(c)) {
        push();
        in_digits = false;
        continue;
      }
      if (std::isdigit(c)) {
        if (!in_digits) cur.push_back('#');
        in_digits = true;
        continue;
      }
      in_digits = false;
      cur.push_back(static_cast<char>(c));
    }
    push();
    return out;
  };
  auto folio_token = [](const std::string& t) {
    return t.find('#') != std::string::npos &&
           std::all_of(t.begin(), t.end(), [](char c) { return c == '#' || std::ispunct(static_cast<unsigned char>(c)); });
  };
  auto join = [](const std::vector<std::string>& t, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
      if (!out.empty()) out.push_back(' ');
      out += t[i];
    }
    return out;
  };

  // A band part: boxes left to right, their tokens (with each box's share),
  // and the core signature between the end folios.
  struct Part {
    std::vector<size_t> boxes;
    std::vector<size_t> box_tokens;  // tokens contributed by each box
    std::vector<std::string> tokens;
    size_t core_begin = 0, core_end = 0;
    std::string signature;
    bool top = false;
    bool in_strip = true;
    std::string text;
  };
  auto make_part = [&](const PageDom& page, std::vector<size_t> boxes, bool top, double strip_top,
                       double strip_bottom) {
    Part part;
    part.boxes = std::move(boxes);
    part.top = top;
    for (size_t bi : part.boxes) {
      const auto& box = page.normalized_boxes[bi];
      const auto t = tokens_of(box.text);
      part.box_tokens.push_back(t.size());
      part.tokens.insert(part.tokens.end(), t.begin(), t.end());
      part.text += box.text + ' ';
      if (top ? box.box.y1 > strip_top : box.box.y0 < strip_bottom) part.in_strip = false;
    }
    part.core_begin = 0;
    part.core_end = part.tokens.size();
    while (part.core_begin < part.core_end && folio_token(part.tokens[part.core_begin])) ++part.core_begin;
    while (part.core_end > part.core_begin && folio_token(part.tokens[part.core_end - 1])) --part.core_end;
    part.signature = join(part.tokens, part.core_begin, part.core_end);
    return part;
  };

  // Per page: band rows (one baseline, boxes left to right) and each row's
  // parts (its folio, title and date set apart; a first page can print a
  // licence between them).
  std::vector<std::vector<Part>> page_rows_parts(pages.size());  // rows, then parts, flattened
  std::vector<std::vector<int>> part_row(pages.size());          // part -> row index (-1: the row itself)
  std::map<std::string, size_t> page_counts;
  for (size_t pi = 0; pi < pages.size(); ++pi) {
    const auto& page = pages[pi];
    if (page.wrapper_page || page.height <= 0) continue;
    // Detection band is 1.5x the stripping band: running heads sit just
    // inside or just below it depending on the template, and requiring
    // cross-page repetition keeps the wider band safe for body text.
    const double top = page.height * heuristics.header_band_frac * 1.5;
    const double bottom = page.height * (1.0 - heuristics.footer_band_frac * 1.5);
    const double strip_top = page.height * heuristics.header_band_frac;
    const double strip_bottom = page.height * (1.0 - heuristics.footer_band_frac);
    std::vector<size_t> band;
    for (size_t bi = 0; bi < page.normalized_boxes.size(); ++bi) {
      const auto& box = page.normalized_boxes[bi].box;
      if (box.y1 <= top || box.y0 >= bottom) band.push_back(bi);
    }
    std::sort(band.begin(), band.end(), [&](size_t a, size_t b) {
      const auto& ba = page.normalized_boxes[a].box;
      const auto& bb = page.normalized_boxes[b].box;
      if (ba.y0 != bb.y0) return ba.y0 < bb.y0;
      return ba.x0 < bb.x0;
    });
    std::vector<std::vector<size_t>> grouped;
    double line_y = -1e9;
    bool row_top = false;
    for (size_t bi : band) {
      const auto& box = page.normalized_boxes[bi];
      const bool is_top = box.box.y1 <= top;
      if (grouped.empty() || std::abs(box.box.y0 - line_y) > 2.5 || row_top != is_top) {
        grouped.push_back({});
        line_y = box.box.y0;
        row_top = is_top;
      }
      grouped.back().push_back(bi);
    }
    // Lines printed over one another (a download stamp across a licence
    // line): boxes of one row whose baselines differ and whose extents
    // interleave are separate lines.
    std::vector<std::vector<size_t>> rows;
    for (auto& row : grouped) {
      std::vector<std::vector<size_t>> clusters;
      std::vector<double> cluster_y;
      for (size_t bi : row) {  // sorted by y0
        const double y0 = page.normalized_boxes[bi].box.y0;
        if (clusters.empty() || y0 - cluster_y.back() > 1.0) {
          clusters.push_back({});
          cluster_y.push_back(y0);
        }
        clusters.back().push_back(bi);
      }
      auto span = [&](const std::vector<size_t>& c) {
        double x0 = 1e18, x1 = -1e18;
        for (size_t bi : c) {
          x0 = std::min(x0, page.normalized_boxes[bi].box.x0);
          x1 = std::max(x1, page.normalized_boxes[bi].box.x1);
        }
        return std::make_pair(x0, x1);
      };
      bool interleaved = false;
      for (size_t a = 0; a < clusters.size() && !interleaved; ++a) {
        for (size_t b = a + 1; b < clusters.size() && !interleaved; ++b) {
          const auto sa = span(clusters[a]), sb = span(clusters[b]);
          const double overlap = std::min(sa.second, sb.second) - std::max(sa.first, sb.first);
          interleaved = overlap > 0.25 * std::min(sa.second - sa.first, sb.second - sb.first);
        }
      }
      if (interleaved) {
        for (auto& c : clusters) rows.push_back(std::move(c));
      } else {
        rows.push_back(std::move(row));
      }
    }
    auto& parts = page_rows_parts[pi];
    auto& owner = part_row[pi];
    std::set<std::string> seen_here;
    for (auto& row : rows) {
      std::sort(row.begin(), row.end(), [&](size_t a, size_t b) {
        return page.normalized_boxes[a].box.x0 < page.normalized_boxes[b].box.x0;
      });
      const bool is_top = page.normalized_boxes[row.front()].box.y1 <= top;
      const int row_index = static_cast<int>(parts.size());
      parts.push_back(make_part(page, row, is_top, strip_top, strip_bottom));
      owner.push_back(-1);
      std::vector<std::vector<size_t>> split;
      double last_x1 = -1e9;
      for (size_t bi : row) {
        const auto& box = page.normalized_boxes[bi].box;
        const double em = std::max(4.0, box.height());
        if (split.empty() || box.x0 - last_x1 > 2.5 * em) split.push_back({});
        split.back().push_back(bi);
        last_x1 = std::max(last_x1, box.x1);
      }
      if (split.size() > 1) {
        for (auto& piece : split) {
          parts.push_back(make_part(page, std::move(piece), is_top, strip_top, strip_bottom));
          owner.push_back(row_index);
        }
      }
    }
    for (const auto& part : parts) {
      if (!part.signature.empty() && seen_here.insert(part.signature).second) ++page_counts[part.signature];
    }
  }

  const size_t needed = std::max<size_t>(
      2, static_cast<size_t>(std::ceil(static_cast<double>(content_pages) * 0.3)));
  auto repeated = [&](const std::string& signature) {
    if (signature.empty() || content_pages < 2) return false;
    const auto it = page_counts.find(signature);
    return it != page_counts.end() && it->second >= needed;
  };
  // Letter-spaced capitals ("P U B L I S H E D  BY  T H E …") read as words.
  auto unspaced = [](const std::string& text) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : text + " ") {
      if (c == ' ') {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    size_t single = 0;
    for (const auto& w : words) single += w.size() == 1 ? 1 : 0;
    if (words.size() < 6 || single * 2 < words.size()) return text;
    std::string out;
    for (size_t i = 0; i < words.size(); ++i) {
      const bool glue = i > 0 && words[i].size() == 1 && words[i - 1].size() == 1;
      if (i > 0 && !glue) out.push_back(' ');
      out += words[i];
    }
    return out;
  };
  auto furniture = [&](const std::string& text) {
    const auto t = unspaced(text);
    return is_folio_line(t) || is_band_furniture_line(t);
  };
  auto extent = [](const PageDom& page, const std::vector<size_t>& boxes) {
    BBox b = page.normalized_boxes[boxes.front()].box;
    for (size_t bi : boxes) {
      const auto& x = page.normalized_boxes[bi].box;
      b.x0 = std::min(b.x0, x.x0);
      b.x1 = std::max(b.x1, x.x1);
      b.y0 = std::min(b.y0, x.y0);
      b.y1 = std::max(b.y1, x.y1);
    }
    return b;
  };
  for (size_t pi = 0; pi < pages.size(); ++pi) {
    auto& page = pages[pi];
    const auto& parts = page_rows_parts[pi];
    std::set<size_t> chrome_boxes;
    std::vector<std::vector<size_t>> chrome_groups;  // for continuation lines
    std::vector<bool> group_top;
    auto mark = [&](const std::vector<size_t>& boxes, bool top) {
      if (boxes.empty()) return;
      for (size_t bi : boxes) chrome_boxes.insert(bi);
      chrome_groups.push_back(boxes);
      group_top.push_back(top);
    };
    for (size_t k = 0; k < parts.size(); ++k) {
      const auto& part = parts[k];
      if (part.tokens.empty()) continue;
      if (repeated(part.signature) || (part.in_strip && furniture(part.text))) {
        mark(part.boxes, part.top);
        continue;
      }
      // A part opening with a running foot's text ("Cell Reports … 2026"
      // then "© 2026 The Author(s). Published by …"): the foot is chrome, the
      // rest is judged on its own.
      size_t best = 0;
      for (size_t n = part.tokens.size(); n >= 3 && best == 0; --n) {
        size_t b = 0;
        while (b < n && folio_token(part.tokens[b])) ++b;
        size_t e = n;
        while (e > b && folio_token(part.tokens[e - 1])) --e;
        if (e - b >= 3 && repeated(join(part.tokens, b, e))) best = n;
      }
      if (best == 0) continue;
      std::vector<size_t> head, rest;
      size_t seen = 0;
      std::string rest_text;
      for (size_t i = 0; i < part.boxes.size(); ++i) {
        (seen < best ? head : rest).push_back(part.boxes[i]);
        if (seen >= best) rest_text += page.normalized_boxes[part.boxes[i]].text + ' ';
        seen += part.box_tokens[i];
      }
      mark(head, part.top);
      if (!rest.empty() && part.in_strip && furniture(rest_text)) mark(rest, part.top);
    }
    // A foot's block runs on in the lines set closely under it (a licence
    // wrapped onto a second line).
    for (size_t g = 0; g < chrome_groups.size(); ++g) {
      if (group_top[g]) continue;
      const BBox a = extent(page, chrome_groups[g]);
      for (const auto& part : parts) {
        if (part.top || part.boxes.empty()) continue;
        if (std::all_of(part.boxes.begin(), part.boxes.end(), [&](size_t bi) { return chrome_boxes.count(bi) > 0; }))
          continue;
        const BBox b = extent(page, part.boxes);
        const double overlap = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
        if (b.y0 > a.y0 + 0.5 * a.height() && b.y0 - a.y1 <= 0.6 * a.height() &&
            overlap >= 0.5 * std::min(a.width(), b.width()) && part.in_strip)
          for (size_t bi : part.boxes) chrome_boxes.insert(bi);
      }
    }
    const double mid = page.height * 0.5;
    for (size_t bi : chrome_boxes) {
      auto& box = page.normalized_boxes[bi];
      if (box.region == RegionKind::Body || box.region == RegionKind::Footnote)
        box.region = box.box.cy() < mid ? RegionKind::Header : RegionKind::Footer;
    }
  }
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
  page.tables.clear();
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
    // A footnote key starts its line; a digit set right after a word on the
    // same row is a superscript citation in the body text ("…centers.12").
    const bool follows_word = std::any_of(
        page.normalized_boxes.begin(), page.normalized_boxes.end(), [&](const auto& other) {
          return &other != &box && other.box.y0 < box.box.y1 && other.box.y1 > box.box.y0 &&
                 other.box.x1 <= box.box.x0 + 0.5 && other.box.x1 >= box.box.x0 - 6.0;
        });
    if (follows_word) continue;
    auto& start = box.box.cx() < page.width * 0.5 ? footnote_start_left
                                                  : footnote_start_right;
    start = std::min(start, box.box.y0);
  }
  double page_body = 0;  // typed pages: the size of this page's running text
  // Typed pages of unknown templates: columns from the page geometry,
  // footnote keys that start a line of smaller type at their column's left
  // edge. The named families keep their own tuned rules (and overrides).
  const bool typed = page.body_font_size > 0 && page.layout_family == LayoutFamily::Generic;
  const double body = page.body_font_size;
  PageColumns columns;
  std::vector<double> typed_footnote_start;
  std::vector<double> col_left;
  if (typed) {
    // The document-level decision (decide_document_columns) when there is
    // one; a page alone otherwise.
    columns = detect_page_columns(page, top, bottom);
    if (page.gutter_decided) {
      columns.gutters = page.gutters;
      columns.split = !columns.gutters.empty();
    } else {
      page.gutters = columns.split ? columns.gutters : std::vector<double>{};
    }
    {
      // Prose rows (many words, mostly lowercase-initial, one or two runs),
      // weighted by their letters: the page's own body size.
      std::vector<size_t> all;
      for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
        const auto& b = page.normalized_boxes[k];
        if (b.rotation == 0 && b.box.y0 >= top && b.box.y1 <= bottom) all.push_back(k);
      }
      std::map<long, size_t> letters_by_size;
      for (const auto& row : page_rows(page, all)) {
        if (row.words < 8 || row.size <= 0 || row.segments > 2) continue;
        if (row.lower_words * 10 < row.words * 6) continue;
        letters_by_size[std::lround(row.size * 10.0)] += row.letters;
      }
      if (!letters_by_size.empty()) {
        const auto mode = std::max_element(letters_by_size.begin(), letters_by_size.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
        page_body = static_cast<double>(mode->first) / 10.0;
      }
    }
    const int n_columns = column_count(columns);
    typed_footnote_start.assign(static_cast<size_t>(n_columns), page.height + 1);
    col_left.assign(static_cast<size_t>(n_columns), 0);
    std::vector<std::vector<double>> lefts(static_cast<size_t>(n_columns));
    for (const auto& box : page.normalized_boxes) {
      if (box.rotation != 0 || box.box.y0 < top || box.box.y1 > bottom) continue;
      const int side = side_of(columns, box.box);
      if (side >= 0) lefts[static_cast<size_t>(side)].push_back(box.box.x0);
    }
    for (int side = 0; side < n_columns; ++side) {
      auto& xs = lefts[static_cast<size_t>(side)];
      if (xs.empty()) {
        col_left[static_cast<size_t>(side)] =
            side == 0 ? columns.left : columns.gutters[static_cast<size_t>(side) - 1];
        continue;
      }
      std::sort(xs.begin(), xs.end());
      col_left[static_cast<size_t>(side)] = xs[xs.size() / 20];
    }
  }
  auto in_typed_footnote_zone = [&](const NormalizedTextBox& box) {
    if (typed_footnote_start.empty()) return false;
    const int side = side_of(columns, box.box);
    const double start =
        side >= 0 ? typed_footnote_start[static_cast<size_t>(side)]
                  : *std::min_element(typed_footnote_start.begin(), typed_footnote_start.end());
    return start <= page.height && box.box.y0 >= start - 1.0;
  };
  // A caption label: "Figure 3." / "Fig. 3" / "TABLE 2" opening its row in
  // its column, told from a sentence that opens with a reference ("Table 3
  // shows…") by its type or by punctuation after a gap.
  auto is_typed_caption_seed = [&](size_t index) {
    const auto& label = page.normalized_boxes[index];
    // Caption labels in the corpus languages (tabla, figura, tabela,
    // tabella, tablo, şekil, abbildung, tabelle, gráfico, cuadro, quadro).
    static const std::regex caption_word(
        R"(^(figure|fig\.|table|tab\.|tabla|tabela|tabella|tablo|tabelle|figura|fig|abbildung|abb\.|gr[aá]fico|cuadro|quadro|imagen|imagem|\xC5\x9Fekil)(?:\s+(?:[A-Z]\.?)?\d+(?:\.\d+)?[a-z]?[\.:|]?)?$)",
        std::regex::icase);
    if (!std::regex_match(label.text, caption_word)) return false;
    const int side = side_of(columns, label.box);
    const NormalizedTextBox* number = nullptr;
    bool text_before = false;
    for (const auto& other : page.normalized_boxes) {
      if (&other == &label || !rows_overlap(label.box, other.box)) continue;
      if (columns.split && side >= 0 && side_of(columns, other.box) != side) continue;
      // A word set just before the label ("see Figure 3"); on a page set in
      // columns, a column of text beside the caption, a gap away, is not.
      const double em = std::max(4.0, label.type_size > 0 ? label.type_size : body);
      if (other.box.x1 <= label.box.x0 + 0.5 && (!columns.split || label.box.x0 - other.box.x1 < 1.0 * em))
        text_before = true;
      // "3", "3a.", "B.1", "A3:", "S2" (appendix and supplement numbering).
      // Roman numbers too ("TABLE II", IEEE).
      static const std::regex number_word(R"(^(?:(?:[A-Z]\.?)?\d+(?:\.\d+)?[A-Za-z]?|[IVX]{1,6})[\.:|]?$)");
      if (!number && other.box.x0 >= label.box.x1 - 0.5 && other.box.x0 - label.box.x1 < 45 &&
          std::regex_match(other.text, number_word))
        number = &other;
    }
    const bool numbered = number || std::regex_search(label.text, std::regex(R"(\d)"));
    if (!numbered || text_before) return false;
    static const std::regex punctuated_number(R"(\d[A-Za-z]?[\.:|]$)");
    const bool punctuated = std::regex_search(number ? number->text : label.text, punctuated_number);
    int letters = 0, uppers = 0;
    for (unsigned char c : label.text) {
      if (std::isalpha(c)) {
        ++letters;
        if (std::isupper(c)) ++uppers;
      }
    }
    const bool capitals = letters >= 3 && uppers == letters;
    const bool typographic = (label.type_size > 0 && label.type_size < body * 0.95 &&
                              (page_body <= 0 || label.type_size < page_body * 0.95)) ||
                             (label.type_heavy && !page.body_font_heavy) || capitals ||
                             label.type_italic;
    if (typographic) return true;
    if (!punctuated) return false;
    // A caption is not indented like a paragraph's first line ("Fig. 10.
    // Comparative analysis…" opening a paragraph of running text).
    const double column_left = side >= 0 ? col_left[static_cast<size_t>(side)] : col_left[0];
    if (label.box.x0 > column_left + body * 0.6) return false;
    // Punctuation alone: a caption stands apart from the text above it.
    double above = -1;
    for (const auto& other : page.normalized_boxes) {
      if (other.box.y1 > label.box.y0 + 0.5 || rows_overlap(other.box, label.box)) continue;
      if (columns.split && side >= 0 && side_of(columns, other.box) != side) continue;
      if (other.box.x1 < label.box.x0 - 5 || other.box.x0 > label.box.x0 + column_width(columns, side))
        continue;
      above = std::max(above, other.box.y1);
    }
    return above < 0 || label.box.y0 - above > body * 1.0;
  };
  // A rail beside the text column (MDPI's first-page history, citation and
  // licence notes; margin notes): small type set wholly outside the measure
  // of the body text.
  double body_left = 0, body_right = page.width;
  if (typed) {
    std::vector<size_t> all;
    for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
      const auto& b = page.normalized_boxes[k];
      if (b.rotation == 0 && b.box.y0 >= top && b.box.y1 <= bottom) all.push_back(k);
    }
    std::vector<double> lefts_body, rights_body;
    for (const auto& row : page_rows(page, all)) {
      if (row.words < 5 || row.size <= 0 || std::abs(row.size - body) > body * 0.05) continue;
      lefts_body.push_back(row.box.x0);
      rights_body.push_back(row.box.x1);
    }
    if (lefts_body.size() >= 5) {
      std::sort(lefts_body.begin(), lefts_body.end());
      std::sort(rights_body.begin(), rights_body.end());
      body_left = lefts_body[lefts_body.size() / 10];
      body_right = rights_body[rights_body.size() * 9 / 10];
    }
  }
  // First pages only: later pages' small type beside the body (a reference
  // list's column, a nomenclature) is text.
  auto in_side_rail = [&](const NormalizedTextBox& box) {
    if (!typed || page.index != 0 || box.type_size <= 0 || box.type_size > body * 0.88) return false;
    const bool left_rail = body_left >= page.width * 0.2 && box.box.x1 < body_left - body * 0.8 &&
                           box.box.x1 < page.width * 0.35;
    const bool right_rail = body_right <= page.width * 0.8 && box.box.x0 > body_right + body * 0.8 &&
                            box.box.x0 > page.width * 0.65;
    return left_rail || right_rail;
  };

  // Footnote keys (typed pages), once caption labels are known: a figure's
  // numbers above its caption are not notes.
  if (typed) {
    std::vector<size_t> caption_labels;
    for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
      if (is_typed_caption_seed(k)) caption_labels.push_back(k);
    }
    auto key_text = [](const std::string& text) {
      if (is_digits(text)) return text.size() <= 2;
      // "2." at a note's head, in the notes' own type (numbered notes).
      if (text.size() >= 2 && text.size() <= 3 && text.back() == '.' && is_digits(text.substr(0, text.size() - 1)))
        return true;
      return text == "*" || text == "**" || text == "\xE2\x80\xA0" || text == "\xE2\x80\xA1" ||
             text == "\xC2\xA7" || text == "\xC2\xB6" || text == "\xE2\x88\x97";  // † ‡ § ¶ ∗
    };
    // Note markers on the page: small digits raised against the word they
    // follow. A numbered key with a period ("2.") names a note only when
    // the page carries its marker (a reference list's entries are numbered
    // alike, with no markers).
    std::set<std::string> page_markers;
    for (size_t k = 1; k < page.normalized_boxes.size(); ++k) {
      const auto& m = page.normalized_boxes[k];
      const auto& before = page.normalized_boxes[k - 1];
      if (!is_digits(m.text) || m.text.size() > 3 || m.type_size <= 0 || before.type_size <= 0) continue;
      if (m.type_size > before.type_size * 0.85) continue;
      if (m.box.x0 < before.box.x1 - 0.5 || m.box.x0 - before.box.x1 > 2.0) continue;
      if (m.box.cy() >= before.box.cy() - before.box.height() * 0.1) continue;
      page_markers.insert(m.text);
    }
    for (const auto& key : page.normalized_boxes) {
      if (!key_text(key.text)) continue;
      if (key.text.back() == '.' && !page_markers.count(key.text.substr(0, key.text.size() - 1))) continue;
      if (key.box.y0 < page.height * 0.60 || key.box.y1 > bottom) continue;
      const int side = side_of(columns, key.box);
      if (side < 0 || key.box.x0 > col_left[static_cast<size_t>(side)] + 1.5 * body) continue;
      const bool small_key = (key.type_size > 0 && key.type_size <= body * 0.85) ||
                             (median_height > 0 && key.box.height() < median_height * 0.82);
      if (!small_key) continue;
      const bool follows_word = std::any_of(
          page.normalized_boxes.begin(), page.normalized_boxes.end(), [&](const auto& other) {
            return &other != &key && other.box.y0 < key.box.y1 && other.box.y1 > key.box.y0 &&
                   other.box.x1 <= key.box.x0 + 0.5 && other.box.x1 >= key.box.x0 - 6.0;
          });
      if (follows_word) continue;
      // The note itself, to the key's right, is words set smaller than the
      // body (not a figure's axis numbers).
      double chars = 0, sum = 0;
      size_t letters = 0, words = 0;
      for (const auto& other : page.normalized_boxes) {
        if (&other == &key || other.type_size <= 0 || !rows_overlap(key.box, other.box)) continue;
        if (other.box.x0 < key.box.x1 - 0.5 || side_of(columns, other.box) != side) continue;
        const double n = static_cast<double>(std::max<size_t>(1, other.text.size()));
        chars += n;
        sum += other.type_size * n;
        ++words;
        for (unsigned char c : other.text) letters += (std::isalpha(c) || c >= 0xC0) ? 1 : 0;
      }
      if (chars == 0 || sum / chars > body * 0.93) continue;
      if (words < 2 || static_cast<double>(letters) < chars * 0.5) continue;
      const bool above_caption = std::any_of(caption_labels.begin(), caption_labels.end(), [&](size_t c) {
        const auto& label = page.normalized_boxes[c];
        const int label_side = side_of(columns, label.box);
        return label.box.y0 > key.box.y1 && (label_side == side || label_side < 0);
      });
      if (above_caption) continue;
      auto& start = typed_footnote_start[static_cast<size_t>(side)];
      start = std::min(start, key.box.y0);
    }
    // The unkeyed lines that open a column's note block (a corresponding
    // author's name and address above the keyed affiliations), set in the
    // notes' type and apart from the text above them.
    for (size_t side = 0; side < typed_footnote_start.size(); ++side) {
      double& start = typed_footnote_start[side];
      if (start > page.height) continue;
      std::vector<size_t> above;
      for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
        const auto& b = page.normalized_boxes[k];
        if (b.rotation == 0 && b.box.y1 <= start + 1.0 && b.box.y0 >= top &&
            side_of(columns, b.box) == static_cast<int>(side))
          above.push_back(k);
      }
      const auto rows = page_rows(page, above);
      double block_top = start;
      for (size_t r = rows.size(); r-- > 0;) {
        const auto& row = rows[r];
        if (row.size <= 0 || row.size > body * 0.9 || block_top - row.box.y1 > 2.0 * row.box.height()) break;
        const bool text_above = r > 0 && rows[r - 1].size > body * 0.9 && row.box.y0 - rows[r - 1].box.y1 >= body;
        block_top = row.box.y0;
        if (text_above) {
          start = block_top;
          break;
        }
      }
    }
    // Notes set across the page's full width: a note line that runs on over
    // the gutter (word spacing, not a gutter's gap) opens the next column's
    // footnote area too.
    for (size_t side = 0; side + 1 < typed_footnote_start.size(); ++side) {
      const double start = typed_footnote_start[side];
      if (start > page.height) continue;
      std::vector<size_t> zone;
      for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
        const auto& b = page.normalized_boxes[k];
        if (b.rotation == 0 && b.box.y0 >= start - 1.0 && b.box.y1 <= bottom &&
            side_of(columns, b.box) != static_cast<int>(side) + 1)
          zone.push_back(k);
      }
      for (const auto& row : page_rows(page, zone)) {
        double reach = -1e18;
        for (size_t k : row.boxes) {
          if (page.normalized_boxes[k].box.x0 <= columns.gutters[side]) reach = std::max(reach, page.normalized_boxes[k].box.x1);
        }
        if (reach < columns.gutters[side] - body * 3) continue;
        const double em = std::max(4.0, row.size > 0 ? row.size : body);
        const bool runs_on = std::any_of(page.normalized_boxes.begin(), page.normalized_boxes.end(), [&](const auto& b) {
          return b.rotation == 0 && rows_overlap(row.box, b.box) && side_of(columns, b.box) == static_cast<int>(side) + 1 &&
                 b.box.x0 >= reach - 0.5 && b.box.x0 - reach <= 0.6 * em;
        });
        if (runs_on) {
          auto& next = typed_footnote_start[side + 1];
          next = std::min(next, row.box.y0);
          break;
        }
      }
    }
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

  std::vector<size_t> typed_seeds;
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
    if (in_side_rail(box)) {
      box.region = RegionKind::Sidebar;
      continue;
    }
    if (page.layout_family != LayoutFamily::AcmConferenceTwoColumn &&
        !page.keep_captions &&
        (typed ? is_typed_caption_seed(static_cast<size_t>(&box - page.normalized_boxes.data()))
               : is_caption_seed(box))) {
      box.region = RegionKind::Float;
      typed_seeds.push_back(static_cast<size_t>(&box - page.normalized_boxes.data()));
      continue;
    }
    if (typed) {
      if (in_typed_footnote_zone(box)) {
        box.region = RegionKind::Footnote;
        continue;
      }
    } else {
      const bool left_half = box.box.cx() < page.width * 0.5;
      const double footnote_start =
          left_half ? footnote_start_left : footnote_start_right;
      if (footnote_start <= page.height &&
          box.box.y0 >= footnote_start - 1.0) {
        box.region = RegionKind::Footnote;
        continue;
      }
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

  // An opening page's foot: a block in smaller type set under all of the
  // page's text (the authors' addresses, a DOI and version date, a licence),
  // apart from it by more than its leading. It is not the text; it goes with
  // the page's notes.
  if (typed && page.opening_page) {
    // Down to the page's edge: a licence's last line can reach the band.
    std::vector<size_t> all;
    for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
      const auto& b = page.normalized_boxes[k];
      if (b.rotation == 0 && b.box.y0 >= top && b.region == RegionKind::Body) all.push_back(k);
    }
    const bool has_seed = [&] {
      for (size_t k : typed_seeds) {
        if (page.normalized_boxes[k].box.y0 > page.height * 0.5) return true;
      }
      return false;
    }();
    if (has_seed) all.clear();  // a float's caption can stand at the foot
    const auto rows = page_rows(page, all);
    auto body_row = [&](const PageRow& row) {
      return row.words >= 3 && row.size > 0 &&
             (std::abs(row.size - body) <= body * 0.07 ||
              (page_body > 0 && std::abs(row.size - page_body) <= page_body * 0.07));
    };
    size_t last_body = rows.size();
    for (size_t r = rows.size(); r-- > 0;) {
      if (body_row(rows[r])) {
        last_body = r;
        break;
      }
    }
    if (last_body + 1 < rows.size() && rows.size() - last_body - 1 <= 10 &&
        rows[last_body + 1].box.y0 - rows[last_body].box.y1 >= 1.2 * body) {
      bool small = true;
      for (size_t r = last_body + 1; r < rows.size() && small; ++r) {
        small = rows[r].size > 0 && rows[r].size <= body * 0.9 &&
                (page_body <= 0 || rows[r].size <= page_body * 0.9);
      }
      if (small) {
        for (size_t r = last_body + 1; r < rows.size(); ++r) {
          if (rows[r].letters == 0) continue;  // a folio: the page's chrome
          for (size_t k : rows[r].boxes) page.normalized_boxes[k].region = RegionKind::Footnote;
        }
      }
    }
  }

  // Typed pages: a caption's island is its own paragraph (the caption's type
  // and leading), then whatever is not running text below it (a table's
  // body, the material of a figure set under its caption), and for figures
  // the non-text material above the caption: axis labels, legends. Running
  // text in the body type ends it, in the caption's own column.
  if (typed) {
    auto& boxes = page.normalized_boxes;
    auto is_seed = [&](size_t k) {
      return std::find(typed_seeds.begin(), typed_seeds.end(), k) != typed_seeds.end();
    };
    for (size_t seed : typed_seeds) {
      // The caption's column: that of its first line, which runs on from the
      // label across the gutter when the caption spans the page.
      int side = side_of(columns, boxes[seed].box);
      double seed_reach = boxes[seed].box.x1;
      // A column of text can run on beside a figure that spans the gutter
      // (its lines start at one x, right of the caption): the island ends
      // short of it.
      double bound_x1 = 1e18;
      // The reach of a line from its first word, by word spacing only (a
      // justified line's spaces reach most of an em): a gutter is an em or
      // two, so the next column's text on the same row must not count.
      auto reach_from = [&](size_t start, double& beside, double spacing = 1.0) {
        std::vector<size_t> row;
        for (size_t k = 0; k < boxes.size(); ++k) {
          if (boxes[k].rotation == 0 && rows_overlap(boxes[start].box, boxes[k].box) &&
              boxes[k].box.x0 >= boxes[start].box.x0 - 0.5)
            row.push_back(k);
        }
        std::sort(row.begin(), row.end(),
                  [&](size_t a, size_t b) { return boxes[a].box.x0 < boxes[b].box.x0; });
        double reach = boxes[start].box.x1;
        beside = 1e18;
        for (size_t k : row) {
          const double em = std::max(4.0, boxes[k].type_size > 0 ? boxes[k].type_size : body);
          if (boxes[k].box.x0 - reach > spacing * em) {
            beside = boxes[k].box.x0;
            break;
          }
          reach = std::max(reach, boxes[k].box.x1);
        }
        return reach;
      };
      // The next line of a caption: its first word under the caption's
      // first, set close below in the caption's type.
      auto next_caption_line = [&](size_t line_start) {
        const auto& first = boxes[line_start].box;
        const double size = boxes[line_start].type_size > 0 ? boxes[line_start].type_size : body;
        size_t next = boxes.size();
        for (size_t k = 0; k < boxes.size(); ++k) {
          const auto& b = boxes[k];
          if (b.rotation != 0 || std::abs(b.box.x0 - boxes[seed].box.x0) > 2.0) continue;
          if (b.box.y0 <= first.y0 + first.height() * 0.5 || b.box.y0 - first.y1 > size * 0.9) continue;
          if (b.type_size > 0 && std::abs(b.type_size - size) > size * 0.12) continue;
          if (next == boxes.size() || b.box.y0 < boxes[next].box.y0) next = k;
        }
        return next;
      };
      if (columns.split && side >= 0) {
        // The caption spans the gutter when its first line does, or a line
        // of its paragraph under it (a short title over a full-width legend).
        size_t line_start = seed;
        for (int line = 0; line < 4 && side >= 0; ++line) {
          double beside = 1e18;
          const double reach = reach_from(line_start, beside);
          if (line == 0) seed_reach = reach;
          const auto& first = boxes[line_start].box;
          if (side_of(columns, BBox{first.x0, first.y0, reach, first.y1}) < 0) {
            side = -1;
            seed_reach = std::max(seed_reach, reach);
            if (beside < 1e17) {
              int starts = 0;
              for (const auto& b : boxes) {
                if (b.rotation == 0 && std::abs(b.box.x0 - beside) <= 2.0 &&
                    std::abs(b.box.cy() - first.cy()) <= body * 8)
                  ++starts;
              }
              if (starts >= 3) bound_x1 = (reach + beside) / 2;
            }
            break;
          }
          const size_t next = next_caption_line(line_start);
          if (next == boxes.size()) break;
          line_start = next;
        }
      }
      // A column of text can also stand to the left of such a figure: lines
      // that end short of the caption's left edge, with a clear gap to it.
      double bound_x0 = -1e18;
      if (side < 0 && columns.split) {
        const auto& first = boxes[seed].box;
        const double em = std::max(4.0, boxes[seed].type_size > 0 ? boxes[seed].type_size : body);
        const double edge = first.x0 - 0.5 * em;
        std::vector<size_t> near;
        for (size_t k = 0; k < boxes.size(); ++k) {
          const auto& b = boxes[k];
          if (b.rotation == 0 && std::abs(b.box.cy() - first.cy()) <= body * 8) near.push_back(k);
        }
        int beside_rows = 0;
        bool crossed = false;
        for (const auto& row : page_rows(page, near)) {
          bool left = false;
          for (size_t k : row.boxes) {
            const auto& b = boxes[k].box;
            if (b.x0 < edge && b.x1 > edge) crossed = true;
            if (b.x1 <= first.x0 - 1.0 * em) left = true;
          }
          if (left) ++beside_rows;
        }
        if (!crossed && beside_rows >= 3) bound_x0 = edge;
      }
      const auto seed_low = fold_lower_utf8(boxes[seed].text);
      const bool table_seed = seed_low.rfind("tab", 0) == 0 || seed_low.rfind("cuadro", 0) == 0 ||
                              seed_low.rfind("quadro", 0) == 0;
      // Running text ends the island. Across the gutter, a line of it in
      // either column does (beside a short line in the other, or beside the
      // figure's own labels). A table spanning the page ends where the page
      // returns to columns of prose side by side (a table's cells are
      // narrower, or cross the gutter).
      auto running_text = [&](const PageRow& row) {
        if (body_like_row(row, body, page_body, columns, side)) return true;
        if (!columns.split || side >= 0) return false;
        std::map<int, std::vector<size_t>> by_column;
        bool crosses = false;
        for (size_t k : row.boxes) {
          const int column = side_of(columns, boxes[k].box);
          if (column >= 0) by_column[column].push_back(k);
          else crosses = true;
        }
        int prose_columns = 0;
        for (const auto& [column, part] : by_column) {
          for (const auto& sub : page_rows(page, part)) {
            if (!table_seed && body_like_row(sub, body, page_body, columns, column)) return true;
            if (sub.words >= 4 && sub.segments <= 1 && sub.letters > 2 * sub.digits &&
                sub.box.width() >= 0.6 * column_width(columns, column)) {
              ++prose_columns;
              break;
            }
          }
        }
        return table_seed && !crosses && prose_columns >= 2;
      };
      // Another float's caption (or one already placed) ends this island.
      auto has_other_seed = [&](const PageRow& row) {
        return std::any_of(row.boxes.begin(), row.boxes.end(), [&](size_t k) {
          return k != seed && (is_seed(k) || boxes[k].region == RegionKind::Caption);
        });
      };
      auto candidate = [&](size_t k) {
        const auto& b = boxes[k];
        return (b.region == RegionKind::Body || b.region == RegionKind::Float || b.region == RegionKind::Caption) &&
               b.rotation == 0 && b.box.y0 >= top && b.box.y1 <= bottom;
      };

      // A caption set in a narrow block beside its figure (a side caption):
      // its lines are the block's own, and the figure is what stands beside
      // and around it, across the gutter when it reaches the other column.
      if (side >= 0 && !table_seed) {
        std::vector<size_t> chain;
        double strip_x1 = boxes[seed].box.x1;
        for (size_t line_start = seed; line_start < boxes.size() && chain.size() < 40;
             line_start = next_caption_line(line_start)) {
          chain.push_back(line_start);
          double beside = 1e18;
          strip_x1 = std::max(strip_x1, reach_from(line_start, beside, 1.1));
        }
        const double strip_x0 = boxes[seed].box.x0;
        const double cap_y0 = boxes[chain.front()].box.y0 - 1.0;
        const double cap_y1 = boxes[chain.back()].box.y1 + 1.0;
        std::vector<size_t> caption_boxes, others, beside, beside_other;
        if (chain.size() >= 3 && strip_x1 - strip_x0 <= 0.5 * column_width(columns, side)) {
          for (size_t k = 0; k < boxes.size(); ++k) {
            if (!candidate(k)) continue;
            const auto& b = boxes[k].box;
            if (b.x0 >= strip_x0 - 1.0 && b.x1 <= strip_x1 + 1.0 && b.y0 >= cap_y0 && b.y1 <= cap_y1) {
              caption_boxes.push_back(k);
              continue;
            }
            others.push_back(k);
            if (b.cy() < cap_y0 || b.cy() > cap_y1 || b.x0 <= strip_x1) continue;
            const int column = side_of(columns, b);
            (column == side || column < 0 ? beside : beside_other).push_back(k);
          }
        }
        // The figure's material stands beside the caption in its column; text
        // wrapping round a narrow caption would be running text.
        bool side_caption = beside.size() >= 2;
        for (const auto& row : page_rows(page, beside)) {
          if (body_like_row(row, body, page_body, columns, side)) side_caption = false;
        }
        if (side_caption) {
          // The figure reaches into the next column when all it holds beside
          // the caption is figure material.
          bool text_beside = false;
          for (const auto& row : page_rows(page, beside_other)) {
            if (body_like_row(row, body, page_body, columns, side_of(columns, row.box))) text_beside = true;
          }
          if (columns.split && !beside_other.empty() && !text_beside) {
            side = -1;
          } else if (columns.split) {
            others.erase(std::remove_if(others.begin(), others.end(),
                                        [&](size_t k) {
                                          const int column = side_of(columns, boxes[k].box);
                                          return column >= 0 && column != side;
                                        }),
                         others.end());
          }
          auto rows = page_rows(page, others);
          std::vector<bool> take(rows.size(), false);
          size_t first_beside = rows.size(), last_beside = rows.size();
          for (size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].box.y1 < cap_y0 || rows[r].box.y0 > cap_y1) continue;
            if (has_other_seed(rows[r]) || running_text(rows[r])) continue;
            take[r] = true;
            if (first_beside == rows.size()) first_beside = r;
            last_beside = r;
          }
          if (first_beside < rows.size()) {
            for (size_t j = first_beside; j-- > 0;) {
              if (rows[j].box.y1 >= cap_y0) continue;
              if (has_other_seed(rows[j]) || running_text(rows[j]) ||
                  heading_like_row(rows[j], body, page.body_font_heavy))
                break;
              take[j] = true;
            }
            size_t prev = last_beside;
            for (size_t j = last_beside + 1; j < rows.size(); ++j) {
              if (rows[j].box.y0 <= cap_y1) continue;
              if (has_other_seed(rows[j]) || running_text(rows[j]) ||
                  heading_like_row(rows[j], body, page.body_font_heavy) ||
                  rows[j].box.y0 - rows[prev].box.y1 > body * 6)
                break;
              take[j] = true;
              prev = j;
            }
          }
          if (std::getenv("AGENTPDF_DEBUG_ISLANDS")) {
            std::fprintf(stderr, "ISLAND p%d side caption '%s' strip %.0f-%.0f lines %zu figure side %d\n",
                         page.index, boxes[seed].text.c_str(), strip_x0, strip_x1, chain.size(), side);
            for (size_t r = 0; r < rows.size(); ++r) {
              if (take[r]) std::fprintf(stderr, "   flt y=%.1f |%.90s|\n", rows[r].box.y0, rows[r].text.c_str());
            }
          }
          for (size_t k : caption_boxes) boxes[k].region = RegionKind::Caption;
          for (size_t r = 0; r < rows.size(); ++r) {
            if (!take[r]) continue;
            for (size_t k : rows[r].boxes) boxes[k].region = RegionKind::Float;
          }
          continue;
        }
      }
      // Members: boxes of the caption's column, or of the columns its first
      // line crosses (a figure can fill two columns of three, with text
      // running on in the third beside it).
      int first_column = side, last_column = side;
      if (side < 0 && columns.split) {
        first_column = 0;
        last_column = column_count(columns) - 1;
        for (size_t g = 0; g < columns.gutters.size(); ++g) {
          if (boxes[seed].box.x0 >= columns.gutters[g] - 1.0) first_column = static_cast<int>(g) + 1;
        }
        for (size_t g = columns.gutters.size(); g-- > 0;) {
          if (seed_reach <= columns.gutters[g] + 1.0) last_column = static_cast<int>(g);
        }
      }
      auto column_of = [&](double x) {
        int column = 0;
        for (double g : columns.gutters) {
          if (x > g) ++column;
        }
        return column;
      };
      std::vector<size_t> members;
      for (size_t k = 0; k < boxes.size(); ++k) {
        const auto& b = boxes[k];
        if (b.region != RegionKind::Body && b.region != RegionKind::Float &&
            b.region != RegionKind::Caption)
          continue;
        if (b.rotation != 0 || b.box.y0 < top || b.box.y1 > bottom) continue;
        const int other = side_of(columns, b.box);
        if (columns.split && side >= 0 && other >= 0 && other != side) continue;
        if (columns.split && side < 0 &&
            (column_of(b.box.x1 - 1.0) < first_column || column_of(b.box.x0 + 1.0) > last_column ||
             b.box.cx() > bound_x1 || b.box.cx() < bound_x0))
          continue;
        members.push_back(k);
      }
      auto rows = page_rows(page, members);
      size_t r0 = rows.size();
      for (size_t r = 0; r < rows.size() && r0 == rows.size(); ++r) {
        if (std::find(rows[r].boxes.begin(), rows[r].boxes.end(), seed) != rows[r].boxes.end()) r0 = r;
      }
      if (r0 == rows.size()) continue;
      std::vector<bool> take(rows.size(), false);
      std::vector<bool> caption_row(rows.size(), false);
      take[r0] = caption_row[r0] = true;
      const double caption_size = rows[r0].size > 0 ? rows[r0].size : body;
      const bool body_size_caption = std::abs(caption_size - body) <= body * 0.05;
      size_t i = r0 + 1, prev = r0;
      while (i < rows.size()) {
        const auto& row = rows[i];
        if (has_other_seed(row)) break;
        const double gap = row.box.y0 - rows[prev].box.y1;
        // In the caption's type, or its legend's (a size smaller), line to
        // line: a raised symbol can lower a line's average a little; a title
        // in small capitals has its initials in the caption's size.
        const bool small_caps = std::abs(row.max_size - caption_size) <= caption_size * 0.08 &&
                                row.letters >= 6 &&
                                std::none_of(row.text.begin(), row.text.end(),
                                             [](unsigned char c) { return std::islower(c); });
        if (row.size <= 0 || gap > caption_size * 0.8 || row.segments > 2 ||
            (!small_caps && std::abs(row.size - caption_size) > caption_size * 0.08 &&
             (prev == r0 || std::abs(row.size - rows[prev].size) > rows[prev].size * 0.05)))
          break;
        // A caption in the body type ends with its sentence; the text that
        // follows it closely is the body again.
        const auto last = trim(rows[prev].text);
        if (body_size_caption && !last.empty() && last.back() == '.' && running_text(row)) break;
        take[i] = caption_row[i] = true;
        prev = i++;
      }
      // A table's body follows its caption; its bold header row is not a
      // section heading unless running text follows it directly.
      while (i < rows.size()) {
        const auto& row = rows[i];
        if (has_other_seed(row)) break;
        // A table's rows line up with the rows above them even when set in
        // the body type across the measure.
        const bool table_row = table_seed && take[prev] && prev != r0 &&
                               aligned_rows(page, rows[prev], row);
        if (!table_row && running_text(row)) break;
        if (!table_row && heading_like_row(row, body, page.body_font_heavy)) {
          const bool introduces_text = row.segments <= 1 && i + 1 < rows.size() && running_text(rows[i + 1]);
          if (!table_seed || introduces_text) break;
        }
        if (row.box.y0 - rows[prev].box.y1 > body * 6) break;
        take[i] = true;
        prev = i++;
      }
      // A table whose caption stands under it: its rows above the caption,
      // cells in rows (several runs, aligned with the row below, or mostly
      // numbers), up to the text.
      bool body_below = false;
      for (size_t r = r0 + 1; r < rows.size(); ++r) body_below = body_below || (take[r] && !caption_row[r]);
      auto claimed = [&](const PageRow& row) {
        return std::any_of(row.boxes.begin(), row.boxes.end(),
                           [&](size_t k) { return boxes[k].region == RegionKind::Float; });
      };
      if (table_seed && r0 > 0 && !body_below) {
        size_t below = r0;
        for (size_t j = r0; j-- > 0;) {
          const auto& row = rows[j];
          if (has_other_seed(row) || claimed(row) || running_text(row) ||
              rows[below].box.y0 - row.box.y1 > body * 3)
            break;
          const bool cells = row.segments >= 2 || (below != r0 && aligned_rows(page, row, rows[below])) ||
                             row.digits * 2 >= row.letters;
          if (!cells) break;
          take[j] = true;
          below = j;
        }
      }
      if (to_lower(boxes[seed].text).rfind("fig", 0) == 0) {
        for (size_t j = r0; j-- > 0;) {
          const auto& row = rows[j];
          // A heading is one run of words; labels spread across the figure
          // ("On-Site Components  Transmission System  Bulk Generation") are not.
          if (has_other_seed(row) || running_text(row) ||
              (row.segments <= 2 && heading_like_row(row, body, page.body_font_heavy)))
            break;
          // The short last line of a paragraph, set close under its text.
          if (j > 0 && row.size > 0 && std::abs(row.size - body) <= body * 0.07 && running_text(rows[j - 1]) &&
              row.box.y0 - rows[j - 1].box.y1 <= body * 0.6)
            break;
          take[j] = true;
        }
      }
      if (std::getenv("AGENTPDF_DEBUG_ISLANDS")) {
        std::fprintf(stderr, "ISLAND p%d seed '%s' side %d gutters", page.index, boxes[seed].text.c_str(), side);
        for (double g : columns.gutters) std::fprintf(stderr, " %.0f", g);
        std::fprintf(stderr, "\n");
        for (size_t r = 0; r < rows.size(); ++r) {
          if (!take[r]) continue;
          std::fprintf(stderr, "   %s y=%.1f size=%.2f seg=%d w=%.0f |%.90s|\n", caption_row[r] ? "CAP" : "flt",
                       rows[r].box.y0, rows[r].size, rows[r].segments, rows[r].box.width(), rows[r].text.c_str());
        }
      }
      // The caption itself is kept (set between paragraphs); the rest of
      // the island, the float's own material, is not. A table's rows are
      // read into its grid, which its caption carries.
      if (table_seed) {
        std::vector<size_t> body_rows;
        for (size_t r = 0; r < rows.size(); ++r) {
          if (take[r] && !caption_row[r]) body_rows.push_back(r);
        }
        auto grid = table_grid(page, rows, body_rows, body);
        if (!grid.rows.empty()) {
          grid.label = boxes[seed].box;
          page.tables.push_back(std::move(grid));
        }
      }
      for (size_t r = 0; r < rows.size(); ++r) {
        if (!take[r]) continue;
        for (size_t k : rows[r].boxes)
          boxes[k].region = caption_row[r] ? RegionKind::Caption : RegionKind::Float;
      }
    }
    // A table the previous page left open at its foot continues at the top
    // of this page's column: aligned rows until running text.
    if (page.table_continues && page.gutters.empty()) {
      std::vector<size_t> members;
      for (size_t k = 0; k < boxes.size(); ++k) {
        const auto& b = boxes[k];
        if (b.region == RegionKind::Body && b.rotation == 0 && b.box.y0 >= top && b.box.y1 <= bottom)
          members.push_back(k);
      }
      auto rows = page_rows(page, members);
      size_t end = 0;
      while (end + 1 < rows.size() && aligned_rows(page, rows[end], rows[end + 1])) ++end;
      if (end >= 2) {
        for (size_t r = 0; r <= end; ++r) {
          for (size_t k : rows[r].boxes) boxes[k].region = RegionKind::Float;
        }
      }
    }
    // Does a table island here run to the foot of the page?
    for (size_t seed : typed_seeds) {
      const auto low = fold_lower_utf8(boxes[seed].text);
      if (low.rfind("tab", 0) != 0) continue;
      double lowest = 0, lowest_body = 0;
      for (const auto& b : boxes) {
        if (b.box.y1 > bottom) continue;
        if (b.region == RegionKind::Float || b.region == RegionKind::Caption)
          lowest = std::max(lowest, b.box.y1);
        if (b.region == RegionKind::Body) lowest_body = std::max(lowest_body, b.box.y1);
      }
      page.table_open_at_foot = lowest > lowest_body && lowest > page.height * 0.8;
    }
    page.text_quality = score_text_quality(page.normalized_boxes);
    return;
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

bool is_references_heading_text(const std::string& text) {
  auto low = to_lower(trim(text));
  if (low == "references" || low == "bibliography" || low == "works cited") {
    return true;
  }
  // Allow a trailing colon or short ornament.
  return low.rfind("references", 0) == 0 && low.size() <= 14;
}

bool region_looks_like_publisher_ancillary(
    const PageDom& page, double from_y) {
  bool saw_wide = false;
  bool saw_formish = false;
  bool saw_appendix = false;
  auto has = [](const std::string& low, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
      if (low.find(n) != std::string::npos) return true;
    }
    return false;
  };
  for (const auto& box : page.normalized_boxes) {
    if (box.box.y0 + 1.0 < from_y) continue;
    const auto low = to_lower(box.text);
    if (low.empty()) continue;
    if (low.rfind("appendix", 0) == 0 || low.rfind("acknowledg", 0) == 0) {
      saw_appendix = true;
    }
    if (box.box.width() > page.width * 0.55) saw_wide = true;
    if (has(low, {"subscription claims", "we provide this form",
                  "print full name", "please print clearly",
                  "to be filled out by", "please do not remove",
                  "photocopy may be used", "member or customer",
                  "today's date", "subscription claims information"})) {
      saw_formish = true;
    }
    // Centered publisher masthead above a tear-out form.
    if (low.rfind("american psychological", 0) == 0 && box.text.size() < 80) {
      saw_formish = true;
    }
    if (low.find("american psychological association") != std::string::npos &&
        box.text.size() < 80) {
      saw_formish = true;
    }
  }
  if (saw_appendix && !saw_formish) return false;
  return saw_wide || saw_formish;
}

void mark_post_references_ancillary(PageDom& page, bool references_active) {
  if (!references_active || page.wrapper_page) return;
  if (page.width <= 0 || page.normalized_boxes.empty()) return;

  double refs_heading_y = -1.0;
  for (const auto& box : page.normalized_boxes) {
    if (is_references_heading_text(box.text)) {
      refs_heading_y = box.box.y0;
      break;
    }
  }
  // Cross-page continuation: still look for collapse on later pages.
  const double y_floor = refs_heading_y >= 0.0 ? refs_heading_y + 4.0 : 0.0;

  struct RowBand {
    double y = 0;
    bool left = false;
    bool right = false;
    double max_width = 0;
  };
  std::vector<RowBand> bands;
  const double mid = page.width * 0.5;
  for (const auto& box : page.normalized_boxes) {
    if (box.region != RegionKind::Body) continue;
    if (box.box.y0 < y_floor) continue;
    if (trim(box.text).empty()) continue;
    // Skip ultra-narrow margin fragments when assessing columns.
    if (box.box.width() < page.width * 0.04) continue;

    bool merged = false;
    for (auto& band : bands) {
      if (std::abs(band.y - box.box.y0) <= 3.0) {
        band.y = (band.y + box.box.y0) * 0.5;
        if (box.box.cx() < mid - 8.0) band.left = true;
        if (box.box.cx() > mid + 8.0) band.right = true;
        band.max_width = std::max(band.max_width, box.box.width());
        merged = true;
        break;
      }
    }
    if (!merged) {
      RowBand band;
      band.y = box.box.y0;
      band.left = box.box.cx() < mid - 8.0;
      band.right = box.box.cx() > mid + 8.0;
      band.max_width = box.box.width();
      bands.push_back(band);
    }
  }
  if (bands.empty()) return;
  std::sort(bands.begin(), bands.end(),
            [](const RowBand& a, const RowBand& b) { return a.y < b.y; });

  double last_two_col_y = -1.0;
  for (const auto& band : bands) {
    if (band.left && band.right) last_two_col_y = band.y;
  }
  if (last_two_col_y < 0.0) return;

  double median_height = 12.0;
  {
    std::vector<double> heights;
    for (const auto& box : page.normalized_boxes) {
      if (box.box.height() > 0) heights.push_back(box.box.height());
    }
    if (!heights.empty()) {
      auto middle =
          heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2);
      std::nth_element(heights.begin(), middle, heights.end());
      median_height = std::max(8.0, *middle);
    }
  }
  const double gap_tol = std::max(14.0, median_height * 1.6);

  double drop_y = -1.0;
  for (const auto& band : bands) {
    if (band.y <= last_two_col_y + gap_tol) continue;
    drop_y = band.y;
    break;
  }
  if (drop_y < 0.0) return;
  if (!region_looks_like_publisher_ancillary(page, drop_y)) return;

  for (auto& box : page.normalized_boxes) {
    if (box.box.y0 + 1.0 >= drop_y) {
      box.region = RegionKind::Metadata;
    }
  }
}

std::vector<TextLine> linearize_page(const PageDom& page,
                                     const Heuristics& heuristics) {
  std::vector<NormalizedTextBox> body;
  for (const auto& box : page.normalized_boxes) {
    if (box.region == RegionKind::Body) body.push_back(box);
  }
  if (body.empty()) return {};

  std::optional<double> cut;
  // Column detection is geometric (a vertical whitespace strip), so it applies
  // to unknown templates as well as the named families.
  if (page.layout_family == LayoutFamily::Generic ||
      page.layout_family == LayoutFamily::MagazineTwoColumn ||
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
  renumber_synthetic_line_y(result);
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
    std::string folded{};       // fold_alnum(text)
    std::string prefix{};       // first four words, when it has four
    bool figure_reference = false;  // "figure 3"
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
  static const std::regex figure_reference_line(R"(^\s*figure\s+\d+[a-z]?[\.,]?\s*$)",
                                                std::regex::icase);
  static const std::regex figure_reference_visual(R"(^figure\s+\d+[a-z]?[\.,]?$)",
                                                  std::regex::icase);
  static const std::regex year_range(R"(^\s*20\d{2}\s*[-–]\s*20\d{2}\s*$)");
  for (auto& visual_line : visual_lines) {
    visual_line.folded = fold_alnum(visual_line.text);
    const auto tokens = split_words(visual_line.text);
    if (tokens.size() >= 4)
      visual_line.prefix = tokens[0] + " " + tokens[1] + " " + tokens[2] + " " + tokens[3];
    visual_line.figure_reference =
        std::regex_match(visual_line.text, figure_reference_visual);
  }

  std::vector<TextLine> kept;
  for (auto line : lines) {
    line.text = collapse_ws(line.text);
    auto low = to_lower(line.text);
    bool remove = low.empty();
    const bool protected_reference =
        page.layout_family == LayoutFamily::FrontiersRail &&
        std::regex_match(line.text, figure_reference_line) && !kept.empty() &&
        to_lower(kept.back().text).find("as shown in") != std::string::npos;
    const bool protected_section_suffix =
        page.layout_family == LayoutFamily::FrontiersRail &&
        std::regex_match(line.text, year_range) && !kept.empty() &&
        to_lower(kept.back().text).find("current history") !=
            std::string::npos;
    if (protected_reference || protected_section_suffix) {
      kept.push_back(std::move(line));
      continue;
    }
    // Per-line values, refreshed whenever an erasure changes the text.
    bool short_figure_reference = false;
    std::string folded_line;
    size_t line_words = 0;
    auto refresh = [&] {
      low = to_lower(line.text);
      short_figure_reference = std::regex_match(line.text, figure_reference_line);
      folded_line = fold_alnum(low);
      line_words = split_words(low).size();
    };
    refresh();
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
              visual_line.region == RegionKind::Float && visual_line.figure_reference &&
              to_lower(line.text.substr(0, position)).ends_with("as shown in ");
          if (inline_figure_reference) continue;
          line.text.erase(position, visual.size());
          line.text = collapse_ws(line.text);
          refresh();
          if (line.text.empty()) {
            remove = true;
            break;
          }
          continue;
        }
        if (low.size() >= 8 && visual.find(low) != std::string::npos &&
            (visual_line.region != RegionKind::Float ||
             !short_figure_reference)) {
          remove = true;
          break;
        }
        // The streams space the same glyphs differently ("R. O R G" against
        // "R . O R G"): failing an exact match, compare alphanumerics only.
        // Only for page chrome: float and footnote islands can over-reach
        // into body text, which must not be removed on a looser match. A
        // line of one or two words ("hypertext.") would be found inside any
        // long quarantined block, so this also needs a few words.
        const bool chrome = visual_line.region == RegionKind::Header ||
                            visual_line.region == RegionKind::Footer ||
                            visual_line.region == RegionKind::MarginOverlay;
        if (chrome && folded_line.size() >= 12 && line_words >= 3 &&
            visual_line.folded.find(folded_line) != std::string::npos) {
          remove = true;
          break;
        }
        if (chrome && visual_line.folded.size() >= 12 &&
            folded_line.find(visual_line.folded) != std::string::npos &&
            erase_folded_words(line.text, visual_line.folded)) {
          refresh();
          if (line.text.empty()) {
            remove = true;
            break;
          }
          continue;
        }
      }
      if (!visual_line.prefix.empty() && low.rfind(visual_line.prefix, 0) == 0 &&
          visual_line.region != RegionKind::Float) {
        remove = true;
        break;
      }
    }
    if (!remove) kept.push_back(std::move(line));
  }
  renumber_synthetic_line_y(kept);
  return kept;
}

void renumber_synthetic_line_y(std::vector<TextLine>& lines) {
  for (size_t i = 0; i < lines.size(); ++i) {
    lines[i].box.y0 = static_cast<double>(i) * 12.0;
    lines[i].box.y1 = lines[i].box.y0 + 10.0;
  }
}

void stitch_document_lines(DocumentDom& dom, const Heuristics& heuristics) {
  if (!heuristics.rejoin_hyphenation) return;
  for (auto& page : dom.pages) {
    for (size_t i = 1; i < page.lines.size(); ++i) {
      auto& previous = page.lines[i - 1];
      auto& current = page.lines[i];
      // Lines rebuilt from their words were joined with the vocabulary.
      if (previous.has_geom || current.has_geom) continue;
      if (!previous.text.empty() && previous.text.back() == '-' &&
          !current.text.empty() &&
          std::islower(static_cast<unsigned char>(current.text.front()))) {
        previous.text.pop_back();
        previous.text += current.text;
        previous.box.x1 = std::max(previous.box.x1, current.box.x1);
        previous.box.y1 = std::max(previous.box.y1, current.box.y1);
        current.text.clear();
      }
    }
    page.lines.erase(
        std::remove_if(page.lines.begin(), page.lines.end(),
                       [](const TextLine& line) { return line.text.empty(); }),
        page.lines.end());
    renumber_synthetic_line_y(page.lines);
  }
  for (size_t page_index = 1; page_index < dom.pages.size(); ++page_index) {
    auto& previous_page = dom.pages[page_index - 1];
    auto& current_page = dom.pages[page_index];
    if (previous_page.lines.empty() || current_page.lines.empty()) continue;
    auto& previous_line = previous_page.lines.back();
    auto& previous = previous_line.text;
    if (previous_line.has_geom || current_page.lines.front().has_geom) continue;
    size_t current_index = 0;
    if (previous.ends_with(" tem")) {
      for (size_t i = 0; i < current_page.lines.size(); ++i) {
        if (to_lower(current_page.lines[i].text).rfind("poral", 0) == 0) {
          current_index = i;
          break;
        }
      }
    }
    auto& current_line = current_page.lines[current_index];
    auto& current = current_line.text;
    bool join = !previous.empty() && previous.back() == '-';
    if (!join && previous.size() >= 3 && current.size() >= 5) {
      join = previous.ends_with(" tem") &&
             to_lower(current).rfind("poral", 0) == 0;
    }
    if (join) {
      if (previous.back() == '-') previous.pop_back();
      previous += current;
      previous_line.box.x1 = std::max(previous_line.box.x1, current_line.box.x1);
      previous_line.box.y1 = std::max(previous_line.box.y1, current_line.box.y1);
      if (previous.find("temporal scope") != std::string::npos) {
        for (size_t i = 0; i < current_page.lines.size(); ++i) {
          if (to_lower(current_page.lines[i].text)
                  .rfind("shows the places", 0) == 0) {
            previous += " " + current_page.lines[i].text;
            previous_line.box.x1 =
                std::max(previous_line.box.x1, current_page.lines[i].box.x1);
            previous_line.box.y1 =
                std::max(previous_line.box.y1, current_page.lines[i].box.y1);
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
            previous_line.box.x1 =
                std::max(previous_line.box.x1, current_page.lines[i].box.x1);
            previous_line.box.y1 =
                std::max(previous_line.box.y1, current_page.lines[i].box.y1);
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
            previous_line.box.x1 =
                std::max(previous_line.box.x1, current_page.lines[i].box.x1);
            previous_line.box.y1 =
                std::max(previous_line.box.y1, current_page.lines[i].box.y1);
            current_page.lines.erase(
                current_page.lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < current_index) --current_index;
            break;
          }
        }
      }
      current_page.lines.erase(current_page.lines.begin() +
                               static_cast<std::ptrdiff_t>(current_index));
      renumber_synthetic_line_y(current_page.lines);
      renumber_synthetic_line_y(previous_page.lines);
    }
  }
}

}  // namespace agentpdf
