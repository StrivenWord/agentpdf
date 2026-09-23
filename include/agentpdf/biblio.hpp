#pragma once

#include "agentpdf/types.hpp"

#include <string>
#include <vector>

namespace agentpdf {

// Evidence gathered from one PDF for bibliographic fields beyond title,
// authors and DOI. Everything here comes from the PDF itself.
struct BiblioEvidence {
  // Visual rows and row segments of the first pages, all regions included
  // (running heads, citation strips and licence lines live in the margins),
  // followed by the reading-order lines of the same pages.
  std::vector<std::string> front_lines;
  // Only the visual row segments of the first pages (labels such as the
  // article type are read from these, never from reading-order text).
  std::vector<std::string> front_rows;
  // Reading-order lines of every page (aggregator record trailers sit at the
  // end of the document).
  std::vector<std::string> all_lines;
  // Rows that look like the journal masthead, best first.
  std::vector<std::string> masthead_rows;
  std::string info_subject;
  std::string info_keywords;
  std::string body_text;          // body blocks, for language detection
  std::string folded_full_text;   // alnum-folded text of all pages (validation)
  int content_pages = 0;
};

// Fill container, publisher, volume/issue/page, dates, identifiers,
// licence, genre, language, title-short/aliases, url and number-of-pages.
// Only fields still empty are filled.
void extract_bibliographic(DocumentMeta& meta, const BiblioEvidence& evidence);

// Final correction and redaction pass over every metadata value: repair
// typographic damage (ligatures, soft hyphens, private-use glyphs,
// zero-width characters) and drop values that remain clearly garbled or
// fail their field's syntax.
void sanitize_metadata(DocumentMeta& meta);

// Parse a date expression at the start of `text` (English, Spanish,
// Portuguese, Italian, French, German, Turkish month names; ISO; dotted
// day-first; unambiguous slashed). Returns YYYY-MM-DD, YYYY-MM, or "".
std::string parse_leading_date(const std::string& text);

// Typographic repair for metadata text (see sanitize_metadata).
std::string clean_meta_text(const std::string& text);

}  // namespace agentpdf
