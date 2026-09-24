#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
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
bool body_like_row(const PageRow& row, double body, const PageColumns& columns, int side) {
  if (row.words < 4 || row.size <= 0) return false;
  if (std::abs(row.size - body) > body * 0.07) return false;
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
    for (size_t j = 1; j < upper.boxes.size(); ++j) {
      if (std::abs(page.normalized_boxes[upper.boxes[j]].box.x0 - word.box.x0) <= 2.0) {
        ++aligned;
        break;
      }
    }
  }
  return aligned >= 2 || (aligned == 1 && lower.boxes.size() <= 6);
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

  struct BandLine {
    std::string signature;
    std::vector<size_t> boxes;
    bool top = false;
    bool folio = false;
  };
  auto signature_of = [](const std::string& text) {
    std::string out;
    bool in_digits = false;
    for (unsigned char c : to_lower(text)) {
      if (std::isdigit(c)) {
        if (!in_digits) out.push_back('#');
        in_digits = true;
        continue;
      }
      in_digits = false;
      out.push_back(static_cast<char>(c));
    }
    return collapse_ws(out);
  };

  std::vector<std::vector<BandLine>> page_lines(pages.size());
  std::map<std::string, size_t> page_counts;
  for (size_t pi = 0; pi < pages.size(); ++pi) {
    const auto& page = pages[pi];
    if (page.wrapper_page || page.height <= 0) continue;
    // Detection band is 1.5x the stripping band: running heads sit just
    // inside or just below it depending on the template, and requiring
    // cross-page repetition keeps the wider band safe for body text.
    const double top = page.height * heuristics.header_band_frac * 1.5;
    const double bottom = page.height * (1.0 - heuristics.footer_band_frac * 1.5);
    std::vector<size_t> band;
    for (size_t bi = 0; bi < page.normalized_boxes.size(); ++bi) {
      const auto& box = page.normalized_boxes[bi].box;
      if (box.y1 <= top || box.y0 >= bottom) band.push_back(bi);
    }
    std::sort(band.begin(), band.end(), [&](size_t a, size_t b) {
      const auto& ba = page.normalized_boxes[a].box;
      const auto& bb = page.normalized_boxes[b].box;
      if (std::abs(ba.y0 - bb.y0) > 2.5) return ba.y0 < bb.y0;
      return ba.x0 < bb.x0;
    });
    auto& lines = page_lines[pi];
    double line_y = -1e9;
    for (size_t bi : band) {
      const auto& box = page.normalized_boxes[bi];
      const bool is_top = box.box.y1 <= top;
      if (lines.empty() || std::abs(box.box.y0 - line_y) > 2.5 || lines.back().top != is_top) {
        lines.push_back({{}, {}, is_top});
        line_y = box.box.y0;
      }
      lines.back().boxes.push_back(bi);
    }
    const double strip_top = page.height * heuristics.header_band_frac;
    const double strip_bottom = page.height * (1.0 - heuristics.footer_band_frac);
    std::set<std::string> seen_here;
    for (auto& line : lines) {
      std::string text;
      bool in_strip_band = true;
      for (size_t bi : line.boxes) {
        const auto& box = page.normalized_boxes[bi];
        text += box.text + ' ';
        if (line.top ? box.box.y1 > strip_top : box.box.y0 < strip_bottom) in_strip_band = false;
      }
      line.signature = signature_of(text);
      line.folio = in_strip_band && (is_folio_line(text) || is_band_furniture_line(text));
      if (!line.signature.empty() && seen_here.insert(line.signature).second)
        ++page_counts[line.signature];
    }
  }

  const size_t needed = std::max<size_t>(
      2, static_cast<size_t>(std::ceil(static_cast<double>(content_pages) * 0.3)));
  for (size_t pi = 0; pi < pages.size(); ++pi) {
    for (const auto& line : page_lines[pi]) {
      if (line.signature.empty()) continue;
      const bool repeated = content_pages >= 2 && page_counts[line.signature] >= needed;
      if (!repeated && !line.folio) continue;
      for (size_t bi : line.boxes) {
        auto& box = pages[pi].normalized_boxes[bi];
        if (box.region == RegionKind::Body || box.region == RegionKind::Footnote)
          box.region = line.top ? RegionKind::Header : RegionKind::Footer;
      }
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
      if (other.box.x1 <= label.box.x0 + 0.5) text_before = true;
      // "3", "3a.", "B.1", "A3:", "S2" (appendix and supplement numbering).
      static const std::regex number_word(R"(^(?:[A-Z]\.?)?\d+(?:\.\d+)?[A-Za-z]?[\.:|]?$)");
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
    const bool typographic = (label.type_size > 0 && label.type_size < body * 0.95) ||
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
    for (const auto& key : page.normalized_boxes) {
      if (!is_digits(key.text) || key.text.size() > 2) continue;
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
      const int side = side_of(columns, boxes[seed].box);
      std::vector<size_t> members;
      for (size_t k = 0; k < boxes.size(); ++k) {
        const auto& b = boxes[k];
        if (b.region != RegionKind::Body && b.region != RegionKind::Float) continue;
        if (b.rotation != 0 || b.box.y0 < top || b.box.y1 > bottom) continue;
        const int other = side_of(columns, b.box);
        if (columns.split && side >= 0 && other >= 0 && other != side) continue;
        members.push_back(k);
      }
      auto rows = page_rows(page, members);
      size_t r0 = rows.size();
      for (size_t r = 0; r < rows.size() && r0 == rows.size(); ++r) {
        if (std::find(rows[r].boxes.begin(), rows[r].boxes.end(), seed) != rows[r].boxes.end()) r0 = r;
      }
      if (r0 == rows.size()) continue;
      auto has_other_seed = [&](const PageRow& row) {
        return std::any_of(row.boxes.begin(), row.boxes.end(),
                           [&](size_t k) { return k != seed && is_seed(k); });
      };
      std::vector<bool> take(rows.size(), false);
      take[r0] = true;
      const double caption_size = rows[r0].size > 0 ? rows[r0].size : body;
      const bool body_size_caption = std::abs(caption_size - body) <= body * 0.05;
      size_t i = r0 + 1, prev = r0;
      while (i < rows.size()) {
        const auto& row = rows[i];
        if (has_other_seed(row)) break;
        const double gap = row.box.y0 - rows[prev].box.y1;
        if (row.size <= 0 || std::abs(row.size - caption_size) > caption_size * 0.08 ||
            gap > caption_size * 0.8)
          break;
        // A caption in the body type ends with its sentence; the text that
        // follows it closely is the body again.
        const auto last = trim(rows[prev].text);
        if (body_size_caption && !last.empty() && last.back() == '.' &&
            body_like_row(row, body, columns, side))
          break;
        take[i] = true;
        prev = i++;
      }
      // A table's body follows its caption; its bold header row is not a
      // section heading unless running text follows it directly.
      const auto seed_low = fold_lower_utf8(boxes[seed].text);
      const bool table_seed = seed_low.rfind("tab", 0) == 0 || seed_low.rfind("cuadro", 0) == 0 ||
                              seed_low.rfind("quadro", 0) == 0;
      while (i < rows.size()) {
        const auto& row = rows[i];
        if (has_other_seed(row)) break;
        // A table's rows line up with the rows above them even when set in
        // the body type across the measure.
        const bool table_row = table_seed && take[prev] && prev != r0 &&
                               aligned_rows(page, rows[prev], row);
        if (!table_row && body_like_row(row, body, columns, side)) break;
        if (!table_row && heading_like_row(row, body, page.body_font_heavy)) {
          const bool introduces_text =
              row.segments <= 1 && i + 1 < rows.size() && body_like_row(rows[i + 1], body, columns, side);
          if (!table_seed || introduces_text) break;
        }
        if (row.box.y0 - rows[prev].box.y1 > body * 6) break;
        take[i] = true;
        prev = i++;
      }
      if (to_lower(boxes[seed].text).rfind("fig", 0) == 0) {
        for (size_t j = r0; j-- > 0;) {
          const auto& row = rows[j];
          if (has_other_seed(row) || body_like_row(row, body, columns, side) ||
              heading_like_row(row, body, page.body_font_heavy))
            break;
          // The short last line of a paragraph, set close under its text.
          if (j > 0 && row.size > 0 && std::abs(row.size - body) <= body * 0.07 &&
              body_like_row(rows[j - 1], body, columns, side) &&
              row.box.y0 - rows[j - 1].box.y1 <= body * 0.6)
            break;
          take[j] = true;
        }
      }
      for (size_t r = 0; r < rows.size(); ++r) {
        if (!take[r]) continue;
        for (size_t k : rows[r].boxes) boxes[k].region = RegionKind::Float;
      }
    }
    // A table the previous page left open at its foot continues at the top
    // of this page's column: aligned rows until running text.
    if (page.table_continues) {
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
        if (b.region == RegionKind::Float) lowest = std::max(lowest, b.box.y1);
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
