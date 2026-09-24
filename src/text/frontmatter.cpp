#include "agentpdf/frontmatter.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <regex>
#include <string_view>
#include <vector>

namespace agentpdf {

namespace {

// ASCII lowercase plus the Latin-1 / Latin Extended-A capitals that occur in
// the European-language labels below (Ö, Ü, Ç, É, İ, …).
std::string fold_lower(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(std::tolower(c)));
      continue;
    }
    if (c == 0xC3 && i + 1 < s.size()) {
      auto d = static_cast<unsigned char>(s[i + 1]);
      if (d >= 0x80 && d <= 0x9E && d != 0x97) d = static_cast<unsigned char>(d + 0x20);
      out.push_back(static_cast<char>(c));
      out.push_back(static_cast<char>(d));
      ++i;
      continue;
    }
    if (c == 0xC4 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xB0) {
      out.push_back('i');  // Turkish dotted capital I
      ++i;
      continue;
    }
    // Latin Extended-A capitals are at even code points (Ğ C4 9E, Ş C5 9E, …).
    if ((c == 0xC4 || c == 0xC5) && i + 1 < s.size()) {
      auto d = static_cast<unsigned char>(s[i + 1]);
      if (d >= 0x80 && d <= 0xBF && d % 2 == 0 && !(c == 0xC4 && d == 0xB0)) ++d;
      out.push_back(static_cast<char>(c));
      out.push_back(static_cast<char>(d));
      ++i;
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  return out;
}

bool is_ascii_alpha(unsigned char c) { return std::isalpha(c) != 0; }

}  // namespace

std::string fold_lower_utf8(const std::string& text) { return fold_lower(text); }

namespace {

// Label vocabulary, longest first within each class so prefixes do not win.
struct LabelSet {
  FrontLabel kind;
  std::vector<std::string_view> words;
};

const std::array<LabelSet, 3>& label_sets() {
  static const std::array<LabelSet, 3> sets{{
      {FrontLabel::Keywords,
       {"anahtar kelimeler", "anahtar sözcükler", "palabras clave", "palabras-clave",
        "palavras-chave", "palavras chave", "schlüsselwörter", "słowa kluczowe",
        "parole chiave", "index terms", "trefwoorden", "mots-clés", "mots clés",
        "key words", "keywords", "keyword"}},
      {FrontLabel::Abstract,
       {"zusammenfassung", "samenvatting", "streszczenie", "riassunto", "sommario",
        "abstract", "resumen", "summary", "résumé", "resumo", "sintesi", "özet", "öz"}},
      {FrontLabel::Introduction,
       {"introducción", "introduzione", "introduction", "introdução", "wprowadzenie",
        "einleitung", "einführung", "inleiding", "giriş"}},
  }};
  return sets;
}

bool is_separator_start(std::string_view rest) {
  if (rest.empty()) return false;
  const auto c = static_cast<unsigned char>(rest.front());
  if (c == ':' || c == '.' || c == '-' || c == '|' || c == ';') return true;
  // En dash (E2 80 93), em dash (E2 80 94).
  return rest.size() >= 3 && c == 0xE2 && static_cast<unsigned char>(rest[1]) == 0x80 &&
         (static_cast<unsigned char>(rest[2]) == 0x93 ||
          static_cast<unsigned char>(rest[2]) == 0x94);
}

std::string strip_leading_separators(std::string s) {
  for (;;) {
    s = trim(s);
    if (s.empty()) return s;
    const auto c = static_cast<unsigned char>(s.front());
    if (c == ':' || c == '.' || c == '-' || c == '|' || c == ';') {
      s.erase(0, 1);
      continue;
    }
    if (s.size() >= 3 && c == 0xE2 && static_cast<unsigned char>(s[1]) == 0x80 &&
        (static_cast<unsigned char>(s[2]) == 0x93 ||
         static_cast<unsigned char>(s[2]) == 0x94)) {
      s.erase(0, 3);
      continue;
    }
    return s;
  }
}

bool has_letters_upper_only(std::string_view s) {
  bool any = false;
  for (unsigned char c : s) {
    if (std::islower(c)) return false;
    if (std::isupper(c)) any = true;
  }
  return any;
}

// Optional section number before an Introduction label: "1", "1.", "I.", "1)".
size_t numbering_prefix_length(const std::string& folded) {
  static const std::regex numbering(R"(^((\d{1,2})|([ivx]{1,4}))[\.\)]?\s+)");
  std::smatch m;
  if (std::regex_search(folded, m, numbering)) return static_cast<size_t>(m.length(0));
  return 0;
}

}  // namespace

