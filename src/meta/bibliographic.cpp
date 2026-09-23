#include "agentpdf/biblio.hpp"
#include "agentpdf/frontmatter.hpp"
#include "agentpdf/util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <regex>
#include <set>

namespace agentpdf {

namespace {

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string lowered(const std::string& s) { return fold_lower_utf8(collapse_ws(s)); }

// Count code points that are letters (ASCII letters, or any non-ASCII lead
// byte, which in this corpus is overwhelmingly an accented or non-Latin
// letter) against all non-space code points.
double letter_share(const std::string& s) {
  size_t letters = 0, total = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if ((c & 0xC0) == 0x80) continue;  // continuation byte
    if (std::isspace(c)) continue;
    ++total;
    if (std::isalpha(c) || c >= 0xC0) ++letters;
  }
  return total ? static_cast<double>(letters) / static_cast<double>(total) : 0.0;
}

bool has_replacement_char(const std::string& s) {
  return s.find("\xEF\xBF\xBD") != std::string::npos;  // U+FFFD
}

// Garbled beyond repair: replacement characters, or mostly non-letters.
bool is_garbled(const std::string& s, double min_letter_share = 0.5) {
  if (has_replacement_char(s)) return true;
  return letter_share(s) < min_letter_share;
}

std::string strip_trailing_punct(std::string s) {
  s = trim(s);
  while (!s.empty() && (s.back() == '.' || s.back() == ',' || s.back() == ';' ||
                        s.back() == ':' || s.back() == '|')) {
    s.pop_back();
    s = trim(s);
  }
  return s;
}

bool mostly_upper(const std::string& s) {
  size_t upper = 0, lower = 0;
  for (unsigned char c : s) {
    if (std::isupper(c)) ++upper;
    if (std::islower(c)) ++lower;
  }
  return upper >= 4 && lower * 5 < upper;
}

// "JOURNAL OF THE AMERICAN PLANNING ASSOCIATION" -> "Journal of the
// American Planning Association"; short all-caps tokens that are not
// function words (ACM, IEEE, EPA) stay capitalised.
std::string title_case_if_shouting(const std::string& s) {
  if (!mostly_upper(s)) return s;
  static const std::set<std::string> small{"of", "the", "and", "in", "on", "for", "a", "an",
                                           "to", "at", "by", "de", "la", "le", "du", "des",
                                           "e", "y", "da", "do", "di", "del", "der", "und"};
  std::string out;
  bool first = true;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    std::string word = s.substr(i, j - i);
    std::string low = to_lower(word);
    std::string cased;
    const size_t letters =
        static_cast<size_t>(std::count_if(word.begin(), word.end(), [](unsigned char c) {
          return std::isalpha(c);
        }));
    if (!first && small.count(low)) {
      cased = low;
    } else if (letters <= 4 && letters == word.size() && !small.count(low)) {
      cased = word;  // acronym
    } else {
      cased = low;
      for (auto& c : cased) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          break;
        }
      }
    }
    if (!out.empty()) out.push_back(' ');
    out += cased;
    first = false;
    i = j + 1;
  }
  return out;
}

void set_if_empty(std::string& field, const std::string& value) {
  if (field.empty() && !trim(value).empty()) field = trim(value);
}

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

int current_year() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  return tm.tm_year + 1900;
}

bool plausible_year(int y) { return y >= 1900 && y <= current_year() + 1; }

bool valid_day(int y, int m, int d) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12 || d < 1) return false;
  int limit = days[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) limit = 29;
  return d <= limit;
}

std::string iso(int y, int m, int d) {
  char buf[16];
  if (d > 0) std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
  else std::snprintf(buf, sizeof buf, "%04d-%02d", y, m);
  return buf;
}

const std::map<std::string, int>& month_table() {
  static const std::map<std::string, int> table = [] {
    std::map<std::string, int> t;
    const std::vector<std::vector<std::string>> names{
        // en, abbreviations, es, pt, it, fr, de, tr (with ASCII-folded forms)
        {"january", "jan", "enero", "janeiro", "gennaio", "janvier", "januar", "ocak"},
        {"february", "feb", "febrero", "fevereiro", "febbraio", "février", "fevrier", "februar",
         "şubat", "subat"},
        {"march", "mar", "marzo", "março", "marco", "mars", "märz", "marz", "mart"},
        {"april", "apr", "abril", "aprile", "avril", "nisan"},
        {"may", "mayo", "maio", "maggio", "mai", "mayıs", "mayis"},
        {"june", "jun", "junio", "junho", "giugno", "juin", "juni", "haziran"},
        {"july", "jul", "julio", "julho", "luglio", "juillet", "juli", "temmuz"},
        {"august", "aug", "agosto", "août", "aout", "ağustos", "agustos"},
        {"september", "sep", "sept", "septiembre", "setiembre", "setembro", "settembre",
         "septembre", "eylül", "eylul"},
        {"october", "oct", "octubre", "outubro", "ottobre", "octobre", "oktober", "ekim"},
        {"november", "nov", "noviembre", "novembro", "novembre", "kasım", "kasim"},
        {"december", "dec", "diciembre", "dezembro", "dicembre", "décembre", "decembre",
         "dezember", "aralık", "aralik"},
    };
    for (size_t m = 0; m < names.size(); ++m) {
      for (const auto& n : names[m]) t[n] = static_cast<int>(m) + 1;
    }
    return t;
  }();
  return table;
}

std::string month_alternation() {
  std::vector<std::string> names;
  for (const auto& [name, m] : month_table()) names.push_back(name);
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  std::string alt;
  for (const auto& n : names) alt += (alt.empty() ? "" : "|") + n;
  return alt;
}

}  // namespace

