#pragma once

#include <string>

namespace agentpdf {

// Publisher-independent vocabulary for article front matter. Every predicate
// here must hold across journal templates and languages; publisher- or
// fixture-specific strings do not belong in this module.

enum class FrontLabel { None, Abstract, Keywords, Introduction };

// Collapse letter-spaced words ("A B S T R A C T" -> "ABSTRACT"). Only runs of
// four or more single letters are joined, so ordinary prose is untouched.
std::string collapse_letterspacing(const std::string& text);

// Classify a line that *is* a front-matter label (optionally followed by
// ':', '.', or a dash). Returns None for anything longer than the label.
FrontLabel front_label_of(const std::string& line);

// Split a line that begins with a label glued to its content, such as
// "Abstract - This paper…" or "Keywords: a; b; c". On success `label` holds the
// label text exactly as printed (letter-spacing collapsed, trailing
// punctuation removed) and `rest` holds the remaining text. A bare label line
// also succeeds, with an empty `rest`.
FrontLabel split_front_label(const std::string& line, std::string& label,
                             std::string& rest);

// Journal provenance that belongs in metadata, not in the reading flow:
// copyright and licence statements, received/accepted/published history,
// correspondence addresses, journal homepages, ISSNs.
bool is_provenance_line(const std::string& line);

// A labelled field of a database export record ("Publication title:",
// "Document URL:", …) of the kind appended by aggregators such as ProQuest.
bool is_record_field_label(const std::string& line);

// Running prose: enough words, mostly lowercase-initial (language agnostic),
// not a name list or affiliation.
bool is_prose_line(const std::string& line);

// Lowercase-initial share of a line's words (0 when it has none).
double lowercase_word_share(const std::string& line);

// Decode the HTML/XML character references that appear in PDF Info strings.
std::string decode_html_entities(const std::string& text);

}  // namespace agentpdf
