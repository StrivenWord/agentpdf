#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <set>

namespace agentpdf {

namespace {

// ---------------------------------------------------------------------------
// Geometry helpers over front-matter boxes
// ---------------------------------------------------------------------------

struct Segment {
  std::string text;
  double x0 = 0, x1 = 0;
};

struct Row {
  std::string text;
  BBox box;
  double size = 0;                // largest glyph size in the row
  double text_size = 0;           // size carrying most characters (drop caps ignored)
  bool bold = false;              // most characters set in a bold face
  bool marked = false;            // carries super/subscript glyphs (affiliation keys)
  std::vector<Segment> segments;  // column-separated pieces, left to right
};

double box_size(const NormalizedTextBox& b) {
  return b.font_size > 0 ? b.font_size : b.box.height();
}

bool excluded_region(RegionKind region) {
  return region == RegionKind::Header || region == RegionKind::Footer ||
         region == RegionKind::MarginOverlay || region == RegionKind::Wrapper;
}

// Group boxes into visual rows. Boxes smaller than `min_rel` of the row's
// largest glyph (superscript affiliation marks, footnote symbols) are dropped
// from the row text.
std::vector<Row> rows_of(const std::vector<NormalizedTextBox>& boxes, double min_rel) {
  std::vector<const NormalizedTextBox*> sorted;
  for (const auto& b : boxes) {
    if (!excluded_region(b.region) && b.rotation == 0) sorted.push_back(&b);
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
    if (std::abs(a->box.y1 - b->box.y1) > 2.0) return a->box.y1 < b->box.y1;
    return a->box.x0 < b->box.x0;
  });
  // Pass 1 groups glyphs sharing a baseline. A smaller glyph overlapping the
  // current row (super/subscript) is deferred: with tight display leading it
  // may belong to the next line, so pass 2 attaches it to the row whose
  // vertical span holds its centre.
  std::vector<std::vector<const NormalizedTextBox*>> grouped;
  std::vector<const NormalizedTextBox*> deferred;
  for (const auto* b : sorted) {
    const double tol = std::max(2.0, box_size(*b) * 0.45);
    bool placed = false;
    if (!grouped.empty()) {
      auto& row = grouped.back();
      const auto* anchor = row.front();
      const bool overlaps = b->box.y0 < anchor->box.y1 && b->box.y1 > anchor->box.y0;
      const bool smaller = box_size(*b) < box_size(*anchor) * 0.85;
      if (std::abs(b->box.y1 - anchor->box.y1) <= tol) {
        row.push_back(b);
        placed = true;
      } else if (overlaps && smaller) {
        deferred.push_back(b);
        placed = true;
      }
    }
    if (!placed) grouped.push_back({b});
  }
  for (const auto* b : deferred) {
    const double cy = b->box.cy();
    size_t best = 0;
    double best_distance = 1e18;
    for (size_t g = 0; g < grouped.size(); ++g) {
      double y0 = 1e18, y1 = -1e18;
      for (const auto* m : grouped[g]) {
        y0 = std::min(y0, m->box.y0);
        y1 = std::max(y1, m->box.y1);
      }
      const double distance = cy < y0 ? y0 - cy : (cy > y1 ? cy - y1 : 0.0);
      if (distance < best_distance) {
        best_distance = distance;
        best = g;
      }
    }
    grouped[best].push_back(b);
  }
  std::vector<Row> rows;
  for (auto& group : grouped) {
    std::sort(group.begin(), group.end(),
              [](const auto* a, const auto* b) { return a->box.x0 < b->box.x0; });
    Row row;
    for (const auto* b : group) row.size = std::max(row.size, box_size(*b));
    row.box = group.front()->box;
    {
      std::vector<std::pair<double, size_t>> sized;
      size_t chars = 0, bold_chars = 0;
      for (const auto* b : group) {
        sized.emplace_back(box_size(*b), b->text.size());
        chars += b->text.size();
        if (b->bold) bold_chars += b->text.size();
      }
      std::sort(sized.begin(), sized.end());
      size_t acc = 0;
      for (const auto& [size, n] : sized) {
        acc += n;
        if (acc * 2 >= chars) {
          row.text_size = size;
          break;
        }
      }
      row.bold = bold_chars * 2 > chars;
    }
    double last_x1 = -1;  // right edge of the last full-size glyph (column gaps)
    double prev_x1 = -1;  // right edge of the last emitted glyph (tight joins)
    double last_size = 0;
    for (const auto* b : group) {
      const bool small = box_size(*b) < row.text_size * 0.8;
      if (small) row.marked = true;
      if (box_size(*b) < row.size * min_rel) continue;
      // A wide horizontal gap separates side-by-side columns; keep the gap
      // visible so column neighbours are not read as one phrase. Small glyphs
      // (super/subscripts, possibly from the adjacent line) never open a
      // column and do not move the gap reference.
      const bool new_segment =
          row.text.empty() || (!small && b->box.x0 - last_x1 > row.text_size * 2.0);
      const bool touching = !row.text.empty() &&
                            b->box.x0 - prev_x1 < std::min(last_size, box_size(*b)) * 0.08;
      if (!row.text.empty()) row.text += new_segment ? " | " : (touching ? "" : " ");
      row.text += b->text;
      if (new_segment) row.segments.push_back({b->text, b->box.x0, b->box.x1});
      else {
        row.segments.back().text += (touching ? "" : " ") + b->text;
        row.segments.back().x1 = b->box.x1;
      }
      if (!small || last_x1 < 0) last_x1 = std::max(last_x1, b->box.x1);
      prev_x1 = b->box.x1;
      last_size = box_size(*b);
      row.box.x0 = std::min(row.box.x0, b->box.x0);
      row.box.x1 = std::max(row.box.x1, b->box.x1);
      row.box.y0 = std::min(row.box.y0, b->box.y0);
      row.box.y1 = std::max(row.box.y1, b->box.y1);
    }
    row.text = collapse_ws(row.text);
    if (!row.text.empty()) rows.push_back(std::move(row));
  }
  return rows;
}

size_t letter_count(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if (std::isalpha(c) || c >= 0xC0) ++n;
  }
  return n;
}

