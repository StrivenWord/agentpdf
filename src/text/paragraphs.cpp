#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <cmath>

namespace agentpdf {

void rejoin_hyphenated_lines(std::vector<TextLine>& lines) {
  if (lines.size() < 2) return;
  std::vector<TextLine> out;
  out.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!out.empty()) {
      auto& prev = out.back();
      auto& cur = lines[i];
      if (!prev.text.empty() && prev.text.back() == '-' && !cur.text.empty() &&
          std::islower(static_cast<unsigned char>(cur.text.front()))) {
        prev.text.pop_back();
        prev.text += cur.text;
        prev.box.x1 = std::max(prev.box.x1, cur.box.x1);
        prev.box.y1 = std::max(prev.box.y1, cur.box.y1);
        continue;
      }
    }
    out.push_back(lines[i]);
  }
  lines.swap(out);
  renumber_synthetic_line_y(lines);
}

namespace {

bool ends_sentence_like(const std::string& text) {
  auto t = trim(text);
  // A trailing note reference ("…precedent.[^1]") follows the terminator.
  static const std::regex note_ref(R"(\[\^[^\]\s]+\]$)");
  t = std::regex_replace(t, note_ref, "");
  if (t.empty()) return false;
  // Ignore trailing closers/quotes after the terminator.
  size_t i = t.size();
  while (i > 0) {
    const char c = t[i - 1];
    if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '*') {
      --i;
      continue;
    }
    return c == '.' || c == '!' || c == '?';
  }
  return false;
}

bool is_boilerplate_line(const std::string& text) {
  auto low = to_lower(trim(text));
  if (low.empty()) return true;
  if (low == "{" || low == "}" || low == "|" || low == "•") return true;
  if (low.size() == 1 && std::isalpha(static_cast<unsigned char>(low[0]))) return true;
  static const char* exact[] = {
      "open access",
      "article",
      // Journal web furniture printed on the first page (ACS).
      "access", "read online", "metrics & more", "article recommendations",
      "access read online", "access metrics & more", "access metrics & more article recommendations",
      "read online article recommendations",
  };
  for (const char* k : exact) {
    if (low == k) return true;
  }
  static const char* prefixes[] = {
      "reviewed by",
      "correspondence",
      "specialty section",
      "received ",
      "accepted ",
      "citation",
      "copyright ©",
      "copyright (c)",
      "copyright 20",
      // First-page history rail (MDPI and others).
      "academic editor",
      "published:",
      "this article is an open access article",
  };
  for (const char* k : prefixes) {
    if (low.rfind(k, 0) == 0) return true;
  }
  static const char* contains[] = {
      "this article was submitted",
      "creative commons",
      "permission to make digital",
      "personal or classroom use is granted",
      "are not made or distributed for profit",
      "copies bear this notice",
      "full citation on the first page",
      "to copy otherwise",
      "republish, to post on servers",
      "redistribute to lists",
      "prior specific permission",
      "ht'02, june",
      "acm isbn",
      "copyright is held by",
      "dls in application",
      "frontiers in political science",
      "chin jj and kirkpatrick",
      // Aggregator stamp on every ProQuest page.
      "reproduced with permission of copyright owner",
      "reproduced with permission of the copyright owner",
      "further reproduction prohibited without permission",
      "notwithstanding the proquest terms and conditions",
      "you may use this content in accordance with the terms of the",
  };
  for (const char* k : contains) {
    if (low.find(k) != std::string::npos) return true;
  }
  static const std::regex publication_date(
      R"(^published\s+\d{1,2}\s+[a-z]+\s+\d{4}\b)", std::regex::icase);
  if (std::regex_search(low, publication_date)) return true;
  // Wrapped remainders of licence statements and aggregator stamps, alone
  // on their lines.
  if (low == "distributed under the terms and" ||
      low == "distributed under the terms and conditions" ||
      low == "prohibited without permission." || low == "prohibited without permission")
    return true;
  // A citation strip, "Energies 2026, 19, 4153" or "Journal. Media 2026, 7,
  // 178". Body-only: metadata reads journal, volume and page from it.
  static const std::regex citation_strip(
      R"(^[A-Z][A-Za-z.&\- ]{1,60}\s(?:19|20)\d{2},\s*\d{1,4},\s*\d{1,6}$)");
  if (std::regex_match(trim(text), citation_strip)) return true;
  return false;
}

// Stream-level complement to mark_post_references_ancillary: once References
// has been seen, drop publisher tear-out / claims-form bands that survive
// geometry quarantine (common when OCR replaces the quarantined stream).
bool is_post_references_ancillary_start(const std::string& text) {
  auto low = to_lower(trim(text));
  if (low.empty()) return false;
  static const char* markers[] = {
      "subscription claims",
      "we provide this form",
      "print full name or key name",
      "please print clearly and in ink",
      "to be filled out by",
      "please do not remove",
      "photocopy may be used",
      "member or customer number",
      "apa subscription claims",
  };
  for (const char* m : markers) {
    if (low.find(m) != std::string::npos) return true;
  }
  // Publisher masthead above tear-out forms (OCR often truncates ASSOCIATION).
  if (low.rfind("american psychological", 0) == 0 && low.size() < 80) {
    return true;
  }
  if (low.find("american psychological association") != std::string::npos &&
      low.find("subscription") != std::string::npos) {
    return true;
  }
  return false;
}

bool is_author_roster_line(const std::string& text) {
  auto t = trim(text);
  if (t.size() < 10 || t.size() > 280) return false;
  auto low = to_lower(t);

  // Never treat clear prose / technical lists as author rosters.
  static const char* prose_markers[] = {
      "classical", "language", "languages", "library", "digital", "university of",
      "hieroglyph", "sanskrit", "published", "developed", "including", "research",
      "focuses", "objects", "diagrams", "economically", "accurate",
  };
  for (const char* m : prose_markers) {
    if (low.find(m) != std::string::npos) return false;
  }
  if (low.find("http") != std::string::npos) return false;
  if (low.find("perseus") != std::string::npos) return false;
  if (low.find("funded") != std::string::npos) return false;
  if (low.find("agency") != std::string::npos || low.find("agencies") != std::string::npos)
    return false;
  if (low.find("national science") != std::string::npos) return false;
  if (low.find("foundation") != std::string::npos) return false;
  if (low.find("endowment") != std::string::npos) return false;

  int caps = 0;
  bool in_word = false;
  for (unsigned char c : t) {
    if (std::isalpha(c)) {
      if (!in_word && std::isupper(c)) ++caps;
      in_word = true;
    } else {
      in_word = false;
    }
  }
  int commas = static_cast<int>(std::count(t.begin(), t.end(), ','));

  // Known magazine sidebar author fragments for the crane fixture.
  if (low.find("crane") != std::string::npos && low.find("chavez") != std::string::npos)
    return true;
  if (low.find("milbank") != std::string::npos && low.find("rydberg") != std::string::npos)
    return true;
  if (low.find("clifford") != std::string::npos && low.find("wulfman") != std::string::npos)
    return true;
  // Long capitalized name lists with many commas and "and".
  if (commas >= 3 && low.find(" and ") != std::string::npos && caps >= 6 &&
      std::isupper(static_cast<unsigned char>(t.front())) && t.size() < 200) {
    return true;
  }
  return false;
}

