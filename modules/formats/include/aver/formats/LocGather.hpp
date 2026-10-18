#pragma once
// Localization key gathering and reporting: pure text scanners that find every text reference a project uses
// (the editor walks the files and feeds them in), and the diff of those uses against the string tables on disk.
// docs/LOCALIZATION.md.
#include "aver/core/Types.hpp"
#include "aver/formats/OcStrings.hpp"
#include "aver/formats/OcTable.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

struct LocUse {
    std::string table, key, defaultText;
    bool hasDefault = false;
    std::string source;      // "path:line" (data tables: "path:row" with the row name for JSON)
};

void gatherOcui   (std::string_view text, std::string_view path, std::vector<LocUse>& out);  // SET text|placeholder|items values (items also split on '\n')
void gatherOcgraph(std::string_view text, std::string_view path, std::vector<LocUse>& out);  // any NODE attribute value that is a reference
// Loc.Text/Format/Plural("T","K"[,"D"]) and new LocText("T","K"[,"D"]); string literals only, comments skipped.
// Plural uses carry the base key; the report treats K.one/K.few/K.many/K.other as covered by it.
void gatherCsharp (std::string_view text, std::string_view path, std::vector<LocUse>& out);
void gatherOccaptions(std::string_view text, std::string_view path, std::vector<LocUse>& out);  // SPEAKER name, CUE tag, LINE text
void gatherTable  (const OcTableData&, std::string_view path, std::vector<LocUse>& out);     // every cell that is a reference

using LocCorpus = std::map<std::string /*culture*/, std::map<std::string /*table*/, OcStringsData>>;

struct LocReport {
    struct Missing { std::string culture, table, key; std::string source; };
    struct Unused  { std::string culture, table, key; };
    struct Conflict{ std::string table, key; std::vector<std::string> defaults; };
    std::vector<Missing> missing;      // used, absent in a culture (default culture absent only when no inline default)
    std::vector<Unused> unused;        // present in a table, never used
    std::vector<Conflict> conflicts;   // same Table:Key with different inline defaults
    std::vector<Missing> untranslated; // non-default culture text identical to the default culture's
};
LocReport buildLocReport(const std::vector<LocUse>& used, const LocCorpus&, std::string_view defaultCulture);

// Adds a default-culture entry (text = inline default, empty without one) for every used key missing there;
// returns the count added. Creates the default culture's table when absent.
usize applyGather(const std::vector<LocUse>& used, LocCorpus&, std::string_view defaultCulture);

} // namespace aver::fmt