std::string alnum_fold(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    else if (c >= 0x80) out.push_back(static_cast<char>(c));
  }
  return out;
}

std::vector<std::string> title_tokens(const std::string& s) {
  std::vector<std::string> out;
  for (const auto& w : split_words(to_lower(s))) {
    const auto folded = alnum_fold(w);
    if (letter_count(folded) >= 3) out.push_back(folded);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Title
// ---------------------------------------------------------------------------

bool is_junk_info_title(const std::string& title) {
  const auto low = to_lower(trim(title));
  if (letter_count(low) < 4) return true;
  static const std::regex file_like(R"(\.(pdf|docx?|tmp|indd|tex|dvi|rtf|odt)\s*$)");
  static const std::regex page_range_id(R"(\b\d+\.\.\d+\b)");  // "es6c06362 1..6"
  if (std::regex_search(low, file_like) || std::regex_search(low, page_range_id)) return true;
  if (low.rfind("microsoft word", 0) == 0 || low == "untitled" ||
      low.find("manuscript") != std::string::npos)
    return true;
  // Identifier-only strings: every token contains a digit.
  const auto words = split_words(low);
  return std::all_of(words.begin(), words.end(), [](const std::string& w) {
    return std::any_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); });
  });
}

// Share of the title's words that occur in the document's opening text.
double text_support(const std::string& title, const std::string& folded_front_text) {
  const auto tokens = title_tokens(title);
  if (tokens.empty()) return 0;
  size_t found = 0;
  for (const auto& t : tokens) {
    if (folded_front_text.find(t) != std::string::npos) ++found;
  }
  return static_cast<double>(found) / static_cast<double>(tokens.size());
}

struct TitleBlock {
  std::string text;
  BBox box;
  double size = 0;
  bool bold = false;
  bool found = false;
};

bool is_strong_byline(const Row& r, bool several_names_count);

// Journal mastheads are often set larger than the article title.
bool is_masthead_row(const std::string& text) {
  static const std::regex masthead(
      R"(\b(journal|revista|rivista|revue|zeitschrift|dergisi|magazine|proceedings|transactions|bulletin|annals|anais|cadernos|quaderni)\b)",
      std::regex::icase);
  return std::regex_search(text, masthead);
}

bool title_eligible(const Row& r, double page_height) {
  if (r.box.y0 > page_height * 0.62) return false;
  if (r.segments.size() > 1) return false;  // side-by-side masthead or info fields
  if (split_words(r.text).size() < 2 || letter_count(r.text) < 8) return false;
  if (is_provenance_line(r.text) || is_masthead_row(r.text)) return false;
  return r.text.find("http") == std::string::npos && r.text.find("www.") == std::string::npos;
}