std::string parse_leading_date(const std::string& raw) {
  const std::string text = lowered(raw);
  static const std::string months = month_alternation();
  static const std::regex day_month_year(
      "^(\\d{1,2})(?:st|nd|rd|th|\\.|º)?[\\s\\-]+(?:de\\s+)?(" + months +
      ")\\.?,?[\\s\\-]+(?:de\\s+)?(\\d{4})\\b");
  static const std::regex month_day_year("^(" + months +
                                         ")\\.?\\s+(\\d{1,2})(?:st|nd|rd|th)?,?\\s+(\\d{4})\\b");
  static const std::regex month_year("^(" + months + ")\\.?,?\\s+(?:de\\s+)?(\\d{4})\\b");
  static const std::regex iso_date(R"(^(\d{4})-(\d{2})-(\d{2})\b)");
  static const std::regex dotted(R"(^(\d{1,2})\.(\d{1,2})\.(\d{4})\b)");
  static const std::regex slashed(R"(^(\d{1,2})/(\d{1,2})/(\d{4})\b)");
  std::smatch m;
  auto month_of = [](const std::string& name) {
    auto it = month_table().find(name);
    return it == month_table().end() ? 0 : it->second;
  };
  int y = 0, mo = 0, d = 0;
  if (std::regex_search(text, m, day_month_year)) {
    d = std::stoi(m[1]);
    mo = month_of(m[2]);
    y = std::stoi(m[3]);
  } else if (std::regex_search(text, m, month_day_year)) {
    mo = month_of(m[1]);
    d = std::stoi(m[2]);
    y = std::stoi(m[3]);
  } else if (std::regex_search(text, m, iso_date)) {
    y = std::stoi(m[1]);
    mo = std::stoi(m[2]);
    d = std::stoi(m[3]);
  } else if (std::regex_search(text, m, dotted)) {
    d = std::stoi(m[1]);  // dotted dates are day-first by convention
    mo = std::stoi(m[2]);
    y = std::stoi(m[3]);
  } else if (std::regex_search(text, m, slashed)) {
    const int a = std::stoi(m[1]), b = std::stoi(m[2]);
    y = std::stoi(m[3]);
    if (a > 12 && b <= 12) {
      d = a;
      mo = b;
    } else if (b > 12 && a <= 12) {
      mo = a;
      d = b;
    } else {
      return {};  // 03/04/2026: ambiguous, never guessed
    }
  } else if (std::regex_search(text, m, month_year)) {
    mo = month_of(m[1]);
    y = std::stoi(m[2]);
    if (!plausible_year(y) || mo == 0) return {};
    return iso(y, mo, 0);
  } else {
    return {};
  }
  if (!plausible_year(y) || !valid_day(y, mo, d)) return {};
  return iso(y, mo, d);
}

