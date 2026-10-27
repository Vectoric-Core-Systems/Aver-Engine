// Localization store and data-table store over their C ABIs (framework_loc_abi.h, framework_table_abi.h),
// driven from a temp-dir fixture. Exits 0 or 1 (ExitCode).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/framework/framework_loc_abi.h"
#include "aver/framework/framework_table_abi.h"
#include "aver/framework/framework_timers_abi.h"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string g_root;   // forward slashes

static void writeFile(const std::string& rel, const std::string& text) {
    const fs::path p = fs::path(g_root) / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    aver::writeFileText(p.generic_string(), text);
}
static std::string path(const std::string& rel) { return (fs::path(g_root) / rel).generic_string(); }

static std::string resolve(const std::string& ref) {
    char buf[256];
    const int32_t n = aver_fw_loc_resolve(ref.c_str(), buf, sizeof buf);
    return n < 0 ? std::string("<-1>") : std::string(buf);
}

// ---- localization -------------------------------------------------------------------------------

static int g_evCount = 0;
static int64_t g_evToken = 0;
static std::string g_evArg;
static int32_t AVER_FW_CALL onCultureEvent(int32_t, int64_t token, void*) {
    ++g_evCount;
    g_evToken = token;
    const char* s = aver_fw_event_current_arg_string(0);
    g_evArg = s ? s : "";
    return 0;
}