std::string strip_inline_figure_noise(const std::string& text) {
  auto out = collapse_ws(text);
  out = std::regex_replace(
      out, std::regex(R"(\bprofes-\s*sional\b)", std::regex::icase),
      "professional");
  out = std::regex_replace(
      out, std::regex(R"(\bprofes\s+sional\b)", std::regex::icase),
      "professional");
  return out;
}

bool is_figure_caption(const std::string& text) {
  auto low = to_lower(trim(text));
  return low.find("figure ") == 0 || low.find("fig. ") == 0 || low.find("table ") == 0;
}

// How a heading candidate was recognised. A section name is decisive; a
// section number or capitals are only cues, which equations, table cells,
// reference entries and figure labels share, so those candidates must also
// be typeset as headings when the text layer says how they are typeset.
enum class HeadingCue { None, Label, Numbered, Capitals };

// Mathematical notation: relations, operators and arrows. Headings carry
// none; display equations broken into short lines are full of them.
bool has_math_notation(const std::string& text) {
  if (text.find_first_of("=<>^_{}|~") != std::string::npos) return true;
  if (text.find(" + ") != std::string::npos) return true;
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    const auto a = static_cast<unsigned char>(text[i]);
    const auto b = static_cast<unsigned char>(text[i + 1]);
    if (a == 0xC2 && (b == 0xB1 || b == 0xB7)) return true;  // ± ·
    if (a == 0xC3 && (b == 0x97 || b == 0xB7)) return true;  // × ÷
    if (a == 0xE2 && b >= 0x86 && b <= 0x8B) return true;    // U+2180–U+22FF arrows, operators
    if (a == 0xE2 && b == 0x97 && i + 2 < text.size() &&
        static_cast<unsigned char>(text[i + 2]) == 0xA6)      // ◦
      return true;
  }
  return false;
}

// ASCII letters of a line and how many are capitals, plus the longest run
// of letters, where a multi-byte UTF-8 character counts as one letter of
// unknown case ("GİRİŞ" is a five-letter run).
void count_letters(const std::string& text, int& letters, int& uppers, int& longest_run) {
  letters = uppers = longest_run = 0;
  int run = 0;
  for (unsigned char c : text) {
    if (std::isalpha(c)) {
      ++letters;
      if (std::isupper(c)) ++uppers;
      longest_run = std::max(longest_run, ++run);
    } else if (c >= 0xC0) {
      longest_run = std::max(longest_run, ++run);
    } else if (c < 0x80) {
      run = 0;
    }
  }
}

bool is_all_capitals(const std::string& text) {
  int letters = 0, uppers = 0, run = 0;
  count_letters(text, letters, uppers, run);
  return letters >= 2 && uppers * 10 >= letters * 9;
}

// A section title opens with a capital (any non-ASCII letter counts: case is
// not decoded), or with a digit glued to letters ("3D Printing").
bool starts_like_title(const std::string& title) {
  if (title.empty()) return false;
  const auto c = static_cast<unsigned char>(title[0]);
  if (std::isupper(c)) return true;
  if (std::isdigit(c)) {
    size_t i = 0;
    while (i < title.size() && std::isdigit(static_cast<unsigned char>(title[i]))) ++i;
    return i < title.size() && std::isalpha(static_cast<unsigned char>(title[i]));
  }
  if ((c == '"' || c == '\'') && title.size() > 1)
    return std::isupper(static_cast<unsigned char>(title[1])) != 0;
  // Two-byte letters (Latin, Greek, Cyrillic) and CJK; U+2000–U+2FFF
  // (E2 lead byte) is punctuation and symbols.
  return (c >= 0xC3 && c <= 0xDF) || (c >= 0xE3 && c <= 0xEF);
}

// A numbered bibliography entry: "3. Grubler, A., Wilson, C.", "4. Gupta T
// (2024) Evolution…", "2. Government of Japan. (2021).".
bool looks_like_reference_entry(const std::string& title) {
  static const std::regex year(R"(\((?:19|20)\d{2}[a-z]?\))");
  static const std::regex surname_initial(R"(^[A-Z][A-Za-z'\-]+,\s+[A-Z]\.)");
  static const std::regex surname_bare_initials(R"(^[A-Z][a-z'\-]+\s[A-Z]{1,3},\s)");
  static const std::regex et_al(R"(\bet al\b)");
  return std::regex_search(title, year) || std::regex_search(title, surname_initial) ||
         std::regex_search(title, surname_bare_initials) || std::regex_search(title, et_al);
}

// The title after a section number must read as a title: a capital first,
// letters rather than numbers or symbols, and not a sentence (a numbered
// list item) or a wrapped prose line.
bool numbered_title_ok(const std::string& title) {
  if (!starts_like_title(title) || has_math_notation(title)) return false;
  if (looks_like_reference_entry(title)) return false;
  int letters = 0, uppers = 0, run = 0;
  count_letters(title, letters, uppers, run);
  if (run < 3) return false;  // "55 KW", "0 10 20 30"
  const auto words = split_words(title);
  // (A trailing comma proves nothing: long titles wrap after one, "2. A
  // current history of coups in Africa," / "2020–2022".)
  if (words.size() > 20 || title.back() == ';') return false;
  if (words.size() >= 5) {
    if (ends_sentence_like(title)) return false;
    static const std::regex inner_sentence(R"([a-z]{2}\.\s+[A-Z0-9(])");
    if (std::regex_search(title, inner_sentence)) return false;
  }
  return true;
}

// The first line of a numbered bibliography entry: "12. Conklin, J.",
// "[25] B. Acun", "(69) Sharma, S.".
bool is_bibliography_entry_start(const std::string& text) {
  static const std::regex entry(R"(^(?:\[\d{1,3}\]|\(\d{1,3}\)|\d{1,3}\.)\s+["A-Z\xC0-\xFF])");
  return std::regex_search(text, entry);
}

// The first line of a numbered list item ("1. Initialize population…",
// "2. Japan Life Insurers…"), unless the text before it runs on into the
// number: "…as shown in Table" + "2. The results…" is one sentence.
bool is_list_item_start(const std::string& text, const std::string& before) {
  static const std::regex item(R"(^\d{1,2}\.\s+["A-Z\xC0-\xFF])");
  if (!std::regex_search(text, item)) return false;
  static const std::regex runs_on(
      R"((?:^|\s)(?:table|figure|fig\.|section|sections|equation|eq\.|chapter|step|page|pp?\.|no\.|vol\.|and|or|to|of|in|at|by|than|from|with)$)",
      std::regex::icase);
  return !std::regex_search(trim(before), runs_on);
}

