#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <regex>

namespace agentpdf {

namespace {

bool fold_char(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

unsigned char folded(unsigned char c) {
  return std::isalnum(c) ? static_cast<unsigned char>(std::tolower(c)) : c;
}

// Lowercase letters of a printed word, apostrophes and inner hyphens kept:
// the key the document's own vocabulary is indexed by.
std::string word_key(const std::string& word) {
  std::string out;
  for (unsigned char c : word) {
    if (std::isalpha(c) || c >= 0x80) out.push_back(static_cast<char>(std::tolower(c)));
    else if ((c == '-' || c == '\'') && !out.empty()) out.push_back(static_cast<char>(c));
  }
  while (!out.empty() && (out.back() == '-' || out.back() == '\'')) out.pop_back();
  return out;
}

bool ends_with_line_hyphen(const std::string& text) {
  return text.size() >= 2 && text.back() == '-' &&
         std::isalpha(static_cast<unsigned char>(text[text.size() - 2]));
}

bool starts_lowercase(const std::string& text) {
  if (text.empty()) return false;
  const auto c = static_cast<unsigned char>(text[0]);
  if (std::islower(c)) return true;
  // Lowercase Latin-1 letters (C3 9F..C3 BF) open many non-English words.
  return c == 0xC3 && text.size() > 1 && static_cast<unsigned char>(text[1]) >= 0x9F;
}

// Productive English prefixes that keep their hyphen in compounds
// ("cross-language", "self-reported", "well-structured").
bool compounding_prefix(const std::string& key) {
  static const char* prefixes[] = {"self", "cross", "well", "half", "quasi", "semi", "non",
                                   "high", "low", "long", "short", "full", "post", "multi"};
  for (const char* p : prefixes) {
    if (key == p) return true;
  }
  return false;
}

const std::regex& unit_before_exponent() {
  // Units whose superscript is an exponent (km², m³), not a note marker.
  static const std::regex unit(R"((?:^|[^A-Za-z])(?:k?m|cm|mm|ft|mi|in|kg|g|s|kWh|MWh|GWh|TWh)$)");
  return unit;
}

struct VisualLine {
  std::vector<size_t> boxes;  // all boxes, in text-layer order
  int flow_line = -1;         // the flow-text line its words came from
  bool block_start = false;   // first line of a Poppler text block
};

bool same_row(const NormalizedTextBox& a, const NormalizedTextBox& b) {
  if (a.rotation != b.rotation) return false;
  const double ha = std::max(a.box.height(), 0.1);
  const double hb = std::max(b.box.height(), 0.1);
  const double overlap = std::min(a.box.y1, b.box.y1) - std::max(a.box.y0, b.box.y0);
  if (overlap < 0.35 * std::min(ha, hb)) return false;
  return b.box.x0 >= a.box.x0 - 0.5;
}

// A lowered, smaller word set tight against the previous one: a chemical or
// variable subscript ("O" + "3", "PM" + "2.5"), part of the same word.
bool is_subscript_of(const NormalizedTextBox& base, const NormalizedTextBox& sub) {
  if (base.type_size <= 0 || sub.type_size <= 0) return false;
  if (sub.type_size > base.type_size * 0.88) return false;
  if (sub.box.x0 - base.box.x1 > base.type_size * 0.15) return false;
  const double h = std::max(base.box.height(), 0.1);
  return sub.box.cy() > base.box.cy() + h * 0.08 && sub.box.y0 > base.box.y0 + h * 0.2;
}

// A raised, smaller word set tight against the previous one: a note or
// citation marker ("precedent" + "1") or an exponent ("km" + "2").
bool is_superscript_of(const NormalizedTextBox& base, const NormalizedTextBox& sup) {
  if (base.type_size <= 0 || sup.type_size <= 0) return false;
  if (sup.type_size > base.type_size * 0.88) return false;
  if (sup.box.x0 - base.box.x1 > base.type_size * 0.15) return false;
  const double h = std::max(base.box.height(), 0.1);
  return sup.box.cy() < base.box.cy() - h * 0.08 && sup.box.y1 < base.box.y1 - h * 0.15;
}

// A drop cap: one capital set several lines tall at a paragraph's start.
bool is_drop_cap(const NormalizedTextBox& cap, double next_size) {
  return cap.text.size() == 1 && std::isupper(static_cast<unsigned char>(cap.text[0])) &&
         cap.type_size > 0 && next_size > 0 && cap.type_size >= next_size * 1.8;
}

bool digits_marker(const std::string& text) {
  if (text.empty()) return false;
  bool digit = false;
  for (unsigned char c : text) {
    if (std::isdigit(c)) digit = true;
    else if (c != ',' && c != '-' && c != ' ') return false;
  }
  return digit;
}


// Sentence-final punctuation at the end of a line, after any closing quotes
// or brackets and a trailing note or citation marker ("…centers.12").
bool ends_paragraph_like(const std::string& text) {
  auto t = trim(text);
  // A trailing note marker (\x1F12\x1F) reads like a citation number.
  t.erase(std::remove(t.begin(), t.end(), '\x1F'), t.end());
  size_t i = t.size();
  size_t digits = i;
  while (digits > 0 && (std::isdigit(static_cast<unsigned char>(t[digits - 1])) ||
                        t[digits - 1] == ',' || t[digits - 1] == '-'))
    --digits;
  if (digits < i && digits > 0 && std::strchr(".!?", t[digits - 1])) i = digits;
  while (i > 0 && std::strchr("\"')]*", t[i - 1])) --i;
  return i > 0 && std::strchr(".!?:", t[i - 1]) != nullptr;
}

// A paragraph opens with a capital, a digit, a quote or bracket, a bullet
// or a non-ASCII letter; never with a lowercase letter.
bool starts_paragraph_like(const std::string& text) {
  if (text.empty()) return false;
  const auto c = static_cast<unsigned char>(text[0]);
  if (std::isupper(c) || std::isdigit(c) || c == '"' || c == '\'' || c == '(' || c == '[')
    return true;
  if (c == 0xC3) return text.size() > 1 && static_cast<unsigned char>(text[1]) < 0x9F;
  return c >= 0xC4;
}

double horizontal_overlap(const BBox& a, const BBox& b) {
  return std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
}

bool same_column(const BBox& a, const BBox& b) {
  const double narrower = std::min(a.width(), b.width());
  return narrower > 0 && horizontal_overlap(a, b) >= 0.5 * narrower;
}

double percentile(std::vector<double> values, double q) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const auto at = static_cast<size_t>(std::floor(q * static_cast<double>(values.size() - 1) + 0.5));
  return values[std::min(at, values.size() - 1)];
}

double em_of(const TextLine& line) {
  if (line.font_size > 0) return line.font_size;
  return std::max(line.geom.height() * 0.8, 6.0);
}

}  // namespace

Vocabulary build_vocabulary(const std::vector<PageDom>& pages) {
  Vocabulary vocab;
  for (const auto& page : pages) {
    const auto& boxes = page.normalized_boxes;
    for (size_t k = 0; k < boxes.size(); ++k) {
      const auto& text = boxes[k].text;
      // Line-end fragments ("infor-") and their continuations are not words.
      if (ends_with_line_hyphen(text)) continue;
      if (k > 0 && ends_with_line_hyphen(boxes[k - 1].text) && !same_row(boxes[k - 1], boxes[k]))
        continue;
      for (const auto& word : split_words(text)) {
        const auto key = word_key(word);
        if (key.size() >= 2) vocab.words.insert(key);
      }
    }
  }
  return vocab;
}

// Whether "first-" at a line end and "second" on the next line form a
// hyphenated compound ("data-driven") rather than one word broken by the
// typesetter ("infor-mation"). The document's own usage decides first; a
// compound's parts are words the document also prints on their own.
bool keep_line_end_hyphen(const std::string& first, const std::string& second,
                          const Vocabulary& vocab) {
  const auto a = word_key(first);
  const auto b = word_key(second);
  if (a.empty() || b.empty()) return false;
  if (vocab.words.count(a + "-" + b)) return true;
  if (vocab.words.count(a + b)) return false;
  if (compounding_prefix(a) && b.size() >= 3) return true;
  return a.size() >= 4 && b.size() >= 3 && a.find('-') == std::string::npos &&
         vocab.words.count(a) && vocab.words.count(b);
}

namespace {

// Superscript digits in running text stand for a note or citation marker
// until footnotes are linked (link_note_markers): kNoteMark digits kNoteMark.
constexpr char kNoteMark = '\x1F';

bool note_key_text(const std::string& text) {
  if (text.empty() || text.size() > 3) {
    return text == "\xE2\x80\xA0" || text == "\xE2\x80\xA1" || text == "\xC2\xA7";  // † ‡ §
  }
  if (text == "*" || text == "**") return true;
  return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

// The words of one region (running text, or footnotes) as lines with their
// own geometry and type.
std::vector<TextLine> region_lines(const std::vector<VisualLine>& visual,
                                   const std::vector<NormalizedTextBox>& boxes, RegionKind region,
                                   const Vocabulary& vocab) {
  std::vector<TextLine> out;
  const bool notes = region == RegionKind::Footnote;
  for (const auto& vl : visual) {
    TextLine line;
    std::string text;
    long previous_kept = -1;
    double chars = 0, size_sum = 0, size_max = 0, heavy = 0, italic = 0;
    bool first = true;
    std::vector<size_t> kept;
    for (size_t k : vl.boxes) {
      if (boxes[k].region == region && !boxes[k].text.empty()) kept.push_back(k);
    }
    // A footnote's printed key opens its first line: a number or symbol set
    // smaller or higher than the note's text.
    size_t begin = 0;
    if (notes && kept.size() >= 2 && note_key_text(boxes[kept[0]].text)) {
      const auto& key = boxes[kept[0]];
      const auto& next = boxes[kept[1]];
      const bool smaller = key.type_size > 0 && next.type_size > 0 && key.type_size < next.type_size * 0.9;
      const bool raised = key.box.cy() < next.box.cy() - next.box.height() * 0.1;
      if (smaller || raised || key.box.x1 <= next.box.x0) {
        line.note_key = key.text;
        begin = 1;
      }
    }
    for (size_t idx = begin; idx < kept.size(); ++idx) {
      const size_t k = kept[idx];
      const auto& box = boxes[k];
      bool marker = false;
      if (!text.empty()) {
        const auto& prev = boxes[static_cast<size_t>(previous_kept)];
        const bool adjacent = previous_kept + 1 == static_cast<long>(k);
        std::string sep = " ";
        if (text.size() == 1 && is_drop_cap(prev, box.type_size) && starts_lowercase(box.text)) {
          sep.clear();
        } else if (adjacent && is_subscript_of(prev, box)) {
          sep.clear();
        } else if (adjacent && is_superscript_of(prev, box) && digits_marker(box.text)) {
          // Exponents stay on their unit ("km2"); other superscript numbers
          // are note or citation markers, resolved once notes are known.
          sep.clear();
          marker = !notes && !std::regex_search(prev.text, unit_before_exponent());
        } else if (adjacent && !prev.space_after) {
          sep.clear();
        }
        text += sep;
      }
      if (marker) {
        text += kNoteMark;
        text += box.text;
        text += kNoteMark;
      } else {
        text += box.text;
      }
      if (first) {
        line.geom = box.box;
        first = false;
      } else {
        line.geom.x0 = std::min(line.geom.x0, box.box.x0);
        line.geom.x1 = std::max(line.geom.x1, box.box.x1);
        line.geom.y0 = std::min(line.geom.y0, box.box.y0);
        line.geom.y1 = std::max(line.geom.y1, box.box.y1);
      }
      if (box.type_size > 0) {
        const double n = static_cast<double>(fold_alnum(box.text).size());
        chars += n;
        size_sum += box.type_size * n;
        size_max = std::max(size_max, box.type_size);
        if (box.type_heavy) heavy += n;
        if (box.type_italic) italic += n;
      }
      previous_kept = static_cast<long>(k);
    }
    text = collapse_ws(text);
    if (text.empty()) continue;
    line.text = text;
    line.has_geom = true;
    if (chars > 0) {
      line.font_size = size_sum / chars;
      line.font_size_max = size_max;
      line.bold = heavy / chars >= 0.6;
      line.italic = italic / chars >= 0.6;
    }
    line.block_start = vl.block_start;
    out.push_back(std::move(line));
  }

  // A drop cap Poppler set apart as its own line opens the next line's word.
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    auto& cap = out[i];
    auto& next = out[i + 1];
    if (cap.text.size() != 1 || !std::isupper(static_cast<unsigned char>(cap.text[0]))) continue;
    if (cap.font_size <= 0 || next.font_size <= 0 || cap.font_size < next.font_size * 1.8) continue;
    if (!starts_lowercase(next.text)) continue;
    next.text = cap.text + next.text;
    next.block_start = next.block_start || cap.block_start;
    cap.text.clear();
  }

  // A note key Poppler read as a line of its own (a raised numeral beside
  // the note's first line) opens that line's note.
  if (notes) {
    for (size_t i = 0; i + 1 < out.size(); ++i) {
      auto& key = out[i];
      auto& next = out[i + 1];
      if (!key.note_key.empty() || !next.note_key.empty() || !note_key_text(key.text)) continue;
      if (key.geom.y1 < next.geom.y0 - 2.0 || key.geom.x0 > next.geom.x0 + 1.0) continue;
      next.note_key = key.text;
      key.text.clear();
    }
  }

  // Line-end hyphens: the next line's first word joins this line's last,
  // with or without the hyphen as the document's usage says. A note's
  // first line never continues the previous note.
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    auto& a = out[i];
    auto& b = out[i + 1];
    if (!b.note_key.empty()) continue;
    if (!ends_with_line_hyphen(a.text) || !starts_lowercase(b.text)) continue;
    const auto space = b.text.find(' ');
    const std::string word = b.text.substr(0, space);
    const auto last_space = a.text.rfind(' ');
    const std::string stem =
        a.text.substr(last_space == std::string::npos ? 0 : last_space + 1);
    const bool keep = keep_line_end_hyphen(stem.substr(0, stem.size() - 1), word, vocab);
    if (!keep) a.text.pop_back();
    a.text += word;
    b.text = space == std::string::npos ? std::string() : trim(b.text.substr(space + 1));
  }
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const TextLine& line) { return line.text.empty(); }),
            out.end());
  // Stream coordinates only: paragraph breaks come from the page geometry
  // (mark_paragraph_starts), not from gaps in this stream.
  renumber_synthetic_line_y(out);
  return out;
}

}  // namespace