static void testLoc() {
    AVER_INFO("=== localization ===");
    check(aver_fw_loc_abi_version() == 2 && aver_fw_table_abi_version() == 1, "ABI versions");

    writeFile("Localization/en/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"Play\"\nENTRY Quit \"Quit\"\nENTRY OnlyEn \"Only English\"\n");
    writeFile("Localization/de/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"Spielen\"\nENTRY Quit \"Beenden\"\n");
    writeFile("Localization/pt/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"Jogar\"\n");
    writeFile("Localization/pt-BR/UI.ocstrings", "OCSTRINGS 1\nENTRY Quit \"Sair\"\n");
    writeFile("Localization/fr/UI.csv", "Key,Text\nPlay,Stub\n");
    writeFile("Localization/es/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"Jugar-ocs\"\n");
    writeFile("Localization/es/UI.csv", "Key,Text\nPlay,Jugar-csv\n");
    writeFile("Localization/not a tag!/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"never\"\n");

    const int32_t e0 = aver_fw_loc_epoch();
    check(aver_fw_loc_init(path("Localization").c_str(), "") == 6, "init counts the 6 valid culture folders (the invalid one is ignored)");
    check(aver_fw_loc_epoch() > e0, "init bumps the epoch");
    check(aver_fw_loc_culture_count() == 6 && std::string(aver_fw_loc_culture_at(0)) == "de" && std::string(aver_fw_loc_culture_at(5)) == "pt-BR" &&
          std::string(aver_fw_loc_culture_at(6)).empty() && std::string(aver_fw_loc_culture_at(-1)).empty(), "cultures are sorted and normalised; out of range is empty");
    check(std::string(aver_fw_loc_default_culture()) == "en" && std::string(aver_fw_loc_culture()) == "en", "default and active start as en");

    check(resolve("@UI:Play") == "Play", "a hit in the active culture");
    check(resolve("@UI:Play=ignored") == "Play", "a hit wins over the inline default");
    check(resolve("@@x") == "<-1>" && resolve("plain") == "<-1>" && resolve("@mention") == "<-1>" && aver_fw_loc_resolve(nullptr, nullptr, 0) == -1,
          "non-references return -1");

    char small[3];
    check(aver_fw_loc_resolve("@UI:Play", nullptr, 0) == 4, "length-only");
    check(aver_fw_loc_resolve("@UI:Play", small, 3) == 4 && std::string(small) == "Pl", "a short buffer truncates but reports the full length");

    // Lazy load: fr is discovered at init but only read when it enters the chain.
    writeFile("Localization/fr/UI.csv", "Key,Text\nPlay,Jouer\n");

    aver_fw_loc_missing_clear();
    check(aver_fw_loc_set_culture("de") == 1, "set_culture de -> 1");
    check(resolve("@UI:Play") == "Spielen" && resolve("@UI:Quit") == "Beenden", "resolves from de");
    check(resolve("@UI:OnlyEn") == "Only English", "a key de lacks falls back to the default culture");

    check(aver_fw_loc_set_culture("pt_br") == 1 && std::string(aver_fw_loc_culture()) == "pt-BR", "pt_br is normalised");
    check(resolve("@UI:Quit") == "Sair" && resolve("@UI:Play") == "Jogar" && resolve("@UI:OnlyEn") == "Only English",
          "chain pt-BR -> pt -> en");

    const int32_t before = aver_fw_loc_epoch();
    check(aver_fw_loc_set_culture("ja") == 0 && resolve("@UI:Play") == "Play", "no tables for ja: returns 0 and resolves from the default");
    check(aver_fw_loc_epoch() > before, "set_culture bumps the epoch");
    check(aver_fw_loc_set_culture("bad!!") == -1 && std::string(aver_fw_loc_culture()) == "ja", "a malformed tag changes nothing");
    const int32_t same = aver_fw_loc_epoch();
    check(aver_fw_loc_set_culture("ja") == 0 && aver_fw_loc_epoch() == same, "setting the active culture again changes nothing");

    check(resolve("@UI:Missing=Fallback") == "Fallback", "the inline default when no culture has the key");
    check(resolve("@UI:Missing") == "[UI:Missing]", "the bracket form without a default");
    check(resolve("@Nope:Key=dflt") == "dflt" && resolve("@UI:Missing=Fallback") == "Fallback", "unknown table, repeated miss");
    check(aver_fw_loc_missing_count() == 2 && std::string(aver_fw_loc_missing_at(0)) == "UI:Missing" && std::string(aver_fw_loc_missing_at(1)) == "Nope:Key",
          "missing keys are unique and in first-hit order");
    aver_fw_loc_missing_clear();
    check(aver_fw_loc_missing_count() == 0 && std::string(aver_fw_loc_missing_at(0)).empty(), "missing list clears");

    char buf[64];
    check(aver_fw_loc_has("UI", "Play") == 1 && aver_fw_loc_has("UI", "Missing") == 0 && aver_fw_loc_has("Nope", "Play") == 0, "has");
    check(aver_fw_loc_get("UI", "Play", buf, sizeof buf) == 4 && std::string(buf) == "Play", "get through the chain");
    check(aver_fw_loc_get("UI", "Missing", buf, sizeof buf) == -1, "get has no default or bracket fallback");

    check(aver_fw_loc_set_culture("es") == 1 && resolve("@UI:Play") == "Jugar-ocs", ".ocstrings beats .csv for the same table");

    // The culture-changed event.
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(onCultureEvent, nullptr);
    aver_fw_event_subscribe("loc.culture_changed", 0, 0, 0, 7);
    g_evCount = 0;
    check(aver_fw_loc_set_culture("fr") == 1, "fr (CSV table, loaded lazily)");
    check(resolve("@UI:Play") == "Jouer", "the CSV edited after init is what the lazy load sees");
    check(g_evCount == 0, "the event is posted, not dispatched inline");
    aver_fw_events_flush();
    check(g_evCount == 1 && g_evToken == 7 && g_evArg == "fr", "loc.culture_changed delivers the new culture");
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(nullptr, nullptr);

    // Hot reload.
    check(aver_fw_loc_set_culture("de") == 1, "back to de");
    const std::string deFile = path("Localization/de/UI.ocstrings");
    const int32_t re0 = aver_fw_loc_epoch();
    writeFile("Localization/de/UI.ocstrings", "OCSTRINGS 1\nENTRY Play \"Spielen2\"\nENTRY Quit \"Beenden\"\n");
    check(aver_fw_loc_reload_path(deFile.c_str()) == 1 && resolve("@UI:Play") == "Spielen2" && aver_fw_loc_epoch() > re0, "an edited file is picked up");
    writeFile("Localization/de/UI.ocstrings", "this is not an ocstrings file\n");
    check(aver_fw_loc_reload_path(deFile.c_str()) == 1 && resolve("@UI:Play") == "Spielen2", "a malformed file keeps the old data");
    writeFile("Localization/de/Extra.ocstrings", "OCSTRINGS 1\nENTRY K \"V\"\n");
    check(aver_fw_loc_reload_path(path("Localization/de/Extra.ocstrings").c_str()) == 1 && aver_fw_loc_has("Extra", "K") == 1, "a new table file appears");
    aver::deleteFile(deFile);
    check(aver_fw_loc_reload_path(deFile.c_str()) == 1 && resolve("@UI:Play") == "Play", "a deleted file drops its table (falls back to the default)");
    check(aver_fw_loc_reload_path("C:/elsewhere/x.ocstrings") == 0 && aver_fw_loc_reload_path(path("Localization/de/readme.txt").c_str()) == 0 &&
          aver_fw_loc_reload_path(path("Data/x.csv").c_str()) == 0 && aver_fw_loc_reload_path(nullptr) == 0, "other files are not localization files");

    // No directory: the store stays usable.
    check(aver_fw_loc_init(path("NoSuchDir").c_str(), "fr") == -1, "init on a missing directory returns -1");
    check(std::string(aver_fw_loc_default_culture()) == "fr" && aver_fw_loc_culture_count() == 0, "...with the requested default and no cultures");
    check(resolve("@UI:Play=Hi") == "Hi" && resolve("@UI:Play") == "[UI:Play]", "...and everything resolves to inline defaults");
    aver_fw_loc_clear();
    check(aver_fw_loc_culture_count() == 0 && std::string(aver_fw_loc_culture()) == "en", "clear returns to the empty store");
}