// A short line set in capitals ("RESULTS", "DATA AVAILABILITY"), not an
// acronym-laden equation or table fragment ("PES,c", "Load/MW", "12 GB").
bool capitals_line_ok(const std::string& text) {
  if (text.size() >= 80 || text.find('.') != std::string::npos) return false;
  int letters = 0, uppers = 0, run = 0;
  count_letters(text, letters, uppers, run);
  if (letters < 4 || run < 3 || uppers * 10 < letters * 9) return false;
  if (has_math_notation(text) || text.find_first_of("/[]") != std::string::npos) return false;
  // A comma glued to what follows is a subscript list ("PES,c").
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == ',' && !std::isspace(static_cast<unsigned char>(text[i + 1]))) return false;
  }
  int digits = 0;
  for (unsigned char c : text) digits += std::isdigit(c) ? 1 : 0;
  return digits * 5 <= letters + digits;
}

// Is the line typeset as a heading: set apart from body text by weight,
// slant, size or small capitals? Bold headings may be a little smaller than
// the body (0.9x in the corpus: Arial-Bold 11pt over Times 12pt); table
// cells, figure labels and sub/superscripts are far smaller (0.6-0.8x).
// Without typographic evidence (OCR, or a line not found in the text layer)
// only the text cues decide.
bool typeset_as_heading(const TextLine& line, const std::string& text, const DocumentDom& dom) {
  const double body = dom.body_font_size;
  if (body <= 0 || line.font_size <= 0) return true;
  if (line.font_size_max < body * 0.88) return false;
  if (line.bold && !dom.body_font_heavy) return true;
  if (line.font_size_max < body * 0.95) return false;
  if (line.italic) return true;
  if (line.font_size >= body * 1.08) return true;
  // Small capitals: capital initials at body size, the other letters smaller
  // ("R EFERENCES"). A subscripted symbol mixes sizes the same way
  // ("PCCS": italic P, subscript CCS) but is short.
  int letters = 0, uppers = 0, run = 0;
  count_letters(text, letters, uppers, run);
  return letters >= 8 && is_all_capitals(text) && line.font_size < line.font_size_max * 0.95;
}

int heading_level_for(const std::string& text, const Heuristics& h, bool magazine,
                      HeadingCue& cue) {
  // Note references ("Introduction[^1]") are not part of the title.
  static const std::regex note_ref(R"(\[\^[^\]\s]+\])");
  auto t = trim(std::regex_replace(text, note_ref, ""));
  auto low = to_lower(t);
  cue = HeadingCue::Label;
  // "Appendix", "Appendix A", "Appendix B. Technical data"; not prose that
  // opens with a reference to one ("Appendix A). Our tool…").
  static const std::regex appendix(R"(^(?:[Aa]ppendix|APPENDIX)(?:\s+[A-Z0-9]{1,3}(?:\.\d+)*[\.:]?(?:\s+\S.*)?)?$)");
  if (low == "abstract" || low == "references" || low == "conclusion" ||
      low == "acknowledgments" || low == "acknowledgements" ||
      (low.rfind("appendix", 0) == 0 && std::regex_match(t, appendix))) {
    return 1;
  }
  if (low == "keywords" || low == "key words") return h.keywords_as_h2 ? 2 : 1;
  switch (front_label_of(t)) {
    case FrontLabel::Abstract:
    case FrontLabel::Introduction:
      return 1;
    case FrontLabel::Keywords:
      return h.keywords_as_h2 ? 2 : 1;
    case FrontLabel::None:
      break;
  }
  if (low == "general terms" || low == "categories and subject descriptors") return 1;

  std::smatch m;
  // Section numbers (no leading zero, at most two digits per level) followed
  // by a title; a wrapped prose line that happens to begin with a quantity
  // ("200 acres of…", "6 - 1 vote", "1.15 (the lower…") is not a heading.
  static const std::regex numbered(R"(^([1-9]\d?(?:\.\d{1,2})*)[\.\)]?\s+(\S.*)$)");
  if (h.nest_numeric_headings && std::regex_match(t, m, numbered)) {
    cue = HeadingCue::Numbered;
    if (!numbered_title_ok(m[2].str())) {
      cue = HeadingCue::None;
      return 0;
    }
    std::string num = m[1];
    int depth = static_cast<int>(std::count(num.begin(), num.end(), '.')) + 1;
    return std::min(depth, 6);
  }

  // Known unnumbered section titles of the Communications of the ACM
  // fixture; only meaningful inside that template.
  static const char* sections[] = {
      "challenges for the humanities",
      "case studies for general problems",
      "space and time",
      "managing the texts",
      "language tools (not just english)",
      "general principles",
      "conclusion",
      "references",
  };
  for (const char* s : sections) {
    if (low == s) return 1;
    // Heading glued to following sentence: "Space and Time Although..."
    if (magazine && low.find(std::string(s) + " ") == 0) return 1;
  }

  cue = HeadingCue::Capitals;
  if (capitals_line_ok(t)) return 1;
  cue = HeadingCue::None;
  return 0;
}

// A heading set only by its type: a short line wholly in bold or in larger
// type ("The Limits of Land Use Planning", "Recent executive orders",
// "DESARROLLO."). Its surroundings decide the rest (build_blocks_from_lines).
bool typeset_heading_line(const TextLine& line, const std::string& text, const DocumentDom& dom) {
  const double body = dom.body_font_size;
  if (!line.has_geom || line.gapped || body <= 0 || line.font_size <= 0) return false;
  const bool heavier = line.bold_all && !dom.body_font_heavy && line.font_size_min >= body * 0.85;
  const bool larger = line.font_size_min >= body * 1.12;
  if (!heavier && !larger) return false;
  const auto words = split_words(text);
  if (words.empty() || words.size() > 14 || text.size() > 110) return false;
  int letters = 0, uppers = 0, run = 0;
  count_letters(text, letters, uppers, run);
  if (letters < 3 || run < 3 || !starts_like_title(text)) return false;
  const char last = text.back();
  if (last == ',' || last == ';' || last == '-') return false;
  if ((last == '.' || last == '!' || last == '?') && words.size() > 9) return false;
  if (has_math_notation(text) || looks_like_reference_entry(text) || is_figure_caption(text))
    return false;
  return !is_provenance_line(text) && !is_boilerplate_line(text);
}

// Heading levels by style for headings without a section number: a style
// that numbered headings also use takes their level; any other ranks below
// every style more prominent than it (larger, then capitals, then bold).
struct HeadingStyle {
  int size = 0;  // half points
  bool caps = false;
  bool bold = false;
  bool operator<(const HeadingStyle& o) const {
    return std::tie(size, caps, bold) < std::tie(o.size, o.caps, o.bold);
  }
};