// Mastheads without journal vocabulary: the journal name recurs in the first
// page's furniture (top line, citation strip). Adjacency to publisher lines
// is no evidence: titles routinely sit right under "journal homepage…" or a
// DOI line.
bool is_masthead_in_context(const std::vector<Row>& rows, size_t i, double page_height) {
  const auto self = alnum_fold(rows[i].text);
  if (self.size() >= 8) {
    for (size_t j = 0; j < rows.size(); ++j) {
      // Only recurrences in page furniture count; a short title may well be
      // repeated in the body or a caption.
      const bool furniture =
          rows[j].box.y0 < page_height * 0.12 || rows[j].box.y0 > page_height * 0.88;
      if (j != i && furniture && alnum_fold(rows[j].text).find(self) != std::string::npos)
        return true;
    }
  }
  return false;
}

TitleBlock text_layer_title(const std::vector<Row>& rows, double page_height) {
  TitleBlock tb;
  double best = 0;
  std::vector<bool> eligible(rows.size(), false);
  for (size_t i = 0; i < rows.size(); ++i) {
    eligible[i] =
        title_eligible(rows[i], page_height) && !is_masthead_in_context(rows, i, page_height);
    if (eligible[i]) best = std::max(best, rows[i].text_size);
  }
  if (best <= 0) return tb;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    if (r.box.y0 > page_height * 0.62) continue;
    if (!tb.found) {
      if (r.text_size < best - 0.6 || !eligible[i]) continue;
      tb.text = r.text;
      tb.box = r.box;
      tb.size = r.text_size;
      tb.bold = r.bold;
      tb.found = true;
      continue;
    }
    // Continuation rows: same size and weight, directly below, and not a
    // byline (documents set entirely in one size separate title and names
    // only by weight or content).
    if (r.text_size < best - 0.6) break;
    if (r.box.y0 - tb.box.y1 > best * 1.3) break;
    if (r.bold != tb.bold) break;
    if (is_strong_byline(r, false)) break;
    if (!tb.text.empty() && tb.text.back() == '-') tb.text += r.text;
    else tb.text += " " + r.text;
    tb.box.y1 = std::max(tb.box.y1, r.box.y1);
    tb.box.x0 = std::min(tb.box.x0, r.box.x0);
    tb.box.x1 = std::max(tb.box.x1, r.box.x1);
  }
  tb.text = collapse_ws(tb.text);
  // Strip column separators introduced by rows_of.
  if (tb.text.find(" | ") != std::string::npos) tb.text = trim(tb.text.substr(0, tb.text.find(" | ")));
  return tb;
}

// ---------------------------------------------------------------------------
// Authors
// ---------------------------------------------------------------------------

bool is_name_token(const std::string& t) {
  static const std::set<std::string> particles{
      "de", "da", "do", "dos", "das", "del", "della", "di", "du", "van", "von",
      "der", "den", "la", "le", "bin", "al", "el", "y", "e", "ten", "ter"};
  if (t.empty()) return false;
  if (particles.count(t)) return true;
  const auto c = static_cast<unsigned char>(t.front());
  if (std::any_of(t.begin(), t.end(), [](unsigned char ch) { return std::isdigit(ch); }))
    return false;
  if (t.find('@') != std::string::npos || t.back() == ':') return false;
  // Initials: "J.", "J.-P.", "A.K."
  static const std::regex initials(R"(^([A-Z]\.)(-?[A-Z]\.)*$)");
  if (std::regex_match(t, initials)) return true;
  // Capitalised word (ASCII capital, or a non-ASCII leading letter).
  return std::isupper(c) || c >= 0xC0;
}