// ---- data tables --------------------------------------------------------------------------------

// Gettext .po messages (Godot's tr): by source text, through the culture chain, the source when untranslated.
static void testPo() {
    std::printf("-- gettext .po messages\n");
    writeFile("LocPo/de/game.po",
              "# a comment\n"
              "msgid \"\"\n"
              "msgstr \"Language: de\\n\"\n"
              "\n"
              "msgid \"Hello, world!\"\n"
              "msgstr \"Hallo, Welt!\"\n"
              "\n"
              "msgctxt \"menu\"\n"
              "msgid \"Open\"\n"
              "msgstr \"\\u00d6ffnen\"\n"
              "\n"
              "msgid \"Long\"\n"
              "msgstr \"\"\n"
              "\"two \"\n"
              "\"lines\"\n"
              "\n"
              "msgid \"%d apple\"\n"
              "msgid_plural \"%d apples\"\n"
              "msgstr[0] \"%d Apfel\"\n"
              "msgstr[1] \"%d \\u00c4pfel\"\n"
              "\n"
              "msgid \"Untranslated\"\n"
              "msgstr \"\"\n");
    check(aver_fw_loc_init(path("LocPo").c_str(), "en") >= 1, "init finds the culture with a .po file");
    aver_fw_loc_set_culture("de");
    char buf[128];
    const auto tr = [&](const char* m, const char* ctx) { aver_fw_loc_tr(m, ctx, buf, sizeof buf); return std::string(buf); };
    check(tr("Hello, world!", "") == "Hallo, Welt!", "a message with punctuation and spaces translates");
    check(tr("Open", "menu") != "Open" && tr("Open", "") == "Open", "msgctxt separates the same text in two contexts");
    check(tr("Long", "") == "two lines", "continuation lines join");
    check(tr("Untranslated", "") == "Untranslated", "an empty msgstr falls back to the source");
    check(tr("Never seen", nullptr) == "Never seen", "an unknown message is its own source text");
    aver_fw_loc_tr_n("%d apple", "%d apples", 1, "", buf, sizeof buf);
    check(std::string(buf) == "%d Apfel", "tr_n picks msgstr[0] for one");
    aver_fw_loc_tr_n("%d apple", "%d apples", 3, "", buf, sizeof buf);
    check(std::string(buf).find("pfel") != std::string::npos && std::string(buf) != "%d Apfel", "and msgstr[1] for more");
    aver_fw_loc_tr_n("%d pear", "%d pears", 2, "", buf, sizeof buf);
    check(std::string(buf) == "%d pears", "an untranslated plural falls back to the source plural");
    aver_fw_loc_set_culture("en");
    check(tr("Hello, world!", "") == "Hello, world!", "in a culture without it, the source text");
}