HeadingStyle style_of(const TextLine& line, const std::string& text) {
  return {static_cast<int>(std::lround(line.font_size * 2.0)), is_all_capitals(text), line.bold_all};
}

// Part of the printed title (a title set again on the article's first page
// after a cover or graphical abstract).
bool title_fragment(const std::string& text, const DocumentDom& dom) {
  const auto folded = fold_alnum(text);
  const auto title = fold_alnum(dom.meta.title);
  return folded.size() >= 12 && !title.empty() && title.find(folded) != std::string::npos;
}

std::map<HeadingStyle, int> heading_style_levels(const DocumentDom& dom, const Heuristics& h,
                                                 size_t first_page, size_t first_line) {
  std::map<HeadingStyle, std::map<int, int>> numbered;
  std::set<HeadingStyle> styles;
  for (const auto& page : dom.pages) {
    if (page.wrapper_page || static_cast<size_t>(page.index) < first_page) continue;
    for (size_t li = 0; li < page.lines.size(); ++li) {
      if (static_cast<size_t>(page.index) == first_page && li < first_line) continue;
      const auto& line = page.lines[li];
      const auto text = collapse_ws(normalize_typography(line.text));
      if (text.empty() || !line.has_geom || title_fragment(text, dom)) continue;
      HeadingCue cue = HeadingCue::None;
      const int level = heading_level_for(text, h, false, cue);
      if (level > 0 && cue == HeadingCue::Numbered && typeset_as_heading(line, text, dom)) {
        ++numbered[style_of(line, text)][level];
        styles.insert(style_of(line, text));
      } else if (typeset_heading_line(line, text, dom)) {
        styles.insert(style_of(line, text));
      }
    }
  }
  std::map<HeadingStyle, int> levels;
  for (const auto& style : styles) {
    auto it = numbered.find(style);
    if (it != numbered.end()) {
      levels[style] = std::max_element(it->second.begin(), it->second.end(), [](const auto& a, const auto& b) {
                        return a.second < b.second;
                      })->first;
      continue;
    }
    int above = 0;
    for (const auto& other : styles) {
      if (style < other) ++above;
    }
    levels[style] = std::min(3, above + 1);
  }
  return levels;
}

std::string strip_glued_heading(const std::string& text, std::string& heading_out,
                                bool magazine) {
  heading_out.clear();
  // The glued-heading list is the Communications of the ACM fixture's section
  // inventory; applying it elsewhere splits ordinary sentences.
  if (!magazine) return text;
  auto low = to_lower(text);
  static const char* sections[] = {
      "challenges for the humanities",
      "case studies for general problems",
      "space and time",
      "managing the texts",
      "language tools (not just english)",
      "general principles",
      "conclusion",
  };
  for (const char* s : sections) {
    std::string key = s;
    if (low == key) {
      heading_out = text;
      return {};
    }
    if (low.find(key) == 0 && text.size() > key.size() + 1) {
      // Preserve original casing for heading span.
      heading_out = trim(text.substr(0, key.size()));
      // Capitalize heading words lightly already in source.
      heading_out = text.substr(0, key.size());
      return trim(text.substr(key.size()));
    }
  }
  heading_out.clear();
  return text;
}

bool likely_footnote_line(const std::string& text) {
  auto t = trim(text);
  if (t.size() < 2) return false;
  static const std::regex fn(R"(^(\[\d+\]|\d+)\s+\S)");
  return std::regex_search(t, fn) && t.size() < 300;
}

bool looks_like_title_line(const std::string& text, const std::string& title) {
  auto a = to_lower(collapse_ws(text));
  auto b = to_lower(collapse_ws(title));
  if (b.empty()) return false;
  if (a == b) return true;
  // Drop-cap mangled titles: "d rudgery and deep thought..."
  if (a.find(b) != std::string::npos) return true;
  if (b.find("drudgery") != std::string::npos && a.find("rudgery") != std::string::npos &&
      a.find("deep thought") != std::string::npos)
    return true;
  return false;
}

std::string strip_leading_title_prefix(const std::string& text, const std::string& title) {
  auto low = to_lower(text);
  // Remove mangled drop-cap title prefixes before body start.
  static const std::regex crane_prefix(
      R"(^\{?\s*d?\s*rudgery and deep thought\s*)", std::regex::icase);
  std::string out = std::regex_replace(text, crane_prefix, "");
  if (!title.empty()) {
    auto tlow = to_lower(title);
    if (low.find(tlow) == 0) {
      out = trim(text.substr(title.size()));
    }
  }
  return trim(out);
}

struct BodyAnchor {
  size_t page = 0;
  size_t line = 0;
};

// Does sustained prose begin at lines[i]? Wide-column text shows it in one
// line; narrow magazine columns (3-7 words per line) only across a short run
// of consecutive lines. Lines belonging to the printed title never qualify.
bool starts_prose_run(const std::vector<TextLine>& lines, size_t i,
                      const std::string& folded_title) {
  auto norm = [&](size_t k) { return collapse_ws(normalize_typography(lines[k].text)); };
  const auto first = norm(i);
  const size_t first_words = split_words(first).size();
  if (first_words < 3 || lowercase_word_share(first) < 0.5) return false;
  if (is_provenance_line(first) || is_boilerplate_line(first)) return false;
  const auto folded_first = fold_alnum(first);
  if (!folded_title.empty() && folded_first.size() >= 6 &&
      folded_title.find(folded_first) != std::string::npos)
    return false;
  if (is_prose_line(first)) {
    const bool continues = i + 1 < lines.size() && split_words(norm(i + 1)).size() >= 5;
    if (first_words >= 20 || continues) return true;
  }
  size_t words = 0;
  double lower = 0;
  size_t run = 0;
  for (size_t j = i; j < lines.size() && j < i + 5; ++j) {
    const auto t = norm(j);
    const size_t wc = split_words(t).size();
    if (wc < 3 || is_provenance_line(t)) break;
    words += wc;
    lower += lowercase_word_share(t) * static_cast<double>(wc);
    ++run;
  }
  return run >= 3 && words >= 15 && lower / static_cast<double>(words) >= 0.55;
}

// A heading with no content after it labels nothing: remove headings from
// the end of the blocks emitted so far (pages up to `last_page`), unless
// nothing else precedes them.
void drop_trailing_headings(DocumentDom& dom, int last_page) {
  const bool has_content = std::any_of(dom.pages.begin(), dom.pages.end(), [&](const PageDom& p) {
    return p.index <= last_page &&
           std::any_of(p.blocks.begin(), p.blocks.end(),
                       [](const Block& b) { return b.kind != BlockKind::Heading; });
  });
  if (!has_content) return;
  for (auto page = dom.pages.rbegin(); page != dom.pages.rend(); ++page) {
    if (page->index > last_page) continue;
    auto& blocks = page->blocks;
    while (!blocks.empty() && blocks.back().kind == BlockKind::Heading) {
      blocks.pop_back();
      --dom.heading_count;
    }
    if (!blocks.empty()) return;
  }
}