bool is_person_name(const std::string& raw) {
  static const std::set<std::string> stop{
      "university", "universidad", "universidade", "università", "universität",
      "üniversitesi", "department", "departamento", "dipartimento", "institute",
      "instituto", "istituto", "school", "college", "laboratory", "laboratories",
      "center", "centre", "faculty", "facultad", "inc", "inc.", "corp", "ltd", "llc",
      "abstract", "keywords", "journal", "received", "article", "research", "review",
      "vol.", "volume", "press", "association", "society", "engineering", "sciences",
      "science", "technology", "national", "ministry", "agency", "group", "division",
      "program", "corresponding", "author", "authors", "editor", "editors", "edited",
      "reviewed", "reviewer", "specialty", "citation", "published", "accepted", "report",
      "news", "open", "access", "full", "text", "key", "takeaways", "highlights",
      "document", "link", "page", "section", "chapter", "contents", "table", "figure"};
  std::vector<std::string> tokens;
  {
    std::string cur;
    for (char ch : raw) {
      if (ch == ' ') {
        if (!cur.empty()) tokens.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(ch);
      }
    }
    if (!cur.empty()) tokens.push_back(cur);
  }
  if (tokens.size() < 2 || tokens.size() > 5) return false;
  for (const auto& t : tokens) {
    if (stop.count(to_lower(t))) return false;
    if (!is_name_token(t)) return false;
  }
  // At least one token must be a full word, not an initial or particle.
  return std::any_of(tokens.begin(), tokens.end(), [](const std::string& t) {
    return letter_count(t) >= 2 && t.find('.') == std::string::npos;
  });
}

// Split an author string on the separators journals print between names
// (and the spaced dash some templates put before an affiliation).
std::vector<std::string> split_author_pieces(std::string s) {
  s = decode_html_entities(s);
  static const std::regex by_prefix(R"(^\s*(by|por|di|von)\s+)", std::regex::icase);
  s = std::regex_replace(s, by_prefix, "");
  static const std::regex seps(
      R"(\s*(,|;|&|·|•|\||\s-\s|\band\b|\by\b|\be\b|\bve\b|\bund\b)\s*)");
  s = std::regex_replace(s, seps, "\n");
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    auto pos = s.find('\n', start);
    if (pos == std::string::npos) pos = s.size();
    auto part = collapse_ws(s.substr(start, pos - start));
    if (!part.empty()) out.push_back(part);
    start = pos + 1;
  }
  return out;
}

// Remove footnote/affiliation marks glued to a name ("Karczeski1", "Chin*",
// trailing "a,b" keys).
std::string clean_name_piece(std::string part) {
  // Alternation, not a character class: std::regex matches bytes, and a
  // class of multibyte symbols would also match bytes inside accented letters.
  static const std::regex marks(R"(([0-9*#]|†|‡|§|¶|∗|✉)+|\(\s*\)|\bORCID\b)");
  part = std::regex_replace(part, marks, " ");
  static const std::regex trailing_keys(R"((\s+[a-z](\s*,\s*[a-z])*)+$)");
  return trim(std::regex_replace(collapse_ws(part), trailing_keys, ""));
}

std::vector<std::string> split_author_list(const std::string& s) {
  std::vector<std::string> out;
  for (const auto& piece : split_author_pieces(s)) {
    auto cleaned = clean_name_piece(piece);
    if (!cleaned.empty()) out.push_back(cleaned);
  }
  return out;
}

// Affiliation text: parentheses, standalone numbers (postcodes, P.O. boxes)
// or institution vocabulary.
bool is_affiliation_piece(const std::string& piece) {
  if (piece.find('(') != std::string::npos) return true;
  static const std::regex standalone_number(R"((^|\s)\d+(\s|$))");
  if (std::regex_search(piece, standalone_number)) return true;
  static const std::regex institution(
      R"(\b(universit\w*|univ\.|department|departamento|dipartimento|institut\w*|istituto|school|college|laborator\w*|lab|center|centre|faculty|facultad|faculdade|hospital|academy|ministry|agency|corporation|company|inc\.?|ltd\.?|llc|gmbh|foundation|council)\b)",
      std::regex::icase);
  return std::regex_search(piece, institution) || piece.find("Üniversitesi") != std::string::npos;
}

// Names at the start of a byline row, up to the first affiliation piece.
std::vector<std::string> byline_names(const std::string& text, bool* affiliated = nullptr) {
  std::vector<std::string> names;
  size_t invalid = 0;
  if (affiliated) *affiliated = false;
  static const std::regex corporate_suffix(R"(^(inc|ltd|llc|gmbh|corp|co|plc|s\.?a|ag|bv)\.?$)",
                                           std::regex::icase);
  for (const auto& piece : split_author_pieces(text)) {
    if (std::regex_match(piece, corporate_suffix)) {
      // "Eastgate Systems, Inc.": the preceding piece named a company.
      if (!names.empty()) names.pop_back();
      break;
    }
    if (is_affiliation_piece(piece)) {
      if (affiliated) *affiliated = !names.empty();
      break;
    }
    const auto cleaned = clean_name_piece(piece);
    if (cleaned.empty()) continue;
    if (is_person_name(cleaned)) names.push_back(cleaned);
    else ++invalid;
  }
  if (names.empty() || invalid * 10 > (names.size() + invalid) * 3) return {};
  return names;
}