static void testTables() {
    AVER_INFO("=== data tables ===");
    writeFile("Data/items.csv", "Name,Damage:int\nsword,25\nbow,12\n");
    writeFile("Data/mobs.json", "[{\"Name\":\"orc\",\"hp\":10}]");
    writeFile("Data/broken.csv", "Name,A,B\nr,1\n");

    check(aver_fw_content_set_root(path("").c_str()) == 1 && aver_fw_content_set_root(nullptr) == 0, "content root set");
    aver_fw_content_set_root(g_root.c_str());
    check(std::string(aver_fw_content_root()) == g_root, "content root read back");

    const int32_t h = aver_fw_table_load("Data/items.csv");
    check(h > 0, "a content-relative path loads");
    check(aver_fw_table_load(path("Data/items.csv").c_str()) == h, "the absolute path of the same file returns the same handle");
    check(std::string(aver_fw_table_path(h)).find("items.csv") != std::string::npos && std::string(aver_fw_table_path(999)).empty(), "path");
    check(aver_fw_table_column_count(h) == 1 && std::string(aver_fw_table_column_name(h, 0)) == "Damage" &&
          aver_fw_table_column_type(h, 0) == AVER_FW_TABLE_COL_INT && aver_fw_table_column_type(h, 5) == -1, "columns");
    check(aver_fw_table_column_find(h, "damage") == 0 && aver_fw_table_column_find(h, "nope") == -1, "column_find");
    check(aver_fw_table_row_count(h) == 2 && std::string(aver_fw_table_row_name(h, 1)) == "bow" && aver_fw_table_row_find(h, "SWORD") == 0 &&
          aver_fw_table_row_find(h, "axe") == -1, "rows");
    check(std::string(aver_fw_table_cell(h, 0, 0)) == "25" && std::string(aver_fw_table_cell(h, 9, 0)).empty() &&
          std::string(aver_fw_table_cell(h, 0, 9)).empty() && std::string(aver_fw_table_cell(h, -1, 0)).empty(), "cell, and out of range is empty");
    check(aver_fw_table_version(h) == 1 && aver_fw_table_version(999) == -1 && aver_fw_table_row_count(999) == -1, "version starts at 1; bad handles are -1");

    check(aver_fw_table_load("Data/missing.csv") == 0 && std::string(aver_fw_table_error()).size() > 0, "a missing file fails with an error");
    check(aver_fw_table_load("Data/broken.csv") == 0 && std::string(aver_fw_table_error()).find("line 2") != std::string::npos, "a bad file fails and names the line");
    check(aver_fw_table_load("") == 0, "an empty path fails");

    const int32_t ep = aver_fw_tables_epoch();
    writeFile("Data/items.csv", "Name,Damage:int\nsword,30\nbow,12\n");
    check(aver_fw_table_reload(h) == 1 && std::string(aver_fw_table_cell(h, 0, 0)) == "30", "reload picks up an edit");
    check(aver_fw_table_version(h) == 2 && aver_fw_tables_epoch() > ep, "reload bumps the table version and the epoch");

    writeFile("Data/items.csv", "Name,Damage:int\nsword\n");
    const int32_t ep2 = aver_fw_tables_epoch();
    check(aver_fw_table_reload(h) == 0 && std::string(aver_fw_table_error()).size() > 0, "a failed reload reports an error");
    check(std::string(aver_fw_table_cell(h, 0, 0)) == "30" && aver_fw_table_version(h) == 2 && aver_fw_tables_epoch() == ep2,
          "...and keeps the old data, version and epoch");

    writeFile("Data/items.csv", "Name,Damage:int\nsword,35\nbow,12\n");
    check(aver_fw_table_reload_path(path("Data/items.csv").c_str()) == 1 && std::string(aver_fw_table_cell(h, 0, 0)) == "35", "reload_path finds the loaded table by path");
    check(aver_fw_table_reload_path(path("Data/other.csv").c_str()) == 0, "reload_path of a table that is not loaded does nothing");

    const int32_t j = aver_fw_table_load("Data/mobs.json");
    check(j > 0 && j != h && aver_fw_table_row_count(j) == 1 && std::string(aver_fw_table_column_name(j, 0)) == "hp" &&
          aver_fw_table_column_type(j, 0) == AVER_FW_TABLE_COL_INT && std::string(aver_fw_table_cell(j, 0, 0)) == "10", "a JSON table loads");
    check(aver_fw_table_reload_path("") == 2 && aver_fw_table_reload_path(nullptr) == 2, "an empty path reloads every loaded table");

    check(aver_fw_table_unload(j) == 1 && aver_fw_table_unload(j) == 0 && aver_fw_table_row_count(j) == -1, "unload invalidates the handle");
    const int32_t j2 = aver_fw_table_load("Data/mobs.json");
    check(j2 > 0 && j2 != j, "loading again gives a fresh handle");
    check(aver_fw_table_reload_path("") == 2, "unloaded tables are not reloaded; the new one is");
}

int main() {
    AVER_INFO("FrameworkContentTest");
    std::error_code ec;
    g_root = (fs::temp_directory_path() / "aver_fw_content_test").generic_string();
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    testLoc();
    testPo();
    testTables();

    fs::remove_all(g_root, ec);
    if (g_failures == 0) AVER_INFO("=== all {} checks passed ===", g_checks);
    else AVER_ERROR("=== FAILED === {} of {} checks failed", g_failures, g_checks);
    return aver::exitCode(g_failures ? aver::ExitCode::Failed : aver::ExitCode::Ok);
}
