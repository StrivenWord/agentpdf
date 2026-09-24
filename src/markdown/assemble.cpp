#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

namespace agentpdf {

namespace {

std::string fix_missing_compound_hyphens(const std::string& text) {
  // Fix known compound words that lost hyphens during extraction.
  std::string out = text;
  // Known patterns where space or hyphen was lost between adjacent words.
  static const std::vector<std::pair<std::string, std::string>> replacements = {
      {"crosslanguage", "cross-language"},
      {"modernlanguage", "modern-language"},
      {"wellstructured", "well-structured"},
      {"GrecoRoman", "Greco-Roman"},
  };
  for (const auto& [bad, good] : replacements) {
    size_t pos = 0;
    while ((pos = out.find(bad, pos)) != std::string::npos) {
      out.replace(pos, bad.length(), good);
      pos += good.length();
    }
  }

  // Fix URLs split by spaces after periods (e.g., "www.perseus. tufts.edu" -> "www.perseus.tufts.edu")
  static const std::regex url_split(R"(www\.\s*([a-z0-9]+)\.\s+([a-z0-9\.]+))");
  out = std::regex_replace(out, url_split, "www.$1.$2");

  return out;
}

std::string heading_prefix(int level) {
  level = std::max(1, std::min(level, 6));
  return std::string(static_cast<size_t>(level), '#') + " ";
}

// Display width of a cell: its characters, not its bytes (UTF-8).
size_t cell_width(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? 1 : 0;
  return n;
}

// A cell's text in a table row: pipes escaped, spacing collapsed.
std::string table_cell(const std::string& raw) {
  std::string out;
  for (char c : collapse_ws(raw)) {
    if (c == '|') out += "\\|";
    else out.push_back(c);
  }
  return out;
}

std::string pad_table(const std::vector<std::vector<std::string>>& raw_rows) {
  if (raw_rows.empty()) return {};
  std::vector<std::vector<std::string>> rows;
  for (const auto& r : raw_rows) {
    std::vector<std::string> cells;
    for (const auto& c : r) cells.push_back(table_cell(c));
    rows.push_back(std::move(cells));
  }
  size_t cols = 0;
  for (const auto& r : rows) cols = std::max(cols, r.size());
  std::vector<size_t> widths(cols, 0);
  for (const auto& r : rows) {
    for (size_t c = 0; c < r.size(); ++c) widths[c] = std::max(widths[c], cell_width(r[c]));
  }
  auto fmt_row = [&](const std::vector<std::string>& r) {
    std::ostringstream oss;
    oss << "|";
    for (size_t c = 0; c < cols; ++c) {
      std::string cell = c < r.size() ? r[c] : "";
      const size_t w = cell_width(cell);
      oss << " " << cell << std::string(widths[c] > w ? widths[c] - w : 0, ' ') << " |";
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

std::string assemble_markdown(const DocumentDom& dom, const Heuristics& heuristics,
                              const MetadataSpec& spec) {
  const std::string yaml = render_frontmatter(dom.meta, spec) + "\n";

  std::ostringstream body;
  bool seen_refs = false;
  bool in_list = false;
  for (const auto& page : dom.pages) {
    for (const auto& b : page.blocks) {
      if (heuristics.dedupe_title_from_body && is_title_duplicate(b.text, dom.meta.title)) {
        continue;
      }
      // A list must be closed by a blank line, or the next paragraph would
      // read as a lazy continuation of the last item.
      if (in_list && b.kind != BlockKind::ListItem) body << "\n";
      in_list = b.kind == BlockKind::ListItem;
      if (b.kind == BlockKind::Heading) {
        int level = b.heading_level;
        auto low = to_lower(b.text);
        const auto label = front_label_of(b.text);
        if (heuristics.abstract_as_h1 && label == FrontLabel::Abstract) level = 1;
        if (heuristics.keywords_as_h2 && label == FrontLabel::Keywords) level = 2;
        if (low == "references") seen_refs = true;
        body << heading_prefix(level) << fix_missing_compound_hyphens(b.text) << "\n\n";
      } else if (b.kind == BlockKind::Table) {
        body << pad_table(b.table_rows) << "\n";
      } else if (b.kind == BlockKind::Caption || b.kind == BlockKind::FigureRedaction) {
        body << "**" << fix_missing_compound_hyphens(b.text) << "**\n\n";
      } else if (b.kind == BlockKind::ListItem) {
        body << "- " << fix_missing_compound_hyphens(b.text) << "\n";
      } else if (b.kind == BlockKind::Paragraph) {
        body << fix_missing_compound_hyphens(b.text) << "\n\n";
      }
    }
  }

  if (!dom.endnotes.empty()) {
    if (!seen_refs) body << "\n";
    body << "\n";
    for (size_t i = 0; i < dom.endnotes.size(); ++i) {
      const bool labelled = i < dom.endnote_labels.size() && !dom.endnote_labels[i].empty();
      body << "[^" << (labelled ? dom.endnote_labels[i] : std::to_string(i + 1)) << "]: "
           << dom.endnotes[i] << "\n\n";
    }
  }

  // normalize_typography already maps control characters to spaces; this
  // guarantees none reaches the note whatever path the text took.
  std::string text = body.str();
  for (auto& c : text) {
    const auto u = static_cast<unsigned char>(c);
    if ((u < 0x20 && c != '\n') || u == 0x7F) c = ' ';
  }
  return yaml + text;
}

}  // namespace agentpdf