// Title-case title lines parse as names too; a byline proper shows
// affiliation keys, a "By" prefix, or several names.
// `several_names_count` admits rows that merely list several names; that is
// only evidence below title size, since title-case title lines with commas
// parse the same way.
bool is_strong_byline(const Row& r, bool several_names_count) {
  bool affiliated = false;
  const auto names = byline_names(r.text, &affiliated);
  if (names.empty()) return false;
  static const std::regex by_prefix(R"(^\s*(by|por|di|von)\s)", std::regex::icase);
  static const std::regex text_marks(R"([0-9*]|†|‡|∗)");
  return r.marked || affiliated || (several_names_count && names.size() >= 2) ||
         std::regex_search(r.text, by_prefix) || std::regex_search(r.text, text_marks);
}

std::vector<std::string> valid_names(const std::vector<std::string>& parts, double& ratio) {
  std::vector<std::string> names;
  for (const auto& p : parts) {
    if (is_person_name(p)) names.push_back(p);
  }
  ratio = parts.empty() ? 0 : static_cast<double>(names.size()) / static_cast<double>(parts.size());
  return names;
}

std::vector<std::string> text_layer_authors(const std::vector<Row>& rows, const TitleBlock& title,
                                            double page_height) {
  // Candidate rows below the title, restricted to the title's horizontal
  // extent (side rails with editor lists or article info sit beside it).
  std::vector<Row> window;
  for (const auto& r : rows) {
    if (r.box.y0 < title.box.y1 - 1.0) continue;  // at or above the title block
    if (window.size() >= 8 || r.box.y0 > page_height * 0.65) break;
    if (front_label_of(r.text) != FrontLabel::None) break;
    Row clipped = r;
    clipped.text.clear();
    for (const auto& seg : r.segments) {
      const double overlap = std::min(seg.x1, title.box.x1) - std::max(seg.x0, title.box.x0);
      if (overlap > 0) clipped.text += (clipped.text.empty() ? "" : " ") + seg.text;
    }
    if (clipped.text.empty()) continue;
    if (is_prose_line(clipped.text)) break;
    window.push_back(std::move(clipped));
  }
  // The byline starts at the first strong byline row; a weak one (a single
  // unmarked name) counts only when set smaller than the title, which keeps
  // subtitles out.
  size_t start = window.size();
  for (size_t i = 0; i < window.size() && start == window.size(); ++i) {
    if (is_strong_byline(window[i], window[i].text_size < title.size * 0.95)) start = i;
  }
  if (start == window.size()) {
    for (size_t i = 0; i < window.size(); ++i) {
      if (window[i].text_size < title.size * 0.95 && !byline_names(window[i].text).empty()) {
        start = i;
        break;
      }
    }
  }
  std::vector<std::string> authors;
  for (size_t i = start; i < window.size(); ++i) {
    const auto names = byline_names(window[i].text);
    if (names.empty()) break;
    authors.insert(authors.end(), names.begin(), names.end());
  }
  return authors;
}

std::string surname_of(const std::string& name) {
  const auto words = split_words(name);
  return words.empty() ? std::string{} : alnum_fold(words.back());
}

// ---------------------------------------------------------------------------
// DOI
// ---------------------------------------------------------------------------

std::string clean_doi(std::string d) {
  while (!d.empty() && (d.back() == '.' || d.back() == ',' || d.back() == ';' ||
                        d.back() == ']' || d.back() == '>' || d.back() == '"')) {
    d.pop_back();
  }
  // Drop a closing parenthesis that has no opening partner inside the DOI.
  if (!d.empty() && d.back() == ')' &&
      std::count(d.begin(), d.end(), '(') < std::count(d.begin(), d.end(), ')'))
    d.pop_back();
  return d;
}

