#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <sstream>

namespace agentpdf {

namespace {

std::string heading_prefix(int level) {
  level = std::max(1, std::min(level, 6));
  return std::string(static_cast<size_t>(level), '#') + " ";
}

std::string pad_table(const std::vector<std::vector<std::string>>& rows) {
  if (rows.empty()) return {};
  size_t cols = 0;
  for (const auto& r : rows) cols = std::max(cols, r.size());
  std::vector<size_t> widths(cols, 0);
  for (const auto& r : rows) {
    for (size_t c = 0; c < r.size(); ++c) widths[c] = std::max(widths[c], r[c].size());
  }
  auto fmt_row = [&](const std::vector<std::string>& r) {
    std::ostringstream oss;
    oss << "|";
    for (size_t c = 0; c < cols; ++c) {
      std::string cell = c < r.size() ? r[c] : "";
      oss << " " << cell << std::string(widths[c] > cell.size() ? widths[c] - cell.size() : 0, ' ')
          << " |";
    }
    return oss.str();
  };
  std::ostringstream out;
  out << fmt_row(rows[0]) << "\n|";
  for (size_t c = 0; c < cols; ++c) {
    out << " " << std::string(std::max<size_t>(3, widths[c]), '-') << " |";
  }
  out << "\n";
  for (size_t i = 1; i < rows.size(); ++i) out << fmt_row(rows[i]) << "\n";
  return out.str();
}

bool is_title_duplicate(const std::string& block, const std::string& title) {
  if (title.empty() || block.empty()) return false;
  auto a = to_lower(collapse_ws(block));
  auto b = to_lower(collapse_ws(title));
  return a == b;
}

}  // namespace

std::string assemble_markdown(const DocumentDom& dom, const Heuristics& heuristics) {
  std::ostringstream yaml;
  yaml << "---\n";
  yaml << "title: " << yaml_escape(dom.meta.title) << "\n";
  yaml << "authors:\n";
  if (dom.meta.authors.empty()) {
    yaml << "  []\n";
  } else {
    for (const auto& a : dom.meta.authors) yaml << "  - " << yaml_escape(a) << "\n";
  }
  if (!dom.meta.date_published.empty())
    yaml << "date-published: " << yaml_escape(dom.meta.date_published) << "\n";
  if (!dom.meta.date_received.empty())
    yaml << "date-received: " << yaml_escape(dom.meta.date_received) << "\n";
  if (!dom.meta.date_accepted.empty())
    yaml << "date-accepted: " << yaml_escape(dom.meta.date_accepted) << "\n";
  if (!dom.meta.publisher.empty())
    yaml << "publisher: " << yaml_escape(dom.meta.publisher) << "\n";
  if (!dom.meta.object_url.empty())
    yaml << "object-url: " << yaml_escape(dom.meta.object_url) << "\n";
  yaml << "source-format: " << yaml_escape(dom.meta.source_format) << "\n";
  if (!dom.meta.published_formats.empty())
    yaml << "published-formats: " << yaml_escape(dom.meta.published_formats) << "\n";
  if (!dom.meta.doi.empty()) yaml << "doi: " << yaml_escape(dom.meta.doi) << "\n";
  if (!dom.meta.type.empty()) yaml << "type: " << yaml_escape(dom.meta.type) << "\n";
  if (!dom.meta.pages.empty()) yaml << "pages: " << yaml_escape(dom.meta.pages) << "\n";
  if (!dom.meta.date_accessed.empty())
    yaml << "date-accessed: " << yaml_escape(dom.meta.date_accessed) << "\n";
  if (!dom.meta.date_extracted.empty())
    yaml << "date-extracted: " << yaml_escape(dom.meta.date_extracted) << "\n";
  if (!dom.meta.keywords.empty()) {
    yaml << "keywords:\n";
    for (const auto& k : dom.meta.keywords) yaml << "  - " << yaml_escape(k) << "\n";
  }
  if (!dom.meta.abstract_text.empty()) {
    yaml << "abstract: |\n";
    std::istringstream iss(dom.meta.abstract_text);
    std::string line;
    while (std::getline(iss, line)) yaml << "  " << line << "\n";
  }
  yaml << "---\n\n";

  std::ostringstream body;
  bool seen_refs = false;
  for (const auto& page : dom.pages) {
    for (const auto& b : page.blocks) {
      if (heuristics.dedupe_title_from_body && is_title_duplicate(b.text, dom.meta.title)) {
        continue;
      }
      if (b.kind == BlockKind::Heading) {
        int level = b.heading_level;
        auto low = to_lower(b.text);
        if (heuristics.abstract_as_h1 && low == "abstract") level = 1;
        if (heuristics.keywords_as_h2 && (low == "keywords" || low == "key words")) level = 2;
        if (low == "references") seen_refs = true;
        body << heading_prefix(level) << b.text << "\n\n";
      } else if (b.kind == BlockKind::Table) {
        body << pad_table(b.table_rows) << "\n";
      } else if (b.kind == BlockKind::Caption || b.kind == BlockKind::FigureRedaction) {
        body << "**" << b.text << "**\n\n";
      } else if (b.kind == BlockKind::ListItem) {
        body << "- " << b.text << "\n";
      } else if (b.kind == BlockKind::Paragraph) {
        body << b.text << "\n\n";
      }
    }
  }

  if (!dom.endnotes.empty()) {
    if (!seen_refs) body << "\n";
    body << "\n";
    for (size_t i = 0; i < dom.endnotes.size(); ++i) {
      body << "[^" << (i + 1) << "]: " << dom.endnotes[i] << "\n\n";
    }
  }

  return yaml.str() + body.str();
}

}  // namespace agentpdf