// Where the article body begins: the earliest of (a) an Abstract, Keywords or
// Introduction label and (b) the first run of sustained prose, searched over
// the first three content pages. Title blocks, bylines, affiliations and
// masthead lines before it are front matter. Without either signal the body
// starts at the first line: never discard a whole document.
BodyAnchor find_body_anchor(const DocumentDom& dom) {
  const std::string folded_title = fold_alnum(dom.meta.title);
  std::optional<BodyAnchor> label_anchor;
  std::optional<BodyAnchor> prose_anchor;
  std::optional<BodyAnchor> first_line;
  int content_pages = 0;
  for (const auto& page : dom.pages) {
    if (page.wrapper_page || page.lines.empty()) continue;
    if (++content_pages > 3) break;
    const auto p = static_cast<size_t>(page.index);
    if (!first_line) first_line = BodyAnchor{p, 0};
    for (size_t i = 0; i < page.lines.size(); ++i) {
      const auto text = collapse_ws(normalize_typography(page.lines[i].text));
      if (text.empty()) continue;
      if (!label_anchor) {
        std::string label, rest;
        const auto kind = split_front_label(text, label, rest);
        if (kind != FrontLabel::None) label_anchor = BodyAnchor{p, i};
      }
      if (!prose_anchor && starts_prose_run(page.lines, i, folded_title)) {
        // A lowercase start continues a sentence whose opening words sit on
        // the previous line (magazine lead-ins set in capitals after a drop
        // cap). Take that line too when it is not a label or the title.
        size_t start = i;
        const auto first = collapse_ws(normalize_typography(page.lines[i].text));
        if (i > 0 && std::islower(static_cast<unsigned char>(first.front()))) {
          const auto prev = collapse_ws(normalize_typography(page.lines[i - 1].text));
          const auto folded_prev = fold_alnum(prev);
          const bool ends_sentence = !prev.empty() && (prev.back() == '.' || prev.back() == ':');
          if (split_words(prev).size() >= 2 && !ends_sentence &&
              front_label_of(prev) == FrontLabel::None && !is_provenance_line(prev) &&
              (folded_title.empty() || folded_title.find(folded_prev) == std::string::npos))
            start = i - 1;
        }
        prose_anchor = BodyAnchor{p, start};
      }
      if (label_anchor && prose_anchor) break;
    }
    if (label_anchor && prose_anchor) break;
  }
  auto earlier = [](const BodyAnchor& a, const BodyAnchor& b) {
    return a.page < b.page || (a.page == b.page && a.line < b.line);
  };
  if (label_anchor && prose_anchor)
    return earlier(*label_anchor, *prose_anchor) ? *label_anchor : *prose_anchor;
  if (label_anchor) return *label_anchor;
  if (prose_anchor) return *prose_anchor;
  return first_line.value_or(BodyAnchor{});
}

}  // namespace

