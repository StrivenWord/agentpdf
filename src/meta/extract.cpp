#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <regex>

namespace agentpdf {

namespace {

std::string extract_doi_from_text(const std::string& text) {
  static const std::regex doi_re(R"((10\.\d{4,9}/[-._;()/:A-Z0-9]+))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(text, m, doi_re)) {
    std::string d = m[1];
    while (!d.empty() && (d.back() == '.' || d.back() == ',')) d.pop_back();
    return d;
  }
  return {};
}

}  // namespace

void extract_and_validate_metadata(DocumentDom& dom, const MetadataSpec& /*spec*/) {
  // Collect early page text for title/abstract/doi heuristics.
  std::string early;
  for (size_t pi = 0; pi < dom.pages.size() && pi < 2; ++pi) {
    for (const auto& line : dom.pages[pi].lines) {
      early += line.text;
      early.push_back('\n');
    }
  }

  if (dom.meta.doi.empty()) {
    dom.meta.doi = extract_doi_from_text(early);
  }
  if (dom.meta.object_url.empty() && !dom.meta.doi.empty()) {
    dom.meta.object_url = "https://doi.org/" + dom.meta.doi;
  }

  // Title: prefer PDF info; else first large heading-like line that isn't ABSTRACT.
  if (dom.meta.title.empty()) {
    for (const auto& page : dom.pages) {
      for (const auto& b : page.blocks) {
        if (b.kind == BlockKind::Heading) {
          auto low = to_lower(b.text);
          if (low == "abstract" || low == "keywords" || low == "references") continue;
          if (b.text.size() >= 8) {
            dom.meta.title = b.text;
            break;
          }
        }
      }
      if (!dom.meta.title.empty()) break;
    }
  }

  // Abstract body: paragraph after Abstract heading — require substantive prose.
  for (const auto& page : dom.pages) {
    bool after_abs = false;
    for (const auto& b : page.blocks) {
      if (b.kind == BlockKind::Heading && to_lower(b.text) == "abstract") {
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
      if (after_abs && b.kind == BlockKind::Heading) break;
    }
    if (!dom.meta.abstract_text.empty()) break;
  }

  // Keywords line after Keywords heading.
  for (const auto& page : dom.pages) {
    bool after_kw = false;
    for (const auto& b : page.blocks) {
      auto low = to_lower(b.text);
      if (b.kind == BlockKind::Heading && (low == "keywords" || low == "key words")) {
        after_kw = true;
        continue;
      }
      if (after_kw && (b.kind == BlockKind::Paragraph || b.kind == BlockKind::Heading)) {
        // split on commas
        std::string s = b.text;
        size_t start = 0;
        while (start < s.size()) {
          size_t pos = s.find(',', start);
          if (pos == std::string::npos) pos = s.size();
          auto part = trim(s.substr(start, pos - start));
          if (!part.empty() && part.size() < 80) dom.meta.keywords.push_back(part);
          start = pos + 1;
        }
        break;
      }
    }
    if (!dom.meta.keywords.empty()) break;
  }

  if (dom.meta.date_extracted.empty()) dom.meta.date_extracted = today_iso_date();
  if (dom.meta.source_format.empty()) dom.meta.source_format = "PDF";
}

}  // namespace agentpdf