std::string front_matter_doi(const DocumentDom& dom) {
  static const std::regex labelled(
      R"((?:\bdoi\s*[:.]?\s*|doi\.org/|/doi/(?:abs/|full/|pdf/|epdf/)?)(10\.\d{4,9}/[^\s"<>,;]+))",
      std::regex::icase);
  static const std::regex bare(R"(\b(10\.\d{4,9}/[^\s"<>,;]+))");
  std::string fallback;
  for (size_t p = 0; p < dom.front_boxes.size(); ++p) {
    std::string text;
    for (const auto& b : dom.front_boxes[p]) {
      if (to_lower(b.text) == "references") break;  // citations follow
      text += b.text + ' ';
    }
    std::smatch m;
    if (std::regex_search(text, m, labelled)) return clean_doi(m[1]);
    if (fallback.empty() && p <= 1 && std::regex_search(text, m, bare)) fallback = clean_doi(m[1]);
  }
  return fallback;
}

// ---------------------------------------------------------------------------
// Keywords
// ---------------------------------------------------------------------------

std::vector<std::string> split_keyword_text(const std::string& s) {
  static const std::regex seps(R"(\s*(;|,|·|•)\s*)");
  std::vector<std::string> out;
  std::sregex_token_iterator it(s.begin(), s.end(), seps, -1), end;
  for (; it != end; ++it) {
    auto k = trim(it->str());
    while (!k.empty() && (k.back() == '.' || k.back() == ';' || k.back() == '"')) k.pop_back();
    while (!k.empty() && k.front() == '"') k.erase(0, 1);
    k = trim(k);
    if (!k.empty() && k.size() < 80) out.push_back(k);
  }
  return out;
}

bool has_keyword_separator(const std::string& s) {
  return s.find_first_of(";,") != std::string::npos || s.find("·") != std::string::npos ||
         s.find("•") != std::string::npos;
}

std::vector<std::string> keywords_from_lines(const std::vector<TextLine>& lines, size_t start,
                                             const std::string& glued) {
  std::vector<std::string> gathered;
  auto stop_line = [](const std::string& t) {
    return front_label_of(t) != FrontLabel::None || is_prose_line(t) ||
           is_provenance_line(t);
  };
  if (!glued.empty()) {
    std::string text = glued;
    for (size_t i = start; i < lines.size() && i < start + 4; ++i) {
      if (text.back() == '.') break;
      const auto next = collapse_ws(normalize_typography(lines[i].text));
      if (next.empty() || stop_line(next)) break;
      std::string label, rest;
      if (split_front_label(next, label, rest) != FrontLabel::None) break;
      if (!std::islower(static_cast<unsigned char>(next.front())) &&
          !has_keyword_separator(text.substr(text.size() > 1 ? text.size() - 2 : 0)))
        break;
      text += " " + next;
    }
    return split_keyword_text(text);
  }
  auto norm = [&](size_t k) { return collapse_ws(normalize_typography(lines[k].text)); };
  auto ends_with_separator = [](const std::string& t) {
    return !t.empty() && (t.back() == ',' || t.back() == ';');
  };
  if (start < lines.size() && is_prose_line(norm(start))) return {};  // abstract text, not a list
  if (start < lines.size() && has_keyword_separator(norm(start))) {
    // Separator-delimited list: it continues only while the text so far ends
    // with a separator (wrapped list), so neighbouring sidebar fragments are
    // not swallowed.
    std::string text = norm(start);
    for (size_t i = start + 1; i < lines.size() && i < start + 6 && ends_with_separator(text); ++i) {
      const auto t = norm(i);
      if (t.empty() || stop_line(t)) break;
      text += " " + t;
    }
    return split_keyword_text(text);
  }
  // One keyword per line, ended by a label, prose, or a line closing with a
  // full stop (which still belongs to the list).
  for (size_t i = start; i < lines.size() && gathered.size() < 16; ++i) {
    const auto t = norm(i);
    if (t.empty()) continue;
    if (stop_line(t) || split_words(t).size() > 8) break;
    std::string label, rest;
    if (split_front_label(t, label, rest) != FrontLabel::None) break;
    if (!gathered.empty() && (t.front() == '(' || std::islower(static_cast<unsigned char>(t.front())))) {
      gathered.back() += " " + t;  // wrapped keyword
    } else {
      gathered.push_back(t);
    }
    if (t.back() == '.') break;
  }
  std::vector<std::string> out;
  for (auto& g : gathered) {
    g = trim(g);
    while (!g.empty() && g.back() == '.') g.pop_back();
    if (!g.empty() && g.size() < 80) out.push_back(g);
  }
  return out;
}

