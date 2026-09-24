#include "agentpdf/frontmatter.hpp"
#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace agentpdf {

namespace {

// Superscript digits in running text, as flow_box_lines marks them.
constexpr char kNoteMark = '\x1F';

bool note_key_text(const std::string& text) {
  if (text == "\xE2\x80\xA0" || text == "\xE2\x80\xA1" || text == "\xC2\xA7") return true;  // † ‡ §
  if (text.empty() || text.size() > 3) return false;
  if (text == "*" || text == "**") return true;
  return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool rows_overlap(const BBox& a, const BBox& b) {
  const double overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  return overlap >= 0.35 * std::max(0.1, std::min(a.height(), b.height()));
}

void append_note_text(std::string& note, const std::string& text) {
  if (note.empty()) {
    note = text;
    return;
  }
  if (note.size() >= 2 && note.back() == '-' &&
      std::isalpha(static_cast<unsigned char>(note[note.size() - 2])) && !text.empty() &&
      std::islower(static_cast<unsigned char>(text.front()))) {
    note.pop_back();
    note += text;
    return;
  }
  note += ' ';
  note += text;
}

struct Note {
  int page = 0;
  std::string key;
  std::string text;
};

// Front-matter notes (a corresponding author's address, submission dates, a
// licence) are provenance, which metadata reads; content notes are kept.
bool provenance_note(const std::string& text) {
  // A licence runs long ("© The Author(s) 2026. Open Access This article is
  // licensed under a Creative Commons …"); it is provenance however long.
  const auto low = fold_lower_utf8(trim(text));
  static const char* licence_openings[] = {"\xC2\xA9", "(c) ", "open access", "this article is licensed",
                                           "this is an open access article", "this article is an open access",
                                           "copyright"};
  for (const char* opening : licence_openings) {
    if (low.rfind(opening, 0) == 0 &&
        (low.find("licen") != std::string::npos || low.find("creative commons") != std::string::npos ||
         low.find("rights reserved") != std::string::npos))
      return true;
  }
  if (split_words(text).size() > 60) return false;
  return is_provenance_line(text);
}

}  // namespace

std::vector<TextLine> footnote_lines_by_geometry(const PageDom& page) {
  std::vector<size_t> selected;
  bool spans_middle = false;
  for (size_t k = 0; k < page.normalized_boxes.size(); ++k) {
    const auto& b = page.normalized_boxes[k];
    if (b.region != RegionKind::Footnote || b.rotation != 0 || b.text.empty()) continue;
    selected.push_back(k);
    if (b.box.x0 < page.width * 0.5 && b.box.x1 > page.width * 0.5) spans_middle = true;
  }
  std::vector<TextLine> out;
  for (int column : {0, 1}) {
    std::vector<size_t> members;
    for (size_t k : selected) {
      const bool right = page.normalized_boxes[k].box.cx() >= page.width * 0.5;
      if (spans_middle ? column == 0 : (right ? 1 : 0) == column) members.push_back(k);
    }
    std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
      return page.normalized_boxes[a].box.cy() < page.normalized_boxes[b].box.cy();
    });
    std::vector<std::vector<size_t>> rows;
    for (size_t k : members) {
      if (!rows.empty() && rows_overlap(page.normalized_boxes[rows.back().front()].box,
                                        page.normalized_boxes[k].box)) {
        rows.back().push_back(k);
      } else {
        rows.push_back({k});
      }
    }
    for (auto& row : rows) {
      std::sort(row.begin(), row.end(), [&](size_t a, size_t b) {
        return page.normalized_boxes[a].box.x0 < page.normalized_boxes[b].box.x0;
      });
      TextLine line;
      size_t begin = 0;
      if (row.size() >= 2 && note_key_text(page.normalized_boxes[row[0]].text)) {
        const auto& key = page.normalized_boxes[row[0]];
        const auto& next = page.normalized_boxes[row[1]];
        const bool smaller =
            key.type_size > 0 && next.type_size > 0 && key.type_size < next.type_size * 0.9;
        const bool raised = key.box.cy() < next.box.cy() - next.box.height() * 0.1;
        if (smaller || raised) {
          line.note_key = key.text;
          begin = 1;
        }
      }
      std::string text;
      for (size_t i = begin; i < row.size(); ++i) {
        const auto& b = page.normalized_boxes[row[i]];
        if (!text.empty()) text += ' ';
        text += b.text;
        if (i == begin) line.geom = b.box;
        line.geom.x0 = std::min(line.geom.x0, b.box.x0);
        line.geom.x1 = std::max(line.geom.x1, b.box.x1);
        line.geom.y0 = std::min(line.geom.y0, b.box.y0);
        line.geom.y1 = std::max(line.geom.y1, b.box.y1);
      }
      line.text = collapse_ws(text);
      line.has_geom = true;
      if (!line.text.empty()) out.push_back(std::move(line));
    }
  }
  return out;
}