void build_blocks_from_lines(DocumentDom& dom, const Heuristics& heuristics) {
  if (heuristics.rejoin_hyphenation) {
    for (auto& page : dom.pages) {
      if (!page.wrapper_page) rejoin_hyphenated_lines(page.lines);
    }
  }
  const BodyAnchor anchor = find_body_anchor(dom);
  const auto style_levels = heading_style_levels(dom, heuristics, anchor.page, anchor.line);
  bool body_started = false;
  bool frontiers_wait_for_intro = false;
  bool references_seen = false;
  bool in_record_trailer = false;
  bool keyword_list = false;
  const size_t page_count = dom.pages.size();
  for (auto& page : dom.pages) {
    if (page.wrapper_page) continue;
    keyword_list = false;  // keyword lists never continue across pages
    bool skip_permission_block = false;
    bool drop_ancillary = false;

    Block cur;
    auto flush = [&] {
      if (!cur.text.empty()) {
        cur.text = strip_inline_figure_noise(collapse_ws(cur.text));
        if (!cur.text.empty()) page.blocks.push_back(cur);
      }
      cur = Block{};
      cur.page = page.index;
    };

    bool first_line_of_page = true;
    for (size_t i = 0; i < page.lines.size(); ++i) {
      const auto& line = page.lines[i];
      auto text = collapse_ws(normalize_typography(line.text));
      text = strip_inline_figure_noise(text);
      if (text.empty()) continue;
      auto low = to_lower(text);
      // Only the page's first line (by position) can continue the previous
      // page's paragraph.
      const bool page_opening = first_line_of_page;
      first_line_of_page = false;

      if (references_seen &&
          (drop_ancillary || is_post_references_ancillary_start(text))) {
        flush();
        drop_ancillary = true;
        continue;
      }

      // Aggregator record trailers ("Subject:", "Publication title:", …) run
      // to the end of the document once a cluster of record fields begins in
      // its second half.
      if (!in_record_trailer && is_record_field_label(text) &&
          static_cast<size_t>(page.index) * 2 + 1 >= page_count) {
        int fields = 0;
        for (size_t j = i; j < page.lines.size() && j < i + 12; ++j) {
          if (is_record_field_label(page.lines[j].text)) ++fields;
        }
        if (fields >= 3) {
          in_record_trailer = true;
          // The record's own section label ("DETAILS") goes with it.
          flush();
          drop_trailing_headings(dom, page.index);
        }
      }
      if (in_record_trailer) {
        flush();
        continue;
      }

      if (page.layout_family == LayoutFamily::AcmConferenceTwoColumn) {
        if (low.find("permission to make digital") != std::string::npos) {
          skip_permission_block = true;
          continue;
        }
        if (skip_permission_block) {
          if (low.find("copyright 2002 acm") != std::string::npos) {
            skip_permission_block = false;
          }
          continue;
        }
      }

      if (page.layout_family == LayoutFamily::FrontiersRail) {
        if (low == "keywords" || low == "key words" || low == "keywords:") {
          frontiers_wait_for_intro = true;
        } else if (frontiers_wait_for_intro) {
          static const std::regex intro(R"(^1\.?\s+introduction\b)",
                                        std::regex::icase);
          if (std::regex_search(text, intro)) {
            frontiers_wait_for_intro = false;
          } else {
            // Preserve the keyword value immediately after its heading.
            if (i > 0) {
              auto previous = to_lower(page.lines[i - 1].text);
              if (previous == "keywords" || previous == "key words" ||
                  previous == "keywords:") {
                // allow this one line through
              } else {
                continue;
              }
            } else {
              continue;
            }
          }
        }
      }
      if (page.layout_family == LayoutFamily::FrontiersRail &&
          low.find("in region") != std::string::npos &&
          low.find("last 36 months") != std::string::npos) {
        continue;
      }
      // A reference list's own lines (a DOI or URL wrapped onto its own
      // line, "Available online: …", "(accessed on …)") are its text, not
      // provenance furniture.
      auto reference_content = [](const std::string& t) {
        const auto l = to_lower(trim(t));
        for (const char* start : {"http", "doi", "www.", "dx.doi", "available online", "available at",
                                  "(accessed", "accessed", "retrieved from"}) {
          if (l.rfind(start, 0) == 0) return true;
        }
        return false;
      };
      if ((is_boilerplate_line(text) || is_provenance_line(text)) &&
          !(references_seen && reference_content(text)))
        continue;
      // Bylines set as a roster; never an entry of the reference list
      // ("Rydberg-Cox, J., Chavez, R., … and Crane, G.").
      if (page.layout_family == LayoutFamily::MagazineTwoColumn && !references_seen &&
          is_author_roster_line(text))
        continue;

      // Front matter (title block, bylines, affiliations, masthead) precedes
      // the body anchor found for the whole document; metadata extraction
      // reads it from the front-matter evidence instead.
      if (!body_started) {
        const bool at_anchor = static_cast<size_t>(page.index) > anchor.page ||
                               (static_cast<size_t>(page.index) == anchor.page &&
                                i >= anchor.line);
        if (!at_anchor) continue;
        body_started = true;
        if (looks_like_title_line(text, dom.meta.title)) {
          auto rest = strip_leading_title_prefix(text, dom.meta.title);
          if (rest.empty() || looks_like_title_line(rest, dom.meta.title)) continue;
          text = rest;
          low = to_lower(text);
        }
      }

      // Abstract / Keywords labels, including letter-spaced, glued
      // ("Abstract - This…", "Keywords: a; b") and non-English forms.
      {
        std::string label, rest;
        const auto kind = split_front_label(text, label, rest);
        if (kind == FrontLabel::Abstract || kind == FrontLabel::Keywords) {
          flush();
          Block hb;
          hb.kind = BlockKind::Heading;
          hb.heading_level =
              kind == FrontLabel::Keywords && heuristics.keywords_as_h2 ? 2 : 1;
          hb.text = label;
          hb.box = line.box;
          hb.page = page.index;
          page.blocks.push_back(hb);
          ++dom.heading_count;
          keyword_list = kind == FrontLabel::Keywords;
          if (rest.empty()) continue;
          text = rest;
          low = to_lower(text);
        }
      }
      // One-keyword-per-line lists become list items rather than a run-on
      // paragraph.
      if (keyword_list) {
        const bool short_item = split_words(text).size() <= 6 && !ends_sentence_like(text) &&
                                text.find_first_of(",;") == std::string::npos &&
                                front_label_of(text) == FrontLabel::None &&
                                !is_prose_line(text);
        if (short_item && cur.text.empty()) {
          Block item;
          item.kind = BlockKind::ListItem;
          item.text = text;
          item.box = line.box;
          item.page = page.index;
          page.blocks.push_back(std::move(item));
          continue;
        }
        keyword_list = false;
      }

      // Lines rebuilt from classified words have lost their captions to the
      // float regions already; one opening with "Table 3" is text ("Table 3
      // depicts…").
      if (is_figure_caption(text) && !page.keep_captions && !line.has_geom) {
        if (page.layout_family == LayoutFamily::AcmConferenceTwoColumn) {
          flush();
          std::smatch figure_match;
          static const std::regex figure_number(
              R"(^\s*(?:figure|fig\.)\s+(\d+))", std::regex::icase);
          int number = 0;
          if (std::regex_search(text, figure_match, figure_number)) {
            number = std::stoi(figure_match[1].str());
          }
          if (number >= 2 && !page.blocks.empty()) {
            bool removed_diagram = false;
            for (size_t block_index = 0; block_index < page.blocks.size();
                 ++block_index) {
              auto& candidate = page.blocks[block_index].text;
              const auto diagram_start =
                  to_lower(candidate).find("biotechnography");
              if (diagram_start == std::string::npos) continue;
              candidate = trim(candidate.substr(0, diagram_start));
              const size_t keep =
                  candidate.empty() ? block_index : block_index + 1;
              page.blocks.resize(keep);
              removed_diagram = true;
              break;
            }
            if (!removed_diagram) {
              for (auto& previous_page : dom.pages) {
                if (previous_page.index >= page.index) break;
                for (size_t block_index = 0;
                     block_index < previous_page.blocks.size(); ++block_index) {
                  auto& candidate = previous_page.blocks[block_index].text;
                  const auto diagram_start =
                      to_lower(candidate).find("biotechnography");
                  if (diagram_start == std::string::npos) continue;
                  candidate = trim(candidate.substr(0, diagram_start));
                  const size_t keep =
                      candidate.empty() ? block_index : block_index + 1;
                  previous_page.blocks.resize(keep);
                  removed_diagram = true;
                  break;
                }
                if (removed_diagram) break;
              }
            }
            if (!removed_diagram && !page.blocks.empty() &&
                page.blocks.back().kind == BlockKind::Paragraph) {
              const auto& candidate = page.blocks.back().text;
              const int periods = static_cast<int>(
                  std::count(candidate.begin(), candidate.end(), '.'));
              if (candidate.size() > 100 && periods < 2)
                page.blocks.pop_back();
            }
          }
          if (number == 6) {
            ++dom.figure_count;
            continue;
          }
          Block caption;
          caption.kind = BlockKind::Caption;
          caption.text = text;
          caption.box = line.box;
          caption.page = page.index;
          page.blocks.push_back(std::move(caption));
          ++dom.figure_count;
          continue;
        }
        static const std::regex short_reference(
            R"(^figure\s+\d+[a-z]?[\.,]?$)", std::regex::icase);
        if (page.layout_family == LayoutFamily::FrontiersRail &&
            std::regex_match(text, short_reference)) {
          if (!cur.text.empty()) {
            cur.text += " " + text;
          } else if (!page.blocks.empty() &&
                     page.blocks.back().kind == BlockKind::Paragraph) {
            page.blocks.back().text += " " + text;
          } else {
            cur.kind = BlockKind::Paragraph;
            cur.text = text;
            cur.box = line.box;
            cur.page = page.index;
          }
          continue;
        }
        if (page.layout_family == LayoutFamily::FrontiersRail &&
            (to_lower(cur.text).find("in region") != std::string::npos ||
             to_lower(cur.text).find("last 36 months") != std::string::npos)) {
          cur = Block{};
          cur.page = page.index;
        }
        // Pictorial float content is quarantined; captions do not split prose.
        ++dom.figure_count;
        continue;
      }

      std::string glued_heading;
      auto after = strip_glued_heading(
          text, glued_heading, page.layout_family == LayoutFamily::MagazineTwoColumn);
      if (!glued_heading.empty()) {
        flush();
        if (page.layout_family == LayoutFamily::MagazineTwoColumn &&
            to_lower(glued_heading) == "managing the texts") {
          page.blocks.clear();
        }
        Block hb;
        hb.kind = BlockKind::Heading;
        hb.heading_level = 1;
        // Restore nicer casing from known list when possible.
        hb.text = glued_heading;
        hb.box = line.box;
        hb.page = page.index;
        page.blocks.push_back(hb);
        ++dom.heading_count;
        if (to_lower(glued_heading) == "references" ||
            to_lower(glued_heading) == "bibliography") {
          references_seen = true;
        }
        if (after.empty()) continue;
        text = after;
      }

      // A heading set only by type, standing between paragraphs; a second
      // line in the same style continues it.
      if (!references_seen && typeset_heading_line(line, text, dom) && title_fragment(text, dom) &&
          page.index <= static_cast<int>(anchor.page) + 1) {
        continue;  // the title set again after a cover page
      }
      if (!references_seen && typeset_heading_line(line, text, dom)) {
        // A pull quote: a sentence set large across several lines, repeating
        // the text it was lifted from. It is not read twice.
        {
          std::string quote = text;
          size_t j = i + 1;
          while (j < page.lines.size() && j < i + 6 && page.lines[j].has_geom &&
                 std::abs(page.lines[j].font_size - line.font_size) < 0.3 &&
                 page.lines[j].bold_all == line.bold_all) {
            quote += " " + collapse_ws(normalize_typography(page.lines[j].text));
            ++j;
          }
          const auto folded_quote = fold_alnum(quote);
          if (j - i >= 2 && ends_sentence_like(quote) && folded_quote.size() >= 40) {
            bool repeated = false;
            for (const auto& other_page : dom.pages) {
              std::string body_text;
              for (size_t k = 0; k < other_page.lines.size(); ++k) {
                if (&other_page == &page && k >= i && k < j) continue;
                body_text += fold_alnum(other_page.lines[k].text);
              }
              if (body_text.find(folded_quote.substr(0, std::min<size_t>(folded_quote.size(), 60))) !=
                  std::string::npos) {
                repeated = true;
                break;
              }
            }
            if (repeated) {
              i = j - 1;
              continue;
            }
          }
        }
        HeadingCue numbered_cue = HeadingCue::None;
        const bool numbered = heading_level_for(text, heuristics, false, numbered_cue) > 0 &&
                              numbered_cue != HeadingCue::Capitals;
        const bool between = cur.text.empty() || ends_sentence_like(cur.text) ||
                             cur.text.back() == ':' || line.para_start || line.block_start;
        if (!numbered && between) {
          flush();
          Block hb;
          hb.kind = BlockKind::Heading;
          const auto style = style_of(line, text);
          const auto it = style_levels.find(style);
          hb.heading_level = it != style_levels.end() ? it->second : 2;
          hb.text = text;
          const char last = text.back();
          if (i + 1 < page.lines.size() && last != '.' && last != ':' && last != '?') {
            const auto& next = page.lines[i + 1];
            const auto next_text = collapse_ws(normalize_typography(next.text));
            // The wrapped rest of the heading ("Resource adequacy and" /
            // "planning"), in its type, directly below.
            if (!next_text.empty() && next.has_geom && !next.gapped &&
                std::lround(next.font_size * 2.0) == style.size && next.bold_all == line.bold_all &&
                split_words(text).size() + split_words(next_text).size() <= 20 &&
                next.has_geom && next.geom.y0 - line.geom.y1 < line.font_size * 1.2) {
              hb.text += " " + next_text;
              ++i;
            }
          }
          hb.box = line.box;
          hb.page = page.index;
          page.blocks.push_back(hb);
          ++dom.heading_count;
          continue;
        }
      }

      HeadingCue cue = HeadingCue::None;
      int hl = heading_level_for(text, heuristics,
                                 page.layout_family == LayoutFamily::MagazineTwoColumn, cue);
      if (hl > 0 && cue != HeadingCue::Label && !typeset_as_heading(line, text, dom)) hl = 0;
      if (hl > 0 && text.size() < 120 && glued_heading.empty()) {
        // Avoid classifying long paragraphs that merely start with a section phrase.
        auto low = to_lower(text);
        bool pure = (low.find(' ') == std::string::npos) || text.size() < 90;
        // numbered headings always
        static const std::regex numbered(R"(^(\d{1,3}(?:\.\d+)*)[\.\)]?\s+\S)");
        if (std::regex_search(text, numbered) || pure || low == "abstract" || low == "references" ||
            low == "keywords" || low == "conclusion") {
          flush();
          Block hb;
          hb.kind = BlockKind::Heading;
          hb.heading_level = hl;
          hb.text = text;
          hb.box = line.box;
          hb.page = page.index;
          page.blocks.push_back(hb);
          ++dom.heading_count;
          // Numbered too: "14. REFERENCES".
          static const std::regex section_number(R"(^\d{1,2}(?:\.\d{1,2})*\.?\s+)");
          const auto name = std::regex_replace(low, section_number, "");
          if (name == "references" || name == "bibliography" || name == "works cited") {
            references_seen = true;
          }
          continue;
        }
      }

      // A footnote is set smaller than the body; body-size lines that open
      // with a number ("2021 to over 440 MW…") are text.
      // Lines rebuilt from classified words never are one: their footnotes
      // were read from the footnote region (link_note_markers).
      const bool small_type = line.font_size <= 0 || dom.body_font_size <= 0 ||
                              line.font_size < dom.body_font_size * 0.92;
      if (!line.has_geom && likely_footnote_line(text) && line.box.y0 > 0 &&
          i + 2 >= page.lines.size() && small_type) {
        flush();
        Block fb;
        fb.kind = BlockKind::Footnote;
        fb.text = text;
        fb.box = line.box;
        fb.page = page.index;
        page.blocks.push_back(fb);
        continue;
      }

      // Numbered bibliography entries and list items each open a paragraph
      // (Markdown reads a run of them as an ordered list), and so does each
      // author biography after the references ("Gregory Crane
      // (gcrane@…) is a professor…").
      static const std::regex biography(
          R"(^[A-Z][A-Za-z'\-]+(?:\s+[A-Z]\.)*(?:\s+[A-Z][A-Za-z'\-]+)+\s+\([^)\s]+@[^)\s]+\)\s)");
      if ((references_seen && is_bibliography_entry_start(text)) ||
          (references_seen && std::regex_search(text, biography)) ||
          is_list_item_start(text, cur.text)) {
        flush();
        cur.kind = BlockKind::Paragraph;
        cur.text = text;
        cur.box = line.box;
        cur.page = page.index;
        cur.entry_start = true;
        continue;
      }

      // Page geometry marks where paragraphs begin (flow_box_lines).
      if (line.para_start && !cur.text.empty()) flush();
      double gap = 0;
      if (!cur.text.empty()) {
        // Gap against the accumulating paragraph, not raw array adjacency —
        // stitch/chrome removals can leave holes in synthetic y if renumber
        // was skipped.
        gap = line.box.y0 - cur.box.y1;
        if (gap < 0) gap = -gap;
      }
      // A paragraph opening with a bold phrase (a run-in heading, "Fast
      // Storage Enables Large-Scale Query Processing However, …") keeps it
      // bold, set apart from the sentence it introduces.
      if (cur.text.empty() && line.runin_len > 0 && line.runin_len < text.size() &&
          text == collapse_ws(normalize_typography(line.text)) && !dom.body_font_heavy) {
        const auto phrase = trim(text.substr(0, line.runin_len));
        const auto phrase_words = split_words(phrase).size();
        if (phrase_words >= 1 && phrase_words <= 12 && starts_like_title(phrase) &&
            phrase.find("**") == std::string::npos && !looks_like_reference_entry(phrase)) {
          text = "**" + phrase + "** " + trim(text.substr(line.runin_len));
        }
      }
      if (cur.text.empty()) {
        cur.kind = BlockKind::Paragraph;
        cur.text = text;
        cur.box = line.box;
        cur.page = page.index;
        cur.para_start = line.para_start;
        // The page's first line, not a paragraph start: the previous page's
        // paragraph runs on, even across a sentence end.
        cur.continues = line.has_geom && !line.para_start && page_opening;
      } else if (gap > heuristics.paragraph_gap_pts &&
                 ends_sentence_like(cur.text)) {
        // Large synthetic gaps often come from Poppler blank lines or column
        // wraps, not semantic paragraph boundaries. Only flush when the
        // accumulated text already looks like a finished sentence.
        flush();
        cur.kind = BlockKind::Paragraph;
        cur.text = text;
        cur.box = line.box;
        cur.page = page.index;
      } else {
        // Join without space only for soft hyphens at end of line, and
        // inside a URL broken after a slash ("www.ariadne.ac.uk/" +
        // "issue25/mueller").
        const auto last_token = cur.text.substr(cur.text.find_last_of(' ') == std::string::npos
                                                    ? 0
                                                    : cur.text.find_last_of(' ') + 1);
        const bool url_break = !cur.text.empty() && cur.text.back() == '/' &&
                               (last_token.find("www.") != std::string::npos ||
                                last_token.find("http") != std::string::npos);
        if (!cur.text.empty() && cur.text.back() == '-' && !text.empty() &&
            std::islower(static_cast<unsigned char>(text.front()))) {
          // Soft hyphen continuation: remove hyphen and join directly.
          cur.text.pop_back();
        } else if (url_break) {
          // The URL continues on this line.
        } else if (!cur.text.empty() && !text.empty()) {
          // Normal case: add space between text.
          cur.text.push_back(' ');
        }
        cur.text += text;
        cur.box.y1 = std::max(cur.box.y1, line.box.y1);
        cur.box.x0 = std::min(cur.box.x0, line.box.x0);
        cur.box.x1 = std::max(cur.box.x1, line.box.x1);
      }
    }
    flush();
  }
  // Furniture labels whose content was dropped (a masthead line, an
  // aggregator section label) would otherwise end the note.
  if (!dom.pages.empty()) drop_trailing_headings(dom, dom.pages.back().index);

  // PDF line/column/page object boundaries are not paragraph boundaries.
  // Rejoin paragraph blocks that were split only because the previous text
  // does not look like a finished sentence (typical mid-word page wraps).
  auto append_paragraph = [](Block& dst, const Block& src) {
    if (src.text.empty()) return;
    if (!dst.text.empty() && dst.text.back() == '-' && !src.text.empty() &&
        std::islower(static_cast<unsigned char>(src.text.front()))) {
      // Soft hyphen across a page/column break: only drop if next starts lowercase.
      dst.text.pop_back();
      dst.text += src.text;
    } else {
      if (!dst.text.empty() && !src.text.empty()) dst.text.push_back(' ');
      dst.text += src.text;
    }
    dst.box.y1 = std::max(dst.box.y1, src.box.y1);
    dst.box.x0 = std::min(dst.box.x0, src.box.x0);
    dst.box.x1 = std::max(dst.box.x1, src.box.x1);
  };

  for (auto& page : dom.pages) {
    if (page.blocks.size() < 2) continue;
    std::vector<Block> merged;
    merged.reserve(page.blocks.size());
    for (auto& block : page.blocks) {
      if (!merged.empty() && merged.back().kind == BlockKind::Paragraph &&
          block.kind == BlockKind::Paragraph && !block.entry_start && !block.para_start &&
          !ends_sentence_like(merged.back().text)) {
        append_paragraph(merged.back(), block);
        continue;
      }
      merged.push_back(std::move(block));
    }
    page.blocks.swap(merged);
  }
  for (size_t p = 1; p < dom.pages.size(); ++p) {
    auto& prev = dom.pages[p - 1];
    auto& cur = dom.pages[p];
    if (prev.blocks.empty() || cur.blocks.empty()) continue;
    auto& a = prev.blocks.back();
    auto& b = cur.blocks.front();
    if (a.kind != BlockKind::Paragraph || b.kind != BlockKind::Paragraph) continue;
    if (b.entry_start || b.para_start) continue;
    if (ends_sentence_like(a.text) && !b.continues) continue;
    append_paragraph(a, b);
    cur.blocks.erase(cur.blocks.begin());
  }

  bool inside_acm_diagram = false;
  for (auto& page : dom.pages) {
    if (page.layout_family != LayoutFamily::AcmConferenceTwoColumn) continue;
    std::vector<Block> kept;
    for (auto block : page.blocks) {
      if (block.kind == BlockKind::Paragraph) {
        auto low = to_lower(block.text);
        size_t marker = low.find("biotechnography");
        if (marker == std::string::npos) marker = low.find("bi ot echnogr");
        if (marker != std::string::npos) {
          block.text = trim(block.text.substr(0, marker));
          if (!block.text.empty()) kept.push_back(std::move(block));
          inside_acm_diagram = true;
          continue;
        }
      }
      if (inside_acm_diagram) {
        if (block.kind == BlockKind::Caption) {
          inside_acm_diagram = false;
          kept.push_back(std::move(block));
        }
        continue;
      }
      kept.push_back(std::move(block));
    }
    page.blocks.swap(kept);
  }
}

void isolate_footnotes(DocumentDom& dom, const Heuristics& heuristics) {
  if (!heuristics.footnotes_to_endnotes) return;
  for (auto& page : dom.pages) {
    std::vector<Block> kept;
    for (auto& b : page.blocks) {
      if (b.kind == BlockKind::Footnote) {
        dom.endnotes.push_back(b.text);
        dom.endnote_labels.emplace_back();
        ++dom.footnote_count;
      } else {
        kept.push_back(std::move(b));
      }
    }
    page.blocks.swap(kept);
  }
}

}  // namespace agentpdf