namespace {

// ---------------------------------------------------------------------------
// Labelled history dates ("Received 12 March 2026; Accepted …")
// ---------------------------------------------------------------------------

enum class DateRole { Skip, Received, Accepted, Available, Published };

struct DateLabel {
  const char* text;
  DateRole role;
};

// Longest first, so "received in revised form" wins over "received" and
// "published online" over "published".
const std::vector<DateLabel>& date_labels() {
  static const std::vector<DateLabel> labels = [] {
    std::vector<DateLabel> l{
        {"received in revised form", DateRole::Skip},
        {"revised manuscript received", DateRole::Skip},
        {"first published online", DateRole::Available},
        {"published online", DateRole::Available},
        {"available online", DateRole::Available},
        {"online publication date", DateRole::Available},
        {"first published", DateRole::Available},
        {"publicado en línea", DateRole::Available},
        {"publicado online", DateRole::Available},
        {"date of publication", DateRole::Published},
        {"publication date", DateRole::Published},
        {"date of current version", DateRole::Skip},
        {"manuscript received", DateRole::Received},
        {"revizyon talebi", DateRole::Skip},
        {"geliş tarihi", DateRole::Received},
        {"kabul tarihi", DateRole::Accepted},
        {"yayın tarihi", DateRole::Published},
        {"received", DateRole::Received},
        {"submitted", DateRole::Received},
        {"recibido", DateRole::Received},
        {"recebido", DateRole::Received},
        {"ricevuto", DateRole::Received},
        {"eingegangen", DateRole::Received},
        {"başvuru", DateRole::Received},
        {"accepted", DateRole::Accepted},
        {"aceptado", DateRole::Accepted},
        {"aceito", DateRole::Accepted},
        {"accettato", DateRole::Accepted},
        {"accepté", DateRole::Accepted},
        {"angenommen", DateRole::Accepted},
        {"kabul", DateRole::Accepted},
        {"revised", DateRole::Skip},
        {"revisado", DateRole::Skip},
        {"revizyon", DateRole::Skip},
        {"published", DateRole::Published},
        {"publicado", DateRole::Published},
        {"pubblicato", DateRole::Published},
        {"publié", DateRole::Published},
        {"veröffentlicht", DateRole::Published},
    };
    std::stable_sort(l.begin(), l.end(), [](const DateLabel& a, const DateLabel& b) {
      return std::string(a.text).size() > std::string(b.text).size();
    });
    return l;
  }();
  return labels;
}

struct LabelledDates {
  std::string received, accepted, available, published;
};

void scan_labelled_dates(const std::string& line, LabelledDates& out) {
  const std::string low = lowered(line);
  size_t pos = 0;
  while (pos < low.size()) {
    // Next label occurrence at a word boundary.
    size_t best = std::string::npos;
    const DateLabel* hit = nullptr;
    for (const auto& label : date_labels()) {
      const std::string needle = label.text;
      size_t at = low.find(needle, pos);
      while (at != std::string::npos && at > 0 &&
             std::isalnum(static_cast<unsigned char>(low[at - 1]))) {
        at = low.find(needle, at + 1);
      }
      if (at == std::string::npos) continue;
      if (at < best || (at == best && hit && needle.size() > std::string(hit->text).size())) {
        best = at;
        hit = &label;
      }
    }
    if (!hit) return;
    size_t after = best + std::string(hit->text).size();
    while (after < low.size() && (low[after] == ' ' || low[after] == ':' || low[after] == ';' ||
                                  low[after] == ',' || low[after] == '-' || low[after] == '.')) {
      ++after;
    }
    for (const char* filler : {"on ", "in "}) {
      if (low.compare(after, std::strlen(filler), filler) == 0) after += std::strlen(filler);
    }
    const std::string date = parse_leading_date(low.substr(after, 40));
    if (!date.empty()) {
      switch (hit->role) {
        case DateRole::Received: set_if_empty(out.received, date); break;
        case DateRole::Accepted: set_if_empty(out.accepted, date); break;
        case DateRole::Available: set_if_empty(out.available, date); break;
        case DateRole::Published: set_if_empty(out.published, date); break;
        case DateRole::Skip: break;
      }
    }
    pos = after;
  }
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

bool issn_checksum_ok(const std::string& digits8) {
  int sum = 0;
  for (int i = 0; i < 7; ++i) sum += (digits8[static_cast<size_t>(i)] - '0') * (8 - i);
  const int check = (11 - sum % 11) % 11;
  const char last = static_cast<char>(std::toupper(static_cast<unsigned char>(digits8[7])));
  return check == 10 ? last == 'X' : last == static_cast<char>('0' + check);
}

// "0360-5442", "03605442", "10862714" -> "0360-5442" when the check digit holds.
std::string normalize_issn(const std::string& raw) {
  std::string d;
  for (unsigned char c : raw) {
    if (std::isdigit(c) || c == 'X' || c == 'x') d.push_back(static_cast<char>(std::toupper(c)));
  }
  if (d.size() != 8 || d.find('X') < 7 || !issn_checksum_ok(d)) return {};
  return d.substr(0, 4) + "-" + d.substr(4);
}

std::string normalize_isbn(const std::string& raw) {
  std::string d;
  for (unsigned char c : raw) {
    if (std::isdigit(c) || c == 'X' || c == 'x') d.push_back(static_cast<char>(std::toupper(c)));
  }
  if (d.size() == 10) {
    int sum = 0;
    for (int i = 0; i < 10; ++i) {
      const int v = d[static_cast<size_t>(i)] == 'X' ? 10 : d[static_cast<size_t>(i)] - '0';
      if (d[static_cast<size_t>(i)] == 'X' && i != 9) return {};
      sum += v * (10 - i);
    }
    return sum % 11 == 0 ? d : std::string{};
  }
  if (d.size() == 13 && d.find('X') == std::string::npos) {
    int sum = 0;
    for (int i = 0; i < 13; ++i) sum += (d[static_cast<size_t>(i)] - '0') * (i % 2 ? 3 : 1);
    return sum % 10 == 0 ? d : std::string{};
  }
  return {};
}

void add_unique(std::vector<std::string>& list, const std::string& value) {
  if (value.empty()) return;
  if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}

void scan_identifiers(const std::string& line, DocumentMeta& meta) {
  static const std::regex issn_labelled(
      R"(\b(?:e-?issn|p-?issn|online issn|print issn|issn)\b\s*(?:\((?:print|online|electronic)\))?\s*:?\s*(\d{4}\s*-?\s*\d{3}[\dxX]))",
      std::regex::icase);
  static const std::regex issn_before_copyright(R"(\b(\d{4}-\d{3}[\dX])\s*/?\s*(?:©|\(c\)))");
  static const std::regex isbn_labelled(R"(\bisbn(?:-1[03])?\s*:?\s*([\dxX][\d\s\-xX]{8,20}))",
                                        std::regex::icase);
  static const std::regex arxiv_id(R"(\barxiv\s*:\s*(\d{4}\.\d{4,5})(?:v\d+)?)", std::regex::icase);
  for (const auto* re : {&issn_labelled, &issn_before_copyright}) {
    for (std::sregex_iterator it(line.begin(), line.end(), *re), end; it != end; ++it) {
      add_unique(meta.issn, normalize_issn((*it)[1]));
    }
  }
  for (std::sregex_iterator it(line.begin(), line.end(), isbn_labelled), end; it != end; ++it) {
    add_unique(meta.isbn, normalize_isbn((*it)[1]));
  }
  std::smatch m;
  if (meta.arxiv.empty() && std::regex_search(line, m, arxiv_id)) meta.arxiv = m[1];
}

// ---------------------------------------------------------------------------
// Citation strips, running heads and Info-dictionary citations
// ---------------------------------------------------------------------------

struct Citation {
  std::string container, container_short, volume, issue, page, date, year;
  bool from_running_head = false;  // container may start with author names
};

// Running heads often begin with the authors ("Smith and Jones Journal …",
// "Smith et al. Journal …"): drop leading author surnames and connectors.
std::string strip_author_prefix(const std::string& container,
                                const std::vector<std::string>& authors) {
  std::set<std::string> surnames;
  for (const auto& a : authors) {
    for (const auto& w : split_words(a)) surnames.insert(to_lower(w));
  }
  std::vector<std::string> words;
  size_t i = 0;
  const auto text = collapse_ws(container);
  while (i <= text.size()) {
    size_t j = text.find(' ', i);
    if (j == std::string::npos) j = text.size();
    words.push_back(text.substr(i, j - i));
    i = j + 1;
  }
  size_t k = 0;
  while (k < words.size()) {
    const auto low = to_lower(strip_trailing_punct(words[k]));
    if (surnames.count(low) || low == "and" || low == "&" || low == "et" || low == "al") {
      ++k;
      continue;
    }
    break;
  }
  if (k == 0 || k >= words.size()) return k == 0 ? container : std::string{};
  std::string out;
  for (; k < words.size(); ++k) out += (out.empty() ? "" : " ") + words[k];
  return out;
}

std::string page_range(const std::string& a, const std::string& b) {
  return b.empty() ? a : a + "-" + b;
}

// One printed or embedded citation string -> fields (first match wins).
bool parse_citation(const std::string& raw, Citation& c) {
  const std::string s = collapse_ws(raw);
  std::smatch m;
  // Elsevier Info Subject: "Energy, 356 (2026) 141024. doi:10.1016/…"
  static const std::regex elsevier_subject(
      R"(^(.{3,120}?),\s*(\d{1,4})\s+\((\d{4})\)\s+([A-Za-z]?\d+)\.?\s*doi:)", std::regex::icase);
  // Elsevier Info Subject, articles in press: "Cell Reports Sustainability, Corrected proof, 100823. doi:…"
  static const std::regex elsevier_in_press(
      R"(^(.{3,120}?),\s*(?:corrected proof|in press|article in press|journal pre-proof),\s*([A-Za-z]?\d+)\.?\s*doi:)",
      std::regex::icase);
  // IEEE Info Subject: "Computer;2026;59;8;10.1109/…" or "IEEE Trans…; ;PP;99;10.1109/…"
  static const std::regex ieee_subject(R"(^([^;]{3,120});\s*(\d{4})?\s*;([^;]*);([^;]*);\s*10\.)");
  // Elsevier citation strip: "Energy 356 (2026) 141024"
  static const std::regex elsevier_strip(R"(^(.{3,120}?)\s+(\d{1,4})\s+\((\d{4})\)\s+([A-Za-z]?\d+)$)");
  // Springer citation strip: "Discover Sustainability (2026) 7:1472"
  static const std::regex springer_strip(R"(^(.{3,120}?)\s+\((\d{4})\)\s+(\d{1,4}):(\d+)$)");
  // Springer running head with author names first:
  // "Brochado Neto and Kazak Discover Sustainability (2026) 7:1472 …"
  static const std::regex springer_head(R"(^(.{3,160}?)\s+\((\d{4})\)\s+(\d{1,4}):(\d+)\b)");
  // ACS article-in-press footer: "ACS EST Air XXXX, XXX, XXX - XXX" (page letter first)
  static const std::regex acs_in_press(
      R"(^(?:[A-Z]\s+)?([A-Z][A-Za-z&.\- ]{2,60}?)\s+XXXX,\s*XXX,\s*XXX)");
  // APA running head: "Journal of … 2000, Vol. 68, No. 2, 331 - 339"
  static const std::regex apa_strip(
      R"(^(.{3,120}?)\s+(\d{4}),\s*vol\.\s*(\d+),\s*no\.\s*(\d+),\s*(\d+)\s*-\s*(\d+))", std::regex::icase);
  // IEEE running head: "IEEE TRANSACTIONS ON COMPUTERS, VOL. 75, NO. 9, SEPTEMBER 2026"
  static const std::regex ieee_head(
      R"(^(.{3,120}?),\s*vol\.\s*(\d+),\s*no\.\s*(\d+),\s*([A-Za-z]+\.?\s+\d{4}))", std::regex::icase);
  // Magazine running foot: "May 2001/Vol. 44, No. 5 COMMUNICATIONS OF THE ACM"
  static const std::regex magazine_foot(
      R"(^([A-Za-z]+\.?\s+\d{4})\s*/\s*vol\.\s*(\d+),\s*no\.\s*(\d+)\s+(.{3,80})$)", std::regex::icase);
  // Taylor & Francis head: "2026, VOL. 92, NO. 4, 1 - 15"
  static const std::regex tandf_head(
      R"(^(\d{4}),\s*vol\.\s*(\d+),\s*no\.\s*(\d+),\s*(\d+)\s*-\s*(\d+)$)", std::regex::icase);
  // Year, volume(issue): pages — "2026, 9(1): 160 - 171" (common in national journals)
  static const std::regex year_vol_issue(
      R"(^(\d{4}),\s*(\d{1,4})\s*\((\d{1,4})\)\s*:\s*(\d+)\s*-\s*(\d+)$)");
  // ACS: "Cite This: ACS ES&T Air 2026, 3, 1234 - 1245"
  static const std::regex acs_cite(
      R"(cite this:\s*(.{2,60}?)\s+(\d{4}),\s*(\d+),\s*(\d+)(?:\s*-\s*(\d+))?)", std::regex::icase);
  // MDPI page header: "Energies 2026, 19, 4153"
  static const std::regex mdpi_head(R"(^([A-Z][A-Za-z&.\- ]{2,60}?)\s+(\d{4}),\s*(\d{1,4}),\s*(\d{1,7})$)");
  // MDPI: "Energies 2026, 19, 1234. https://doi.org/…"
  static const std::regex mdpi_cite(R"(^(.{3,80}?)\s+(\d{4}),\s*(\d+),\s*(\d+)\.\s*https?://doi\.org)");

  if (std::regex_search(s, m, elsevier_subject)) {
    c = {m[1], "", m[2], "", m[4], "", m[3]};
  } else if (std::regex_search(s, m, elsevier_in_press)) {
    c = {m[1], "", "", "", m[2], "", ""};
  } else if (std::regex_search(s, m, ieee_subject)) {
    c.container = m[1];
    c.year = m[2];
    const std::string vol = trim(m[3]), iss = trim(m[4]);
    if (vol != "PP" && !vol.empty()) c.volume = vol;  // PP/99: IEEE early-access placeholders
    if (vol != "PP" && iss != "99" && !iss.empty()) c.issue = iss;
  } else if (std::regex_match(s, m, elsevier_strip)) {
    c = {m[1], "", m[2], "", m[4], "", m[3]};
  } else if (std::regex_match(s, m, springer_strip)) {
    c = {m[1], "", m[3], "", m[4], "", m[2]};
  } else if (std::regex_search(s, m, springer_head)) {
    c = {m[1], "", m[3], "", m[4], "", m[2]};
    c.from_running_head = true;
  } else if (std::regex_search(s, m, acs_in_press)) {
    c.container_short = trim(m[1]);
  } else if (std::regex_search(s, m, apa_strip)) {
    c = {m[1], "", m[3], m[4], page_range(m[5], m[6]), "", m[2]};
  } else if (std::regex_search(s, m, ieee_head)) {
    c = {m[1], "", m[2], m[3], "", parse_leading_date(m[4]), ""};
  } else if (std::regex_match(s, m, magazine_foot)) {
    c = {m[4], "", m[2], m[3], "", parse_leading_date(m[1]), ""};
  } else if (std::regex_match(s, m, tandf_head)) {
    c = {"", "", m[2], m[3], page_range(m[4], m[5]), "", m[1]};
  } else if (std::regex_match(s, m, year_vol_issue)) {
    c = {"", "", m[2], m[3], page_range(m[4], m[5]), "", m[1]};
  } else if (std::regex_search(s, m, acs_cite)) {
    c = {"", m[1], m[3], "", page_range(m[4], m[5].matched ? m[5].str() : ""), "", m[2]};
  } else if (std::regex_match(s, m, mdpi_head)) {
    c = {m[1], "", m[3], "", m[4], "", m[2]};
    // MDPI headers print the abbreviated title ("Journal. Media").
    if (c.container.find('.') != std::string::npos) std::swap(c.container, c.container_short);
  } else if (std::regex_search(s, m, mdpi_cite)) {
    c = {m[1], "", m[3], "", m[4], "", m[2]};
  } else {
    return false;
  }
  c.container = title_case_if_shouting(strip_trailing_punct(c.container));
  return true;
}

// ---------------------------------------------------------------------------
// Publisher and licence (from copyright / licence furniture)
// ---------------------------------------------------------------------------

std::string clean_publisher(std::string p) {
  p = strip_trailing_punct(p);
  static const std::regex legal_suffix(
      R"([,\s]+(ltd|limited|inc|llc|l\.l\.c|b\.v|bv|gmbh|s\.a|plc|co|corp)\.?$)", std::regex::icase);
  for (int i = 0; i < 2; ++i) p = strip_trailing_punct(std::regex_replace(p, legal_suffix, ""));
  const auto low = to_lower(p);
  if (low.empty() || low.rfind("the author", 0) == 0 || low.rfind("author", 0) == 0) return {};
  const auto words = split_words(p);
  if (words.empty() || words.size() > 8) return {};
  if (std::any_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
  return title_case_if_shouting(p);
}

std::string find_publisher(const std::vector<std::string>& all, const std::vector<std::string>& authors) {
  // Only furniture lines (copyright / licence / publisher statements), never
  // body prose, and never copyright holders who are the article's authors.
  std::vector<std::string> lines;
  for (size_t i = 0; i < all.size(); ++i) {
    if (is_provenance_line(all[i]) || (i > 0 && is_provenance_line(all[i - 1])))
      lines.push_back(all[i]);
  }
  std::set<std::string> surnames;
  for (const auto& a : authors) {
    const auto w = split_words(a);
    if (!w.empty()) surnames.insert(to_lower(w.back()));
  }
  auto not_authors = [&](const std::string& p) {
    for (const auto& w : split_words(p)) {
      if (surnames.count(to_lower(w))) return std::string{};
    }
    return p;
  };
  static const std::regex published_by(
      R"(\b[Pp]ublished (?:with (?:a )?licen[cs]e )?by\s+(?:[Tt]he\s+)?([A-Z][A-Za-z&'.\- ]{1,60}?)(?:\s+on behalf of\b|[.;,(]|$))");
  static const std::regex licensee(R"(\b[Ll]icensee\s+([A-Z][A-Za-z&\- ]{1,40}?)(?:[,.;]|$))");
  // Case-sensitive on purpose: the holder must start with a capital letter.
  static const std::regex copyright_org(
      R"((?:©|\([cC]\)|[Cc]opyright|COPYRIGHT)\s*(?:©\s*)?((?:19|20)\d{2})\s+(?:[Bb]y\s+)?(?:[Tt]he\s+)?([A-Z][A-Za-z&'.\- ]{1,60}?)(?:\.\s|\.$|,|;|\s+[Aa]ll rights|\s+\d|\s*$))");
  static const std::regex dangling(R"(\bpublished (?:with (?:a )?licen[cs]e )?by\s*$)", std::regex::icase);
  static const std::regex leading_org(R"(^([A-Z][A-Za-z&'.\- ]{1,60}?)(?:\s+https?://|[.;,(]|$))");
  std::smatch m;
  for (const auto* re : {&published_by, &licensee}) {
    for (size_t i = 0; i < lines.size(); ++i) {
      const auto& line = lines[i];
      if (std::regex_search(line, m, *re)) {
        const auto p = not_authors(clean_publisher(m[1]));
        if (!p.empty()) return p;
      }
      // "… Published by" wrapped onto the next line(s).
      if (re == &published_by && std::regex_search(line, dangling)) {
        for (size_t j = i + 1; j < lines.size() && j <= i + 2; ++j) {
          if (std::regex_search(lines[j], m, leading_org)) {
            const auto p = not_authors(clean_publisher(m[1]));
            if (!p.empty()) return p;
          }
        }
      }
    }
  }
  for (const auto& line : lines) {
    if (std::regex_search(line, m, copyright_org)) {
      const auto p = not_authors(clean_publisher(m[2]));
      if (!p.empty()) return p;
    }
  }
  return {};
}

std::string find_copyright_year(const std::vector<std::string>& lines) {
  static const std::regex year(R"((?:©|\(c\)|copyright)\s*(?:©\s*)?((?:19|20)\d{2}))", std::regex::icase);
  std::smatch m;
  for (const auto& line : lines) {
    if (std::regex_search(line, m, year) && plausible_year(std::stoi(m[1]))) return m[1];
  }
  return {};
}

std::string find_license(const std::vector<std::string>& lines) {
  static const std::set<std::string> codes{"by", "by-sa", "by-nd", "by-nc", "by-nc-sa", "by-nc-nd"};
  static const std::regex cc_url(R"(creativecommons\.org/licen[cs]es/([a-z]+(?:-[a-z]+)*)/(\d\.\d))",
                                 std::regex::icase);
  static const std::regex cc_zero(R"(creativecommons\.org/publicdomain/zero/1\.0|\bCC0\b)",
                                  std::regex::icase);
  static const std::regex cc_text(R"(\bCC[\s-]?(BY(?:[\s-](?:NC|ND|SA)){0,2})[\s-]+(\d\.\d)\b)");
  std::smatch m;
  for (const auto& line : lines) {
    // URLs are often broken across rows or by spaces; search a spaceless copy.
    std::string compact;
    for (char c : line) {
      if (c != ' ') compact.push_back(c);
    }
    if (std::regex_search(compact, m, cc_url)) {
      const auto code = to_lower(m[1]);
      if (codes.count(code))
        return "https://creativecommons.org/licenses/" + code + "/" + m[2].str() + "/";
    }
    if (std::regex_search(line, m, cc_text)) {
      std::string code = to_lower(m[1]);
      std::replace(code.begin(), code.end(), ' ', '-');
      if (codes.count(code))
        return "https://creativecommons.org/licenses/" + code + "/" + m[2].str() + "/";
    }
    if (std::regex_search(compact, m, cc_zero))
      return "https://creativecommons.org/publicdomain/zero/1.0/";
  }
  return {};
}

// ---------------------------------------------------------------------------
// Genre: printed article-type labels
// ---------------------------------------------------------------------------

std::string find_genre(const std::vector<std::string>& lines) {
  static const std::set<std::string> vocabulary{
      "original research", "original research article", "original article", "research article",
      "research paper", "review", "review article", "systematic review", "mini review",
      "short communication", "brief report", "letter", "letter to the editor", "editorial",
      "perspective", "commentary", "opinion", "case study", "case report", "technical note",
      "white paper", "cover feature", "feature", "feature article", "data descriptor",
      "communication", "article", "policy analysis", "critical review"};
  for (const auto& line : lines) {
    std::string t = strip_trailing_punct(collapse_ws(line));
    std::string low = to_lower(t);
    if (low.rfind("type ", 0) == 0) {  // "TYPE Original Research" (label then value)
      t = trim(t.substr(5));
      low = to_lower(t);
    }
    // Printed type labels are capitalised or set in capitals; a bare
    // lowercase word ("review") is running text, not a label.
    if (vocabulary.count(low) && std::isupper(static_cast<unsigned char>(t.front())))
      return title_case_if_shouting(t);
  }
  return {};
}

// ---------------------------------------------------------------------------
// Aggregator record trailers ("Publication title:", "Publication date:", …)
// ---------------------------------------------------------------------------

std::map<std::string, std::string> parse_record(const std::vector<std::string>& lines) {
  std::map<std::string, std::string> record;
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto line = collapse_ws(lines[i]);
    if (!is_record_field_label(line)) continue;
    const auto colon = line.find(':');
    const auto key = to_lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    if (value.empty() && i + 1 < lines.size() && !is_record_field_label(lines[i + 1]))
      value = collapse_ws(lines[i + 1]);
    if (!value.empty() && !record.count(key)) record[key] = value;
  }
  if (record.size() < 3) record.clear();  // a real record has many fields
  return record;
}

std::string language_code_from_name(const std::string& name) {
  static const std::map<std::string, std::string> names{
      {"english", "en"}, {"spanish", "es"},    {"español", "es"}, {"portuguese", "pt"},
      {"italian", "it"}, {"french", "fr"},     {"german", "de"},  {"turkish", "tr"},
      {"dutch", "nl"},   {"chinese", "zh"},    {"japanese", "ja"}, {"arabic", "ar"},
      {"russian", "ru"}, {"korean", "ko"}};
  const auto it = names.find(lowered(name));
  return it == names.end() ? std::string{} : it->second;
}

// ---------------------------------------------------------------------------
// Language detection (function-word frequencies of the body)
// ---------------------------------------------------------------------------

std::string detect_language(const std::string& body) {
  // Only words distinctive to one language count, so shared forms such as
  // "de", "la" or "a" never tip the balance.
  static const std::vector<std::pair<std::string, std::set<std::string>>> profiles{
      {"en", {"the", "and", "of", "to", "is", "that", "with", "this", "are", "from", "which",
              "were", "have", "been"}},
      {"es", {"el", "los", "las", "del", "por", "una", "con", "más", "pero", "también", "según",
              "está", "son"}},
      {"pt", {"os", "das", "dos", "não", "uma", "com", "mais", "pelo", "pela", "também", "ao",
              "são", "às", "seus"}},
      {"it", {"il", "della", "delle", "degli", "gli", "sono", "nel", "nella", "anche", "dei",
              "alla", "che", "più"}},
      {"fr", {"les", "des", "est", "dans", "pour", "sur", "une", "du", "au", "aux", "cette",
              "qui", "sont"}},
      {"de", {"der", "die", "das", "und", "ist", "nicht", "mit", "von", "den", "dem", "ein",
              "eine", "auch", "für"}},
      {"tr", {"ve", "bir", "bu", "ile", "için", "olarak", "olan", "daha", "gibi", "veya", "ise",
              "çok"}},
      {"nl", {"het", "een", "van", "zijn", "niet", "voor", "met", "ook", "worden", "deze"}},
  };
  std::map<std::string, int> score;
  size_t seen = 0;
  for (const auto& word : split_words(fold_lower_utf8(body))) {
    if (++seen > 4000) break;
    for (const auto& [code, words] : profiles) {
      if (words.count(word)) ++score[code];
    }
  }
  std::string best;
  int top = 0, second = 0;
  for (const auto& [code, n] : score) {
    if (n > top) {
      second = top;
      top = n;
      best = code;
    } else if (n > second) {
      second = n;
    }
  }
  if (top < 15 || top < second * 2) return {};  // too little text, or mixed
  return best;
}

// Keywords from the Info dictionary, accepted only when the document text
// supports most of them.
std::vector<std::string> info_keywords(const std::string& raw, const std::string& folded_text) {
  std::vector<std::string> out;
  static const std::regex seps(R"(\s*(;|,)\s*)");
  std::sregex_token_iterator it(raw.begin(), raw.end(), seps, -1), end;
  size_t supported = 0;
  for (; it != end; ++it) {
    const auto k = strip_trailing_punct(it->str());
    if (k.empty() || k.size() > 80) continue;
    out.push_back(k);
    std::string folded;
    for (unsigned char c : fold_lower_utf8(k)) {
      if (std::isalnum(c) || c >= 0x80) folded.push_back(static_cast<char>(c));
    }
    if (!folded.empty() && folded_text.find(folded) != std::string::npos) ++supported;
  }
  if (out.empty() || supported * 2 < out.size()) return {};
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

std::string clean_meta_text(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    // U+00AD soft hyphen: a discretionary break; the word continues.
    if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xAD) {
      i += 2;
      while (i < text.size() && text[i] == ' ') ++i;
      continue;
    }
    // U+00A0 no-break space.
    if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
      out.push_back(' ');
      i += 2;
      continue;
    }
    if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80) {
      const auto d = static_cast<unsigned char>(text[i + 2]);
      if (d >= 0x80 && d <= 0x8A) {  // U+2000..U+200A spaces
        out.push_back(' ');
        i += 3;
        continue;
      }
      if (d >= 0x8B && d <= 0x8D) {  // zero-width space / joiners
        i += 3;
        continue;
      }
    }
    if (c == 0xEF && i + 2 < text.size()) {
      const auto d = static_cast<unsigned char>(text[i + 1]);
      const auto e = static_cast<unsigned char>(text[i + 2]);
      if (d == 0xBB && e == 0xBF) {  // U+FEFF byte-order mark
        i += 3;
        continue;
      }
      if (d == 0xAC && e >= 0x80 && e <= 0x86) {  // U+FB00..U+FB06 ligatures
        static const char* lig[] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
        out += lig[e - 0x80];
        i += 3;
        continue;
      }
      if (d <= 0xA3) {  // U+F000..U+F8FF private use (icon fonts, ORCID glyphs)
        i += 3;
        continue;
      }
    }
    if (c == 0xEE && i + 2 < text.size()) {  // U+E000..U+EFFF private use
      i += 3;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      out.push_back(' ');
      ++i;
      continue;
    }
    out.push_back(static_cast<char>(c));
    ++i;
  }
  return collapse_ws(out);
}