LineColumns measure_line_columns(const std::vector<TextLine>& lines) {
  LineColumns cols;
  const size_t n = lines.size();
  cols.left.assign(n, 0);
  cols.right.assign(n, 0);
  cols.justified.assign(n, false);
  for (size_t i = 0; i < n; ++i) {
    std::vector<double> left, right;
    for (size_t j = 0; j < n; ++j) {
      if (!lines[j].has_geom || !same_column(lines[i].geom, lines[j].geom)) continue;
      left.push_back(lines[j].geom.x0);
      right.push_back(lines[j].geom.x1);
    }
    if (left.empty()) {
      cols.left[i] = lines[i].geom.x0;
      cols.right[i] = lines[i].geom.x1;
      continue;
    }
    const bool many = left.size() >= 5;
    cols.left[i] = many ? percentile(left, 0.15) : *std::min_element(left.begin(), left.end());
    cols.right[i] = many ? percentile(right, 0.85) : *std::max_element(right.begin(), right.end());
    const double em = em_of(lines[i]);
    size_t flush = 0;
    for (double x : right) flush += x >= cols.right[i] - em ? 1 : 0;
    cols.justified[i] = right.size() >= 3 && flush * 2 >= right.size();
  }
  return cols;
}

bool line_is_short(const TextLine& line, const LineColumns& cols, size_t i) {
  return cols.justified[i] && line.geom.x1 < cols.right[i] - 1.5 * em_of(line);
}