// Keyword boxes set in their own column beside the abstract appear after the
// abstract in reading order; read them geometrically instead: the segments
// stacked under the Keywords label, in its column.
std::vector<std::string> keywords_from_geometry(const DocumentDom& dom) {
  for (size_t p = 0; p < dom.front_boxes.size() && p < dom.pages.size(); ++p) {
    if (dom.pages[p].wrapper_page) continue;
    const auto rows = rows_of(dom.front_boxes[p], 0.8);
    for (size_t i = 0; i < rows.size(); ++i) {
      for (const auto& label : rows[i].segments) {
        if (front_label_of(label.text) != FrontLabel::Keywords) continue;
        std::vector<std::string> lines;
        double last_y1 = rows[i].box.y1;
        for (size_t j = i + 1; j < rows.size() && lines.size() < 12; ++j) {
          if (rows[j].box.y0 - last_y1 > rows[i].text_size * 3.0) break;
          const Segment* below = nullptr;
          for (const auto& seg : rows[j].segments) {
            // In the label's column: starts near it and overlaps it.
            if (seg.x0 > label.x0 - 12.0 && seg.x0 < label.x1 + 12.0) below = &seg;
          }
          if (!below) {
            // A segment running into the label's column means the column
            // text merged with its neighbour: stop rather than splice
            // non-adjacent lines into a false keyword.
            bool occluded = false;
            for (const auto& seg : rows[j].segments) {
              if (seg.x0 <= label.x0 - 12.0 && seg.x1 > label.x0) occluded = true;
            }
            if (occluded && !lines.empty()) {
              lines.clear();
              break;
            }
            continue;  // a row of the neighbouring column only
          }
          if (front_label_of(below->text) != FrontLabel::None || is_prose_line(below->text)) break;
          lines.push_back(below->text);
          last_y1 = rows[j].box.y1;
        }
        if (lines.empty()) continue;
        std::string joined;
        for (const auto& l : lines) joined += (joined.empty() ? "" : " ") + l;
        if (has_keyword_separator(joined)) return split_keyword_text(joined);
        std::vector<std::string> out;
        for (const auto& l : lines) {
          if (l.size() < 80) out.push_back(trim(l));
        }
        return out;
      }
    }
  }
  return {};
}

}  // namespace