void extract_bibliographic(DocumentMeta& meta, const BiblioEvidence& ev) {
  // Structured sources first: the Info-dictionary citation, then printed
  // citation strips and running heads, then masthead rows.
  Citation cite;
  bool have_cite = false;
  if (!ev.info_subject.empty()) have_cite = parse_citation(ev.info_subject, cite);
  for (const auto& line : ev.front_lines) {
    Citation c;
    if (!parse_citation(line, c)) continue;
    if (c.from_running_head) c.container = strip_author_prefix(c.container, meta.authors);
    // Merge: keep the first value found for each part.
    set_if_empty(cite.container, c.container);
    set_if_empty(cite.container_short, c.container_short);
    set_if_empty(cite.volume, c.volume);
    set_if_empty(cite.issue, c.issue);
    set_if_empty(cite.page, c.page);
    set_if_empty(cite.date, c.date);
    set_if_empty(cite.year, c.year);
    have_cite = true;
  }
  const auto record = parse_record(ev.all_lines);
  auto rec = [&](const char* key) {
    auto it = record.find(key);
    return it == record.end() ? std::string{} : it->second;
  };

  // Container title: citation > aggregator record > masthead row.
  set_if_empty(meta.container_title, cite.container);
  {
    std::string title = rec("publication title");
    if (const auto semi = title.find("; "); semi != std::string::npos) title = title.substr(0, semi);
    set_if_empty(meta.container_title, title);
  }
  for (const auto& row : ev.masthead_rows) set_if_empty(meta.container_title, title_case_if_shouting(row));
  set_if_empty(meta.container_title_short, cite.container_short);

  set_if_empty(meta.volume, cite.volume);
  set_if_empty(meta.volume, rec("volume"));
  set_if_empty(meta.issue, cite.issue);
  set_if_empty(meta.issue, rec("issue"));
  set_if_empty(meta.page, cite.page);
  set_if_empty(meta.page, rec("pages"));

  // Generic "Vol. X, No. Y" in the first pages' furniture.
  static const std::regex vol_no(R"(\bvol(?:ume)?\.?\s*(\d{1,4})\s*[,(]?\s*(?:no|issue|iss|núm|num)\.?\s*(\d{1,4})\b)",
                                 std::regex::icase);
  for (const auto& line : ev.front_lines) {
    std::smatch m;
    if ((meta.volume.empty() || meta.issue.empty()) && std::regex_search(line, m, vol_no)) {
      set_if_empty(meta.volume, m[1]);
      if (meta.volume == m[1].str()) set_if_empty(meta.issue, m[2]);
    }
  }

  // Dates: explicit history labels, then the record, then citations.
  LabelledDates dates;
  for (const auto& line : ev.front_lines) scan_labelled_dates(line, dates);
  // Some templates print the history at the end of the article
  // ("Received: … / Accepted: … / Published online: …"). Elsewhere only lines
  // that begin with a history label count, so body prose never supplies a date.
  static const std::regex history_line(
      R"(^\s*(received|accepted|published online|published|available online|recibido|aceptado|recebido|aceito|ricevuto|accettato)\b\s*:?)",
      std::regex::icase);
  for (const auto& line : ev.all_lines) {
    if (split_words(line).size() <= 30 && std::regex_search(line, history_line))
      scan_labelled_dates(line, dates);
  }
  set_if_empty(meta.date_received, dates.received.size() == 10 ? dates.received : "");
  set_if_empty(meta.date_accepted, dates.accepted.size() == 10 ? dates.accepted : "");
  set_if_empty(meta.available_date, dates.available.size() == 10 ? dates.available : "");
  set_if_empty(meta.date, dates.published);
  set_if_empty(meta.date, parse_leading_date(rec("publication date")));
  set_if_empty(meta.date, dates.available);
  set_if_empty(meta.date, cite.date);
  const std::string record_year = rec("publication year");
  for (const auto& y : {cite.year, record_year}) {
    if (meta.date.empty() && y.size() == 4 && plausible_year(std::stoi(y))) meta.date = y;
  }
  if (meta.year == 0 && meta.date.size() >= 4) meta.year = std::stoi(meta.date.substr(0, 4));
  if (meta.year == 0) {
    const auto y = find_copyright_year(ev.front_lines);
    if (!y.empty()) meta.year = std::stoi(y);
  }

  // Publisher and licence from copyright/licence furniture and the record.
  set_if_empty(meta.publisher, find_publisher(ev.front_lines, meta.authors));
  set_if_empty(meta.publisher, clean_publisher(rec("publisher")));
  set_if_empty(meta.license, find_license(ev.front_lines));

  // Identifiers.
  for (const auto& line : ev.front_lines) scan_identifiers(line, meta);
  if (!rec("issn").empty()) add_unique(meta.issn, normalize_issn(rec("issn")));
  if (!rec("isbn").empty()) add_unique(meta.isbn, normalize_isbn(rec("isbn")));
  if (meta.url.empty() && !meta.doi.empty()) meta.url = "https://doi.org/" + meta.doi;

  // Genre: printed article-type label, else the record's document type.
  set_if_empty(meta.genre, find_genre(ev.front_rows));
  set_if_empty(meta.genre, rec("document type"));

  // Language: detected from the body; the record's statement as fallback.
  set_if_empty(meta.language, detect_language(ev.body_text));
  set_if_empty(meta.language, language_code_from_name(rec("language of publication")));

  // Keywords: the Info dictionary as a last resort.
  if (meta.keywords.empty()) meta.keywords = info_keywords(ev.info_keywords, ev.folded_full_text);

  // Short title (Zotero convention: the main title before a subtitle colon)
  // and aliases, so wikilinks by short title resolve to the note.
  if (meta.title_short.empty()) {
    const auto colon = meta.title.find(": ");
    if (colon != std::string::npos && colon >= 3) {
      const auto short_title = trim(meta.title.substr(0, colon));
      if (split_words(short_title).size() >= 1 && short_title.size() < meta.title.size())
        meta.title_short = short_title;
    }
  }
  if (!meta.title_short.empty()) add_unique(meta.aliases, meta.title_short);

  if (meta.number_of_pages == 0) meta.number_of_pages = ev.content_pages;
}