void mark_paragraph_starts(std::vector<TextLine>& lines) {
  const auto cols = measure_line_columns(lines);
  auto indent_of = [&](size_t i) { return lines[i].geom.x0 - cols.left[i]; };
  auto indented = [&](size_t i) {
    const double em = em_of(lines[i]);
    const double indent = indent_of(i);
    return indent >= 0.6 * em && indent <= 5.0 * em;
  };
  for (size_t i = 0; i < lines.size(); ++i) {
    auto& line = lines[i];
    line.para_start = false;
    if (!line.has_geom || !starts_paragraph_like(line.text)) continue;
    if (i == 0) {
      line.para_start = indented(i);
      continue;
    }
    const auto& prev = lines[i - 1];
    if (!prev.has_geom || !ends_paragraph_like(prev.text)) continue;
    const bool column = same_column(prev.geom, line.geom) && prev.geom.y0 < line.geom.y0;
    // An indented first line; not a run of equally indented lines (a
    // quotation, a list, a centred block).
    if (indented(i) && !(column && indented(i - 1) &&
                         std::abs(indent_of(i) - indent_of(i - 1)) < em_of(line) * 0.5)) {
      line.para_start = true;
      continue;
    }
    // Flush-left paragraphs: the previous line stops short of the measure,
    // or a blank line's worth of space separates the blocks.
    if (line_is_short(prev, cols, i - 1)) {
      line.para_start = true;
      continue;
    }
    if (column && line.block_start && line.geom.y0 - prev.geom.y1 > 0.9 * em_of(line))
      line.para_start = true;
  }
}