void link_note_markers(DocumentDom& dom) {
  // Notes in document order. A page whose footnote area opens without a key
  // continues the last note of an earlier page.
  // Provenance lines under the notes (a DOI, submission dates, a licence)
  // never continue a note; they gather apart, and go.
  std::vector<Note> notes;
  bool in_provenance = false;
  for (const auto& page : dom.pages) {
    for (const auto& line : page.footnote_lines) {
      const bool provenance = line.note_key.empty() && is_provenance_line(line.text);
      if (!line.note_key.empty() || notes.empty() || (provenance && !in_provenance)) {
        notes.push_back({page.index, line.note_key, {}});
      }
      in_provenance = line.note_key.empty() ? (provenance || in_provenance) : false;
      append_note_text(notes.back().text, line.text);
    }
  }
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [](const Note& note) {
                               return trim(note.text).empty() || provenance_note(note.text);
                             }),
              notes.end());

  // Labels: the printed key when it names one note only, else key and page.
  std::map<std::string, int> key_count;
  for (const auto& note : notes) {
    if (!note.key.empty()) ++key_count[note.key];
  }
  size_t unkeyed = 0;
  auto label_of = [&](const Note& note) -> std::string {
    if (note.key.empty()) return "n" + std::to_string(++unkeyed);
    if (key_count[note.key] == 1 &&
        std::all_of(note.key.begin(), note.key.end(), [](unsigned char c) { return std::isdigit(c); }))
      return note.key;
    return (note.key == "*" ? std::string("a") : note.key) + "-p" + std::to_string(note.page + 1);
  };
  std::map<std::pair<int, std::string>, std::string> labels;  // (page, key) -> label
  for (const auto& note : notes) {
    if (!note.key.empty()) labels[{note.page, note.key}] = label_of(note);
  }

  // Markers: a superscript number that names a note on its page (or the
  // page before, for a note that began there) becomes a note reference;
  // any other is a citation number and stays plain text, set apart from a
  // word it was printed against.
  for (auto& page : dom.pages) {
    for (auto& line : page.lines) {
      auto& text = line.text;
      if (text.find(kNoteMark) == std::string::npos) continue;
      std::string out;
      for (size_t i = 0; i < text.size();) {
        if (text[i] != kNoteMark) {
          out.push_back(text[i++]);
          continue;
        }
        const auto close = text.find(kNoteMark, i + 1);
        if (close == std::string::npos) {
          ++i;
          continue;
        }
        const std::string digits = text.substr(i + 1, close - i - 1);
        std::string label;
        for (int p : {page.index, page.index - 1}) {
          auto it = labels.find({p, digits});
          if (it != labels.end()) {
            label = it->second;
            break;
          }
        }
        if (!label.empty()) {
          out += "[^" + label + "]";
        } else {
          if (!out.empty() && std::isalnum(static_cast<unsigned char>(out.back()))) out += ' ';
          out += digits;
        }
        i = close + 1;
      }
      text = out;
    }
  }

  unkeyed = 0;
  // An author's affiliation or note keyed from the byline (front matter,
  // which is not part of the text) is metadata, not a note of the text.
  int first_page = -1;
  for (const auto& page : dom.pages) {
    if (!page.wrapper_page && !page.lines.empty()) {
      first_page = page.index;
      break;
    }
  }
  std::set<std::string> referenced;
  for (const auto& page : dom.pages) {
    for (const auto& line : page.lines) {
      for (size_t at = line.text.find("[^"); at != std::string::npos; at = line.text.find("[^", at + 2)) {
        const auto close = line.text.find(']', at);
        if (close != std::string::npos) referenced.insert(line.text.substr(at + 2, close - at - 2));
      }
    }
  }
  auto affiliation_like = [](const std::string& text) {
    const auto low = fold_lower_utf8(text);
    static const char* markers[] = {"universit", "department", "departamento", "dipartimento",
                                    "institute", "instituto", "istituto", "school of", "college",
                                    "faculty", "faculdade", "facultad", "laborator", "research center",
                                    "research centre", "programa de", "program in", "e-mail", "email",
                                    "@", "orcid", "corresponding", "gmbh", "inc.", "ltd"};
    for (const char* m : markers) {
      if (low.find(m) != std::string::npos) return true;
    }
    return false;
  };

  unkeyed = 0;
  for (const auto& note : notes) {
    const auto label = label_of(note);
    if (note.page == first_page && !referenced.count(label) && affiliation_like(note.text)) continue;
    // An unkeyed line or two under the opening page's text (a corresponding
    // author's name over the address) is front matter.
    if (note.page == first_page && note.key.empty() && split_words(note.text).size() <= 8) continue;
    dom.endnotes.push_back(trim(note.text));
    dom.endnote_labels.push_back(label);
  }
}

}  // namespace agentpdf