std::string collapse_letterspacing(const std::string& text) {
  // Space-separated tokens, punctuation preserved. A "letter token" is one
  // ASCII letter, optionally followed by punctuation only ("T:" ends
  // "A B S T R A C T:").
  std::vector<std::string> tokens;
  {
    std::string cur;
    for (char c : text) {
      if (c == ' ') {
        if (!cur.empty()) tokens.push_back(std::move(cur));
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
  }
  auto is_letter_token = [](const std::string& t) {
    if (t.empty() || !is_ascii_alpha(static_cast<unsigned char>(t[0]))) return false;
    return std::all_of(t.begin() + 1, t.end(), [](unsigned char c) {
      return std::ispunct(c) != 0;
    });
  };
  std::string out;
  size_t i = 0;
  while (i < tokens.size()) {
    size_t j = i;
    // Only the final token of a run may carry punctuation.
    while (j < tokens.size() && is_letter_token(tokens[j])) {
      ++j;
      if (tokens[j - 1].size() > 1) break;
    }
    if (!out.empty()) out.push_back(' ');
    if (j - i >= 4) {
      for (size_t k = i; k < j; ++k) out += tokens[k];
      i = j;
    } else {
      out += tokens[i];
      ++i;
    }
  }
  return out;
}

FrontLabel split_front_label(const std::string& line, std::string& label,
                             std::string& rest) {
  label.clear();
  rest.clear();
  const std::string text = collapse_letterspacing(collapse_ws(line));
  if (text.empty() || text.size() > 4000) return FrontLabel::None;
  const std::string folded = fold_lower(text);

  for (const auto& set : label_sets()) {
    const size_t prefix =
        set.kind == FrontLabel::Introduction ? numbering_prefix_length(folded) : 0;
    for (const auto word : set.words) {
      if (folded.compare(prefix, word.size(), word) != 0) continue;
      const size_t end = prefix + word.size();
      if (end < folded.size() && is_ascii_alpha(static_cast<unsigned char>(folded[end])))
        continue;  // "Abstracts…", "Summary-level…" handled by separator check
      const std::string_view after(text.data() + end, text.size() - end);
      std::string remainder = strip_leading_separators(std::string(after));
      const std::string printed = text.substr(0, end);
      if (remainder.empty()) {
        label = printed;
        return set.kind;
      }
      // Introductions are never split from glued prose: too ambiguous.
      if (set.kind == FrontLabel::Introduction) return FrontLabel::None;
      // Glued content requires an explicit separator, or an all-caps label
      // ("ABSTRACT This study…"), so that sentences such as "Summary of the
      // results…" stay prose.
      const std::string_view trimmed_after = std::string_view(after).substr(
          after.find_first_not_of(' ') == std::string_view::npos
              ? after.size()
              : after.find_first_not_of(' '));
      const bool separated = is_separator_start(trimmed_after);
      const bool caps_label = has_letters_upper_only(printed) && printed.size() >= 4 &&
                              std::isupper(static_cast<unsigned char>(remainder.front()));
      // Templates that set the label in bold without punctuation: a keyword
      // label followed by a delimited list ("Keywords Data centers, ESG, …"),
      // or an abstract label (never "Summary") followed by a sentence start.
      const bool list_follows = set.kind == FrontLabel::Keywords &&
                                (remainder.find(',') != std::string::npos ||
                                 remainder.find(';') != std::string::npos ||
                                 remainder.find("\xC2\xB7") != std::string::npos);
      const bool sentence_follows = set.kind == FrontLabel::Abstract && word != "summary" &&
                                    std::isupper(static_cast<unsigned char>(remainder.front()));
      if (!separated && !caps_label && !list_follows && !sentence_follows)
        return FrontLabel::None;
      label = printed;
      rest = remainder;
      return set.kind;
    }
  }
  return FrontLabel::None;
}

FrontLabel front_label_of(const std::string& line) {
  std::string label, rest;
  const auto kind = split_front_label(line, label, rest);
  return rest.empty() ? kind : FrontLabel::None;
}

bool is_provenance_line(const std::string& line) {
  std::string low = fold_lower(collapse_ws(line));
  if (low.empty()) return false;
  // Footnote keys before correspondence lines ("3 E-mail: …", "* Corresponding").
  {
    static const std::regex leading_key(R"(^([0-9]{1,2}|[a-z]|\*|\xE2\x80\xA0|\xE2\x80\xA1)\s+)");
    const std::string rest = std::regex_replace(low, leading_key, "", std::regex_constants::format_first_only);
    if (rest.size() < low.size() && (rest.rfind("e-mail", 0) == 0 || rest.rfind("email", 0) == 0 ||
                                     rest.rfind("corresponding", 0) == 0 || rest.rfind("orcid", 0) == 0))
      return true;
  }
  // Lines that are only e-mail addresses (bylines, correspondence blocks).
  {
    bool any = false;
    bool only_addresses = true;
    std::string cur;
    auto check = [&](const std::string& t) {
      if (t.empty()) return;
      any = true;
      if (t.find('@') == std::string::npos && t != "and" && t != "," && t != ";") only_addresses = false;
    };
    for (char c : low + " ") {
      if (c == ' ' || c == ',' || c == ';') {
        check(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (any && only_addresses) return true;
  }
  const size_t words = split_words(low).size();
  // A copyright sign opens licence lines ("© 2026 The Authors", "0018-9162 ©
  // 2026 IEEE") and fills short ones; mid-sentence in prose ("…are ©
  // Copyright 2002 by…") it is only one marker among several.
  static const std::regex leading_copyright(R"(^([0-9x-]{4,}\s*/?\s*)?(\xC2\xA9|\(c\)))");
  const bool has_copyright_sign =
      low.find("\xC2\xA9") != std::string::npos || low.find("(c) ") != std::string::npos;
  if (std::regex_search(low, leading_copyright)) return true;
  if (has_copyright_sign && words <= 12 && !is_prose_line(line)) return true;

  static const char* starts[] = {
      "received:", "received ", "accepted:", "accepted ", "revised:", "revised ",
      "published online", "available online", "first published", "article history",
      "e-mail:", "e-mail address", "email:", "email address", "*corresponding",
      "* corresponding", "\xE2\x88\x97 corresponding", "\xE2\x88\x97corresponding",
      "corresponding author", "to cite this article", "how to cite",
      "cite this article", "citation:", "journal homepage", "contents lists available",
      "issn", "e-issn", "doi:", "https://doi.org", "http://dx.doi.org", "copyright", "cite this:",
      "this article is licensed", "this is an open access article", "licensee ",
      "contact ", "contact:",
      // Submission history and correspondence in the corpus' other languages.
      "recibido", "aceptado", "recebido", "aceito", "ricevuto", "accettato",
      "başvuru", "kabul tarihi", "sorumlu yazar", "autor correspondiente",
      "autor para correspondência", "autore corrispondente",
  };
  for (const char* s : starts) {
    if (low.rfind(s, 0) == 0) return true;
  }

  static const char* markers[] = {
      "published by", "open access", "creativecommons", "creative commons", "cc by",
      "cc-by", "license", "licence", "all rights reserved", "doi.org", "issn",
      "journal homepage", "sciencedirect", "the author(s)", "the authors.",
      "published online", "available online", "received", "accepted", "@", "copyright",
      "başvuru", "kabul", "revizyon", "sorumlu yazar", "orcid",
  };
  int hits = has_copyright_sign ? 1 : 0;
  for (const char* m : markers) {
    if (low.find(m) != std::string::npos) ++hits;
  }
  // Running prose needs stronger evidence than a short furniture line.
  return hits >= (is_prose_line(line) ? 3 : 2);
}

bool is_record_field_label(const std::string& line) {
  const std::string t = trim(collapse_ws(line));
  const auto colon = t.find(':');
  if (colon == std::string::npos || colon == 0 || colon > 40) return false;
  const std::string key = fold_lower(t.substr(0, colon));
  // Generic bibliographic-record vocabulary (aggregator and library exports).
  static const char* fields[] = {
      "subject", "subjects", "location", "company / organization", "publication title",
      "publication year", "publication date", "publisher", "place of publication",
      "country of publication", "publication subject", "issn", "isbn", "source type",
      "language of publication", "document type", "document id", "document url",
      "proquest document id", "copyright", "last updated", "database", "credit",
      "section", "volume", "issue", "business indexing term", "people", "identifier",
      "accession number", "source", "author", "dateline", "column", "edition",
      "number of pages", "publication status", "record type",
  };
  for (const char* f : fields) {
    if (key == f) return true;
  }
  return false;
}

namespace {

void count_initials(const std::string& line, size_t& words, size_t& lower, size_t& digits) {
  words = lower = digits = 0;
  for (const auto& w : split_words(collapse_ws(line))) {
    ++words;
    const auto c = static_cast<unsigned char>(w.front());
    if (std::islower(c)) ++lower;
    // Lowercase Latin-1 letters (C3 9F..C3 BF) start many non-English words.
    if (c == 0xC3 && w.size() > 1 && static_cast<unsigned char>(w[1]) >= 0x9F) ++lower;
    if (std::isdigit(c)) ++digits;
  }
}

}  // namespace

double lowercase_word_share(const std::string& line) {
  size_t words = 0, lower = 0, digits = 0;
  count_initials(line, words, lower, digits);
  return words ? static_cast<double>(lower) / static_cast<double>(words) : 0.0;
}

bool is_prose_line(const std::string& line) {
  size_t words = 0, lower = 0, digits = 0;
  count_initials(line, words, lower, digits);
  if (words < 8) return false;
  if (line.find('@') != std::string::npos) return false;
  const double n = static_cast<double>(words);
  return lower / n >= 0.5 && digits / n < 0.3;
}

std::string decode_html_entities(const std::string& text) {
  auto append_utf8 = [](std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const auto semi = text.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out.push_back(text[i]);
      continue;
    }
    const std::string name = text.substr(i + 1, semi - i - 1);
    std::uint32_t cp = 0;
    bool ok = true;
    if (name.size() > 1 && name[0] == '#') {
      try {
        cp = (name[1] == 'x' || name[1] == 'X')
                 ? static_cast<std::uint32_t>(std::stoul(name.substr(2), nullptr, 16))
                 : static_cast<std::uint32_t>(std::stoul(name.substr(1)));
      } catch (...) {
        ok = false;
      }
    } else if (name == "amp") cp = '&';
    else if (name == "lt") cp = '<';
    else if (name == "gt") cp = '>';
    else if (name == "quot") cp = '"';
    else if (name == "apos") cp = '\'';
    else if (name == "nbsp") cp = ' ';
    else ok = false;
    if (!ok || cp == 0) {
      out.push_back(text[i]);
      continue;
    }
    append_utf8(out, cp);
    i = semi;
  }
  return out;
}

}  // namespace agentpdf