void mark_page_turn_paragraphs(std::vector<PageDom>& pages) {
  const PageDom* previous = nullptr;
  for (auto& page : pages) {
    if (page.wrapper_page || page.lines.empty()) continue;
    auto& first = page.lines.front();
    if (previous && first.has_geom && !first.para_start && starts_paragraph_like(first.text)) {
      const auto& last = previous->lines.back();
      if (last.has_geom && ends_paragraph_like(last.text)) {
        const auto cols = measure_line_columns(previous->lines);
        if (line_is_short(last, cols, previous->lines.size() - 1)) first.para_start = true;
      }
    }
    previous = &page;
  }
}

bool flow_box_lines(const std::string& flow_text, const PageDom& page, const Vocabulary& vocab,
                    std::vector<TextLine>& out, std::vector<TextLine>* notes) {
  out.clear();
  if (notes) notes->clear();
  const auto& boxes = page.normalized_boxes;
  // Folded text layer, each character owned by its box.
  std::string layer;
  std::vector<size_t> owner;
  for (size_t k = 0; k < boxes.size(); ++k) {
    for (unsigned char c : boxes[k].text) {
      if (!fold_char(c)) continue;
      layer.push_back(static_cast<char>(folded(c)));
      owner.push_back(k);
    }
  }

  // Walk the flow text: every folded character must be the next one of the
  // text layer (both are Poppler's word list in reading order). A Poppler
  // block ends at a blank line.
  std::vector<bool> block_start(boxes.size(), false);
  std::vector<int> flow_line_of(boxes.size(), -1);
  size_t pos = 0;
  int flow_line = 0;
  bool blank_before = true;
  size_t line_begin = 0;
  auto consume_line = [&](const std::string& raw) {
    const std::string text = normalize_typography(raw);
    bool any = false;
    for (unsigned char c : text) {
      if (!fold_char(c)) continue;
      if (pos >= layer.size() || static_cast<unsigned char>(layer[pos]) != folded(c)) return false;
      const size_t k = owner[pos++];
      if (flow_line_of[k] < 0) {
        flow_line_of[k] = flow_line;
        if (!any && blank_before) block_start[k] = true;
      }
      any = true;
    }
    if (any) {
      blank_before = false;
      ++flow_line;
    } else if (trim(text).empty()) {
      blank_before = true;
    }
    return true;
  };
  for (size_t i = 0; i <= flow_text.size(); ++i) {
    if (i == flow_text.size() || flow_text[i] == '\n' || flow_text[i] == '\f') {
      if (!consume_line(flow_text.substr(line_begin, i - line_begin))) return false;
      if (i < flow_text.size() && flow_text[i] == '\f') blank_before = true;
      line_begin = i + 1;
    }
  }
  if (pos != layer.size()) return false;

  // Visual lines: consecutive words of one flow line on one row. Poppler
  // joins a hyphenated line to the next inside a block, so a flow line can
  // hold two rows; the geometry splits them again.
  std::vector<VisualLine> visual;
  for (size_t k = 0; k < boxes.size(); ++k) {
    // Boxes without folded characters (a lone "•", "(", "|") have no flow
    // line of their own; geometry alone places them.
    const bool known = flow_line_of[k] >= 0;
    bool new_line = visual.empty();
    if (!new_line) {
      const auto& current = visual.back();
      if (!same_row(boxes[current.boxes.back()], boxes[k])) new_line = true;
      else if (known && current.flow_line >= 0 && flow_line_of[k] != current.flow_line)
        new_line = true;
    }
    if (new_line) visual.push_back({});
    auto& current = visual.back();
    if (known && current.flow_line < 0) {
      current.flow_line = flow_line_of[k];
      current.block_start = block_start[k];
    }
    current.boxes.push_back(k);
  }

  out = region_lines(visual, boxes, RegionKind::Body, vocab);
  mark_paragraph_starts(out);
  if (notes) *notes = region_lines(visual, boxes, RegionKind::Footnote, vocab);
  return true;
}

}  // namespace agentpdf
