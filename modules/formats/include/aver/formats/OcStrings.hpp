#pragma once
// .ocstrings and string-table .csv: the translated text of one table in one culture, plus the localization
// vocabulary every layer shares (text references, culture tags, the fallback chain). Text only, no engine
// dependency, in the base Aver.Formats target. docs/LOCALIZATION.md.
//
//   OCSTRINGS 1
//   CULTURE de
//   # Main menu
//   ENTRY Menu.Play "Spielen"
//   ENTRY Menu.Quit "Beenden" NOTE "Shown on the quit button; keep short"
//
// A malformed ENTRY line is skipped with a diagnostic naming the line: a typo must not blank a whole table at
// runtime. Only a missing or unsupported header fails the parse.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr int kOcStringsVersion = 1;

struct OcStringEntry { std::string key, text, note; };
struct OcStringsData {
    int version = 1;
    std::string culture;                    // CULTURE record; the folder is authoritative, this is advisory
    std::vector<OcStringEntry> entries;
};

// False only for a missing/unsupported header (err set). Per-line problems go to *diag and the line is skipped.
bool parseOcstrings(std::string_view text, OcStringsData& out, std::vector<std::string>* diag = nullptr,
                    std::string* err = nullptr);
// RFC 4180, header row `Key,Text` plus optional `Note` (case-insensitive). False if Key or Text is missing.
bool parseStringsCsv(std::string_view text, OcStringsData& out, std::vector<std::string>* diag = nullptr,
                     std::string* err = nullptr);

// Line-preserving merge with `existing` (docs/plans/wave2/DATA_TABLES_LOCALIZATION.md 3.2): an ENTRY whose key is
// still present is rewritten only if its text or note changed, a vanished key is dropped, comments, blanks and
// unknown records stay put, new keys append after the last ENTRY. Empty `existing`: a fresh file.
std::string writeOcstrings(const OcStringsData& d, std::string_view existing = "");
std::string writeStringsCsv(const OcStringsData& d);

// A gettext .po catalogue (Godot's PO translations): messages by their source text, not by key. `context` is
// msgctxt; `plurals` holds msgstr[0..n] when there is a msgid_plural, else `text` is the msgstr. The header entry
// (empty msgid) is not an entry; its Language goes to `language`. An empty translation is kept as missing.
struct PoEntry { std::string context, id, idPlural, text; std::vector<std::string> plurals; };
struct PoData { std::string language; std::vector<PoEntry> entries; };
bool parsePo(std::string_view text, PoData& out, std::vector<std::string>* diag = nullptr, std::string* err = nullptr);

// By extension: .ocstrings or .csv.
bool loadStrings(const std::string& path, OcStringsData& out, std::vector<std::string>* diag, std::string* err);

// Table: [A-Za-z0-9_]+   Key: [A-Za-z0-9_.-]+
bool validLocKey(std::string_view);
bool validLocTable(std::string_view);

// A text reference: "@" Table ":" Key [ "=" DefaultText ]. The default is everything after the first '='.
struct LocRef { std::string table, key, defaultText; bool hasDefault = false; };
// False when `s` is not a reference, including the "@@..." escape and any '@' string that misses the grammar.
bool parseLocRef(std::string_view s, LocRef& out);
std::string formatLocRef(const LocRef&);                       // "@Table:Key" or "@Table:Key=Default"

// "pt_br" -> "pt-BR", "zh-hans" -> "zh-Hans". Empty when malformed
// ([A-Za-z]{2,3}(-[A-Za-z]{4})?(-([A-Za-z]{2}|[0-9]{3}))?).
std::string normalizeCulture(std::string_view tag);
// Lookup order for active culture A and project default D: A, each parent of A (drop the last subtag), D and
// its parents; duplicates removed. A malformed A is skipped; an empty or malformed D means "en".
std::vector<std::string> cultureChain(std::string_view active, std::string_view dflt);

} // namespace aver::fmt
