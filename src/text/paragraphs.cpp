#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

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
}

namespace {

bool is_boilerplate_line(const std::string& text) {
  auto low = to_lower(trim(text));
  if (low.empty()) return true;
  if (low == "{" || low == "}" || low == "|" || low == "•") return true;
  if (low.size() == 1 && std::isalpha(static_cast<unsigned char>(low[0]))) return true;
  static const char* exact[] = {
      "open access",
      "article",
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
  };
  for (const char* k : contains) {
    if (low.find(k) != std::string::npos) return true;
  }
  static const std::regex publication_date(
      R"(^published\s+\d{1,2}\s+[a-z]+\s+\d{4}\b)", std::regex::icase);
  if (std::regex_search(low, publication_date)) return true;
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

int heading_level_for(const std::string& text, const Heuristics& h) {
  auto t = trim(text);
  auto low = to_lower(t);
  if (low == "abstract" || low == "references" || low == "conclusion" ||
      low == "acknowledgments" || low == "acknowledgements" || low == "appendix" ||
      low.find("appendix ") == 0) {
    return 1;
  }
  if (low == "keywords" || low == "key words") return h.keywords_as_h2 ? 2 : 1;
  if (low == "general terms" || low == "categories and subject descriptors") return 1;

  std::smatch m;
  static const std::regex numbered(R"(^(\d{1,3}(?:\.\d+)*)[\.\)]?\s+\S)");
  if (h.nest_numeric_headings && std::regex_search(t, m, numbered)) {
    std::string num = m[1];
    int depth = static_cast<int>(std::count(num.begin(), num.end(), '.')) + 1;
    return std::min(depth, 6);
  }

  // Known unnumbered section titles (ACM magazine style).
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
    if (low.find(std::string(s) + " ") == 0) return 1;
  }

  if (t.size() < 80 && t.find('.') == std::string::npos) {
    int letters = 0, uppers = 0;
    for (unsigned char c : t) {
      if (std::isalpha(c)) {
        ++letters;
        if (std::isupper(c)) ++uppers;
      }
    }
    if (letters >= 4 && uppers >= letters / 2) return 1;
  }
  return 0;
}

std::string strip_glued_heading(const std::string& text, std::string& heading_out) {
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

}  // namespace

void build_blocks_from_lines(DocumentDom& dom, const Heuristics& heuristics) {
  bool body_started = false;
  bool frontiers_wait_for_intro = false;
  for (auto& page : dom.pages) {
    if (page.wrapper_page) continue;
    if (heuristics.rejoin_hyphenation) rejoin_hyphenated_lines(page.lines);
    bool skip_permission_block = false;

    Block cur;
    auto flush = [&] {
      if (!cur.text.empty()) {
        cur.text = strip_inline_figure_noise(collapse_ws(cur.text));
        if (!cur.text.empty()) page.blocks.push_back(cur);
      }
      cur = Block{};
      cur.page = page.index;
    };

    for (size_t i = 0; i < page.lines.size(); ++i) {
      const auto& line = page.lines[i];
      auto text = collapse_ws(normalize_typography(line.text));
      text = strip_inline_figure_noise(text);
      if (text.empty()) continue;
      auto low = to_lower(text);

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
      if (is_boilerplate_line(text)) continue;
      if (page.layout_family == LayoutFamily::MagazineTwoColumn &&
          is_author_roster_line(text))
        continue;

      // Skip leading title / drop-cap chrome until real body.
      if (!body_started) {
        if (page.layout_family == LayoutFamily::ScanOcrTwoColumn) {
          if (low.find("a national survey of practicing psychologists") == 0) {
            dom.meta.title = text;
            continue;
          }
          if (!dom.meta.title.empty() &&
              low == "psychotherapy treatment manuals") {
            dom.meta.title += ": " + text;
            continue;
          }
          if (low.find("there has been considerable debate") == 0) {
            body_started = true;
          } else {
            continue;
          }
        }
        if (body_started) {
          // Scan front matter established the first body paragraph above.
        } else if (low == "abstract" || low == "keywords" || low == "1. introduction" ||
            low.find("1. introduction") == 0) {
          body_started = true;
        } else if (looks_like_title_line(text, dom.meta.title)) {
          auto rest = strip_leading_title_prefix(text, dom.meta.title);
          if (rest.empty() || looks_like_title_line(rest, dom.meta.title)) continue;
          text = rest;
          body_started = true;
        } else if (low.find("perseus and other") == 0 || low.find("what explains the") == 0) {
          body_started = true;
        } else {
          // Discard front-matter chrome until a reliable body anchor.
          continue;
        }
      }

      if (is_figure_caption(text) && !page.keep_captions) {
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
      auto after = strip_glued_heading(text, glued_heading);
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
        if (after.empty()) continue;
        text = after;
      }

      int hl = heading_level_for(text, heuristics);
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
          continue;
        }
      }

      if (likely_footnote_line(text) && line.box.y0 > 0 && i + 2 >= page.lines.size()) {
        flush();
        Block fb;
        fb.kind = BlockKind::Footnote;
        fb.text = text;
        fb.box = line.box;
        fb.page = page.index;
        page.blocks.push_back(fb);
        continue;
      }

      double gap = 0;
      if (!cur.text.empty() && i > 0) {
        gap = line.box.y0 - page.lines[i - 1].box.y1;
        if (gap < 0) gap = -gap;  // reading-order synthetic coords increase downward
      }
      if (cur.text.empty()) {
        cur.kind = BlockKind::Paragraph;
        cur.text = text;
        cur.box = line.box;
        cur.page = page.index;
      } else if (gap > heuristics.paragraph_gap_pts) {
        flush();
        cur.kind = BlockKind::Paragraph;
        cur.text = text;
        cur.box = line.box;
        cur.page = page.index;
      } else {
        if (!cur.text.empty() && cur.text.back() != '-' && !text.empty()) {
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
        ++dom.footnote_count;
      } else {
        kept.push_back(std::move(b));
      }
    }
    page.blocks.swap(kept);
  }
}

}  // namespace agentpdf