void sanitize_metadata(DocumentMeta& meta) {
  auto text_field = [](std::string& value, double min_share = 0.5) {
    value = clean_meta_text(value);
    if (!value.empty() && is_garbled(value, min_share)) value.clear();
  };
  text_field(meta.title);
  if (meta.title.size() > 500) meta.title.clear();
  text_field(meta.title_short);
  text_field(meta.container_title);
  if (split_words(meta.container_title).size() > 20) meta.container_title.clear();
  text_field(meta.container_title_short);
  text_field(meta.publisher);
  text_field(meta.genre);
  text_field(meta.abstract_text, 0.6);

  // Syntax-checked scalars: anything else is not a standard value.
  static const std::regex volume_re(R"(^[A-Za-z0-9]{1,12}$)");
  static const std::regex issue_re(R"(^[A-Za-z0-9][A-Za-z0-9 _/\-]{0,14}$)");
  static const std::regex page_re(R"(^[A-Za-z]?\d{1,7}(-[A-Za-z]?\d{1,7})?$)");
  static const std::regex doi_re(R"(^10\.\d{4,9}/[\x21-\x7E]+$)");
  static const std::regex arxiv_re(R"(^\d{4}\.\d{4,5}$)");
  static const std::regex lang_re(R"(^[a-z]{2,3}(-[A-Z]{2})?$)");
  static const std::regex date_re(R"(^\d{4}(-\d{2}(-\d{2})?)?$)");
  auto check = [](std::string& value, const std::regex& re) {
    value = clean_meta_text(value);
    if (!value.empty() && !std::regex_match(value, re)) value.clear();
  };
  meta.page = std::regex_replace(clean_meta_text(meta.page), std::regex(R"(\s*-\s*)"), "-");
  check(meta.volume, volume_re);
  check(meta.issue, issue_re);
  check(meta.page, page_re);
  check(meta.doi, doi_re);
  check(meta.arxiv, arxiv_re);
  check(meta.language, lang_re);
  check(meta.date, date_re);
  if (meta.doi.empty() && meta.url.rfind("https://doi.org/", 0) == 0) meta.url.clear();
  if (meta.year != 0 && (meta.year < 1900 || meta.year > current_year() + 1)) meta.year = 0;

  auto list_field = [](std::vector<std::string>& list, size_t max_len, double min_share) {
    std::vector<std::string> kept;
    std::set<std::string> seen;
    for (auto item : list) {
      item = clean_meta_text(item);
      if (item.empty() || item.size() > max_len || is_garbled(item, min_share)) continue;
      if (seen.insert(to_lower(item)).second) kept.push_back(item);
    }
    list = std::move(kept);
  };
  // Chemical formulas split by subscripts in keyword lines: "O 3" -> "O3",
  // "PM 2.5" -> "PM2.5" (one- or two-letter uppercase symbol + number).
  for (auto& k : meta.keywords) {
    k = std::regex_replace(k, std::regex(R"(\b([A-Z][A-Za-z]?) (\d(?:\.\d+)?)\b)"), "$1$2");
  }
  list_field(meta.authors, 120, 0.5);
  list_field(meta.keywords, 80, 0.34);  // short formulas: "PM2.5", "CO2"
  list_field(meta.aliases, 200, 0.5);
}

}  // namespace agentpdf