void extract_front_matter_metadata(DocumentDom& dom) {
  // Carry body-classification regions onto the font-bearing evidence boxes
  // (both come from the same Poppler text list, in the same order).
  for (size_t p = 0; p < dom.front_boxes.size() && p < dom.pages.size(); ++p) {
    auto& front = dom.front_boxes[p];
    const auto& body = dom.pages[p].normalized_boxes;
    if (front.size() != body.size()) continue;
    for (size_t i = 0; i < front.size(); ++i) {
      if (front[i].text == body[i].text) front[i].region = body[i].region;
    }
    if (dom.pages[p].wrapper_page) {
      for (auto& b : front) b.region = RegionKind::Wrapper;
    }
  }

  std::string folded_front;
  for (const auto& page : dom.front_boxes) {
    for (const auto& b : page) folded_front += alnum_fold(b.text) + ' ';
  }

  // First content page carries the title block.
  TitleBlock title;
  std::vector<Row> rows;
  double page_height = 0;
  for (size_t p = 0; p < dom.front_boxes.size() && p < dom.pages.size(); ++p) {
    if (dom.pages[p].wrapper_page) continue;
    page_height = dom.pages[p].height;
    title = text_layer_title(rows_of(dom.front_boxes[p], 0.0), page_height);
    rows = rows_of(dom.front_boxes[p], 0.8);  // superscript marks dropped for bylines
    if (title.found) break;
  }

  // Title: the Info title when it is not junk and the page text supports it;
  // otherwise the text-layer title.
  const std::string info_title = decode_html_entities(dom.info_title);
  const bool info_ok = !is_junk_info_title(info_title);
  const bool info_supported = info_ok && text_support(info_title, folded_front) >= 0.7;
  if (info_supported) {
    dom.meta.title = info_title;
    // Prefer the printed title when the Info title only wraps it with extra
    // identifiers (report numbers, quotes).
    if (title.found) {
      const auto a = alnum_fold(title.text);
      const auto b = alnum_fold(info_title);
      const auto at = b.find(a);
      if (!a.empty() && a.size() < b.size() && at != std::string::npos) {
        // The surplus must be identifier-like (digits), not title words lost
        // from the printed block (drop caps, decorative initials).
        const std::string surplus = b.substr(0, at) + b.substr(at + a.size());
        if (std::any_of(surplus.begin(), surplus.end(),
                        [](unsigned char c) { return std::isdigit(c); }))
          dom.meta.title = title.text;
      }
    }
  } else if (title.found) {
    dom.meta.title = title.text;
  } else if (info_ok) {
    dom.meta.title = info_title;
  }
  // Some templates print the field label itself ("TÍTULO:", "Title:").
  static const std::regex title_label(
      R"(^\s*(title|t\xC3\x8DTULO|t\xC3\xADtulo|titulo|titolo|titre|titel|ba\xC5\x9Fl\xC4\xB1k)\s*:\s*)",
      std::regex::icase);
  dom.meta.title = std::regex_replace(dom.meta.title, title_label, "");

  // Authors: text-layer names below the title, reconciled with the Info list.
  std::vector<std::string> text_authors;
  if (title.found) text_authors = text_layer_authors(rows, title, page_height);
  double info_ratio = 0;
  auto info_authors = valid_names(split_author_list(dom.info_author), info_ratio);
  if (info_ratio < 1.0) info_authors.clear();  // any junk piece discredits the field
  if (!text_authors.empty() && !info_authors.empty()) {
    const bool agree = surname_of(text_authors.front()) == surname_of(info_authors.front());
    dom.meta.authors =
        (agree && info_authors.size() > text_authors.size()) ? info_authors : text_authors;
  } else if (!text_authors.empty()) {
    dom.meta.authors = text_authors;
  } else {
    dom.meta.authors = info_authors;
  }

  dom.meta.doi = front_matter_doi(dom);
}

void extract_and_validate_metadata(DocumentDom& dom, const MetadataSpec& /*spec*/) {
  if (dom.meta.object_url.empty() && !dom.meta.doi.empty()) {
    dom.meta.object_url = "https://doi.org/" + dom.meta.doi;
  }

  // Abstract body: paragraph after an Abstract heading — require substantive prose.
  for (const auto& page : dom.pages) {
    bool after_abs = false;
    for (const auto& b : page.blocks) {
      if (b.kind == BlockKind::Heading && front_label_of(b.text) == FrontLabel::Abstract) {
        after_abs = true;
        continue;
      }
      if (after_abs && b.kind == BlockKind::Paragraph) {
        auto words = split_words(b.text);
        if (words.size() >= 40) {
          dom.meta.abstract_text = b.text;
        }
        break;
      }
      // Keywords set beside or between the label and its text are skipped;
      // other headings end the search under this label (some templates print
      // an Abstract label over a highlights box first, so later labels count).
      if (after_abs && b.kind == BlockKind::ListItem) continue;
      if (after_abs && b.kind == BlockKind::Heading &&
          front_label_of(b.text) != FrontLabel::Keywords)
        after_abs = false;
    }
    if (!dom.meta.abstract_text.empty()) break;
  }

  // Keywords: read from visual lines of the first content pages, which keep
  // one-keyword-per-line lists that paragraph building would merge.
  int content_pages = 0;
  for (const auto& page : dom.pages) {
    if (page.wrapper_page) continue;
    if (++content_pages > 3) break;
    for (size_t i = 0; i < page.lines.size(); ++i) {
      std::string label, rest;
      const auto text = collapse_ws(normalize_typography(page.lines[i].text));
      if (split_front_label(text, label, rest) != FrontLabel::Keywords) continue;
      dom.meta.keywords = keywords_from_lines(page.lines, i + 1, rest);
      if (!dom.meta.keywords.empty()) break;
    }
    if (!dom.meta.keywords.empty()) break;
  }
  if (dom.meta.keywords.empty()) dom.meta.keywords = keywords_from_geometry(dom);

  if (dom.meta.date_extracted.empty()) dom.meta.date_extracted = today_iso_date();
  if (dom.meta.source_format.empty()) dom.meta.source_format = "PDF";
}

}  // namespace agentpdf
