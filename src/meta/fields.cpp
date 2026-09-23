#include "agentpdf/pdf.hpp"
#include "agentpdf/util.hpp"

#include <regex>
#include <sstream>

namespace agentpdf {

namespace {

// Canonical field template: key | Obsidian type | Zotero field | CSL variable | source.
// Kept in sync with config/metadata.json and documentation/metadata-fields.md.
const char* const kDefaultFields[] = {
    "title | text | title | title | pdf",
    "title-short | text | shortTitle | title-short | pdf",
    "author | multitext | creators (author) | author | pdf",
    "editor | multitext | creators (editor) | editor | zotero",
    "translator | multitext | creators (translator) | translator | zotero",
    "contributor | multitext | creators (contributor) | contributor | zotero",
    "date | text | date | issued | pdf",
    "year | number | date (year part) | issued | pdf",
    "item-type | text | itemType | type | zotero",
    "genre | text | type | genre | pdf",
    "container-title | text | publicationTitle | container-title | pdf",
    "container-title-short | text | journalAbbreviation | container-title-short | pdf",
    "publisher | text | publisher | publisher | pdf",
    "publisher-place | text | place | publisher-place | zotero",
    "volume | text | volume | volume | pdf",
    "issue | text | issue | issue | pdf",
    "page | text | pages | page | pdf",
    "number-of-pages | number | numPages | number-of-pages | pdf",
    "section | text | section | section | zotero",
    "collection-title | text | seriesTitle | collection-title | zotero",
    "language | text | language | language | pdf",
    "abstract | text | abstractNote | abstract | pdf",
    "keywords | multitext | tags (automatic) | keyword | pdf",
    "license | text | rights | license | pdf",
    "doi | text | DOI | DOI | pdf",
    "issn | multitext | ISSN | ISSN | pdf",
    "isbn | multitext | ISBN | ISBN | pdf",
    "pmid | text | PMID | PMID | zotero",
    "pmcid | text | PMCID | PMCID | zotero",
    "arxiv | text | extra (arXiv) | — | pdf",
    "url | text | url | URL | pdf",
    "available-date | date | extra (Available) | available-date | pdf",
    "date-received | date | extra (Received) | — | pdf",
    "date-accepted | date | extra (Accepted) | — | pdf",
    "accessed | date | accessDate | accessed | caller",
    "date-added | date | dateAdded | — | zotero",
    "library-catalog | text | libraryCatalog | source | zotero",
    "citation-key | text | citationKey | citation-key | zotero",
    "zotero-key | text | item key | — | zotero",
    "zotero-library | text | library / group | — | zotero",
    "zotero-uri | text | zotero://select link | — | zotero",
    "zotero-pdf | text | zotero://open-pdf link | — | zotero",
    "aliases | aliases | — | — | pdf",
    "tags | tags | tags (manual) | — | zotero",
    "agentpdf-extracted | date | — | — | agentpdf",
    "agentpdf-version | text | — | — | agentpdf",
    "agentpdf-source | text | — | — | agentpdf",
};

// YAML double-quoted scalar. Values are single-line (Obsidian Text is a
// single-line type), so newlines and control characters become spaces.
std::string quoted(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20 || c == 0x7F) {
      out.push_back(' ');
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}

bool is_iso_date(const std::string& value) {
  static const std::regex iso(R"(^\d{4}-\d{2}-\d{2}$)");
  return std::regex_match(value, iso);
}

// Value of one canonical key. `text` for scalars, `list` for list types.
struct Value {
  std::string text;
  std::vector<std::string> list;
  int number = 0;
};

Value value_of(const DocumentMeta& m, const std::string& key) {
  Value v;
  if (key == "title") v.text = m.title;
  else if (key == "title-short") v.text = m.title_short;
  else if (key == "author") v.list = m.authors;
  else if (key == "date") v.text = m.date;
  else if (key == "year") v.number = m.year;
  else if (key == "genre") v.text = m.genre;
  else if (key == "container-title") v.text = m.container_title;
  else if (key == "container-title-short") v.text = m.container_title_short;
  else if (key == "publisher") v.text = m.publisher;
  else if (key == "volume") v.text = m.volume;
  else if (key == "issue") v.text = m.issue;
  else if (key == "page") v.text = m.page;
  else if (key == "number-of-pages") v.number = m.number_of_pages;
  else if (key == "language") v.text = m.language;
  else if (key == "abstract") v.text = m.abstract_text;
  else if (key == "keywords") v.list = m.keywords;
  else if (key == "license") v.text = m.license;
  else if (key == "doi") v.text = m.doi;
  else if (key == "issn") v.list = m.issn;
  else if (key == "isbn") v.list = m.isbn;
  else if (key == "arxiv") v.text = m.arxiv;
  else if (key == "url") v.text = m.url;
  else if (key == "available-date") v.text = m.available_date;
  else if (key == "date-received") v.text = m.date_received;
  else if (key == "date-accepted") v.text = m.date_accepted;
  else if (key == "aliases") v.list = m.aliases;
  else if (key == "agentpdf-extracted") v.text = m.agentpdf_extracted;
  else if (key == "agentpdf-version") v.text = m.agentpdf_version;
  else if (key == "agentpdf-source") v.text = m.agentpdf_source;
  return v;
}

}  // namespace

MetadataSpec default_metadata_spec() {
  MetadataSpec spec;
  for (const char* line : kDefaultFields) {
    FieldSpec f;
    if (parse_field_spec(line, f)) spec.fields.push_back(std::move(f));
  }
  return spec;
}

bool parse_field_spec(const std::string& line, FieldSpec& out) {
  std::vector<std::string> parts;
  std::stringstream ss(line);
  std::string part;
  while (std::getline(ss, part, '|')) parts.push_back(trim(part));
  if (parts.size() != 5 || parts[0].empty()) return false;
  static const std::regex key_re(R"(^[a-z][a-z0-9]*(-[a-z0-9]+)*$)");
  if (!std::regex_match(parts[0], key_re)) return false;
  static const char* types[] = {"text", "multitext", "number", "date", "datetime",
                                "checkbox", "aliases", "tags"};
  bool known = false;
  for (const char* t : types) known = known || parts[1] == t;
  if (!known) return false;
  out = FieldSpec{parts[0], parts[1], parts[2], parts[3], parts[4]};
  return true;
}

std::string render_frontmatter(const DocumentMeta& meta, const MetadataSpec& spec) {
  std::ostringstream yaml;
  yaml << "---\n";
  for (const auto& field : spec.fields) {
    if (!field.emitted()) continue;
    const Value v = value_of(meta, field.key);
    const auto& type = field.obsidian_type;
    if (type == "multitext" || type == "aliases" || type == "tags") {
      std::vector<std::string> items;
      for (const auto& item : v.list) {
        if (!trim(item).empty()) items.push_back(trim(item));
      }
      if (items.empty()) continue;
      yaml << field.key << ":\n";
      for (const auto& item : items) yaml << "  - " << quoted(item) << "\n";
    } else if (type == "number") {
      if (v.number <= 0) continue;
      yaml << field.key << ": " << v.number << "\n";
    } else if (type == "date") {
      // Obsidian's Date type accepts full dates only; anything else is
      // omitted rather than written in a form the property cannot hold.
      if (!is_iso_date(v.text)) continue;
      yaml << field.key << ": " << v.text << "\n";
    } else {
      const auto text = trim(v.text);
      if (text.empty()) continue;
      yaml << field.key << ": " << quoted(text) << "\n";
    }
  }
  yaml << "---\n";
  return yaml.str();
}

}  // namespace agentpdf
