#pragma once
// A font set: named families, each an ordered fallback chain of faces (project fonts, optional OS fonts,
// the baked bitmap font), parsed from an .ocfonts document and resolved per code point.
//
//   OCFONTS 1
//   DEFAULT Body
//   FAMILY Body
//   FACE   Fonts/Inter-Regular.ttf
//   FACE   Fonts/Inter-Bold.ttf weight=bold
//   FACE   @exe/Roboto-Regular.ttf          # next to the executable
//   SYSTEM auto                             # Windows fonts, loaded lazily and only when needed
//   BITMAP legacy                           # the font lent with aver_ui_set_font
//   OVERRIDE ja Body BodyJa                 # when the UI language is ja, "Body" means "BodyJa"
//
// Core-only: bytes arrive through a UiFontLoader callback, nothing here opens a file.
#include "aver/ui/UiFont.hpp"
#include "aver/ui/UiFontFace.hpp"
#include "aver/ui/UiTextLayout.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::ui {

// Where a font file is looked for. Mirrors AVER_UI_FONT_*.
enum class UiFontKind : u8 { Content = 0, System = 1, Engine = 2 };

struct UiFontLoader {
    // Opens `name` (a path under the project's Content, a file name in the OS fonts directory, or a name
    // next to the executable). False when it cannot be opened.
    std::function<bool(UiFontKind kind, const std::string& name, UiFontBlob& out)> open;
};

// One line of a family chain, as authored.
struct UiFontEntry {
    enum class Source : u8 { Project, System, Bitmap };
    Source source = Source::Project;
    std::string file;          // "Fonts/X.ttf", "@exe/X.ttf"; "auto" for SYSTEM auto; ignored for BITMAP
    bool bold = false, italic = false;
    std::string lang;          // BCP-47 prefix; empty = any language
    f32 scale = 1.0f;          // size multiplier for this face
    f32 dy = 0.0f;             // baseline shift in pixels at 16 px (positive is down)
    i32 index = 0;             // TTC member
};

struct UiFontFamilyDef {
    std::string name;
    std::vector<UiFontEntry> entries;
    f32 lineGap = 1.0f;        // multiplier on the primary face's line gap
};

struct UiFontOverride {
    std::string lang, from, to;
};

struct UiFontSetDoc {
    i32 version = 1;
    std::string defaultFamily;
    std::vector<UiFontFamilyDef> families;
    std::vector<UiFontOverride> overrides;
    std::string note;          // set by parse when the file declares a newer version than this build reads
};

// The text of the built-in default set: family "Default" = Roboto Regular + Medium next to the
// executable, the OS fonts, the lent bitmap font.
const char* uiDefaultFontSetText();

class UiFontSet {
public:
    // ---- the document ----
    static bool parse(std::string_view text, UiFontSetDoc& out, std::string* err = nullptr);
    static std::string write(const UiFontSetDoc& doc);

    // ---- the runtime set ----
    UiFontSet();
    ~UiFontSet();
    UiFontSet(const UiFontSet&) = delete;
    UiFontSet& operator=(const UiFontSet&) = delete;

    // Replaces the set: opens every project face now, leaves OS faces to open on first need. False (and the
    // set emptied) when the document names no family. Faces that fail to open are skipped, not fatal.
    bool load(const UiFontSetDoc& doc, const UiFontLoader& loader, std::string* err = nullptr);
    void clear();
    // The lent baked font; borrowed, may be null. Answers BITMAP entries.
    void setBitmapFont(const UiFont* f);
    const UiFont* bitmapFont() const { return bitmap_; }
    // The UI language (BCP-47). Reorders the chains (lang=) and applies OVERRIDE rows.
    void setLanguage(std::string_view tag);
    const std::string& language() const { return language_; }
    // Changes on load, clear and language change: anything cached from the set is stale when it moves.
    u32 epoch() const { return epoch_; }

    // True when at least one TrueType face is open or can be tried (a chain with a project or OS entry).
    bool hasTrueType() const;
    u32 familyCount() const { return static_cast<u32>(families_.size()); }
    const std::string& familyName(u32 i) const;
    // Case-insensitive; an unknown or empty name gives the DEFAULT family; OVERRIDE rows apply. -1 only
    // when no set is loaded.
    i32 familyIndex(std::string_view name) const;

    struct Resolved {
        u16 face = kUiFaceNone;    // slot id, kUiFaceBitmap, or kUiFaceNone when no face has the code point
        u32 glyph = 0;
    };
    // The first face in the chain, for (bold, italic), that has the code point; memoised including misses.
    Resolved resolve(i32 family, bool bold, bool italic, u32 cp);
    // The first usable face of a chain (opens OS faces on demand): the face line metrics come from.
    // kUiFaceNone when none opens.
    u16 primaryFace(i32 family, bool bold, bool italic);

    // ---- faces ----
    u32 faceCount() const { return static_cast<u32>(slots_.size()); }
    // A face by slot id; null when the slot is closed, failed or the id is not a slot.
    UiFontFace* face(u16 id);
    const UiFontFace* face(u16 id) const;
    const UiFontEntry& faceEntry(u16 id) const { return slots_[id].entry; }
    const std::string& faceName(u16 id) const { return slots_[id].file; }
    // The multiplier a family applies to its primary face's line gap.
    f32 lineGapScale(i32 family) const;
    // Slots that opened / failed, for the editor's diagnostics.
    u32 openFaceCount() const;
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    enum class State : u8 { Closed, Open, Failed };
    struct Slot {
        UiFontEntry entry;
        std::string file;          // the name handed to the loader
        UiFontKind kind = UiFontKind::Content;
        State state = State::Closed;
        UiFontFace face;
        bool isBitmap = false;
    };
    struct Family {
        std::string name;
        std::vector<u16> slots;    // authored order
        f32 lineGap = 1.0f;
    };

    bool ensureOpen(u16 id);
    const std::vector<u16>& order(i32 family, bool bold, bool italic);
    void dropMemos();

    std::vector<Slot> slots_;
    std::vector<Family> families_;
    std::vector<UiFontOverride> overrides_;
    std::string defaultFamily_;
    UiFontLoader loader_;
    const UiFont* bitmap_ = nullptr;
    std::string language_;         // canonical (lower case, zh normalised)
    std::vector<std::string> warnings_;
    u32 epoch_ = 1;
    std::unordered_map<u64, Resolved> memo_;
    std::unordered_map<u32, std::vector<u16>> orders_;
};

// True when `uiLang` (the UI language) selects an entry tagged `tag`: case-insensitive prefix match on
// BCP-47 subtags, with zh-CN / zh-SG read as zh-Hans and zh-TW / zh-HK / zh-MO as zh-Hant.
bool uiLangMatches(std::string_view uiLang, std::string_view tag);

} // namespace aver::ui
