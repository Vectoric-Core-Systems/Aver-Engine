// .ocfonts parse / write and the runtime fallback chain. See UiFontSet.hpp.
#include "aver/ui/UiFontSet.hpp"
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>

namespace aver::ui {
namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// The UI language in the form entries are matched against: lower case, '-' separators, and zh given a
// script (zh-CN -> zh-hans-cn, zh-TW -> zh-hant-tw).
std::string canonUiLang(std::string_view s) {
    std::string o = lower(s);
    for (char& c : o) if (c == '_') c = '-';
    if (o.size() >= 2 && o.compare(0, 2, "zh") == 0 && (o.size() == 2 || o[2] == '-')) {
        if (o.find("-hans") == std::string::npos && o.find("-hant") == std::string::npos) {
            std::string region;
            if (o.size() > 3) {
                const usize end = o.find('-', 3);
                region = o.substr(3, end == std::string::npos ? std::string::npos : end - 3);
            }
            const bool trad = region == "tw" || region == "hk" || region == "mo";
            o = std::string("zh-") + (trad ? "hant" : "hans") + (region.empty() ? std::string() : "-" + region);
        }
    }
    return o;
}

// Strips a trailing `# comment`: a '#' outside quotes at the start of the line or after whitespace.
std::string_view stripComment(std::string_view line) {
    bool inQuote = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') inQuote = !inQuote;
        else if (c == '#' && !inQuote && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))))
            return line.substr(0, i);
    }
    return line;
}

struct SysCand {
    const char* file;
    const char* lang;
};

// SYSTEM auto: optional Windows fonts per script, in preference order. A name the host cannot open is skipped.
const SysCand kSystemAuto[] = {
    {"segoeui.ttf", ""}, {"tahoma.ttf", ""}, {"arial.ttf", ""},          // Latin, Greek, Cyrillic, Arabic, Hebrew
    {"seguisym.ttf", ""},                                                  // symbols
    {"YuGothM.ttc", "ja"}, {"meiryo.ttc", "ja"}, {"msgothic.ttc", "ja"},
    {"malgun.ttf", "ko"},
    {"msyh.ttc", "zh-Hans"}, {"simsun.ttc", "zh-Hans"},
    {"msjh.ttc", "zh-Hant"},
    {"Nirmala.ttf", ""},                                                   // Devanagari
    {"LeelawUI.ttf", ""},                                                  // Thai
};

bool parseAttrs(const std::vector<std::string>& t, usize from, UiFontEntry& e, i32 lineNo, std::string* err) {
    const auto bad = [&](const std::string& why) {
        if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };
    for (usize i = from; i < t.size(); ++i) {
        const std::string& a = t[i];
        const usize eq = a.find('=');
        const std::string key = lower(eq == std::string::npos ? a : a.substr(0, eq));
        const std::string val = eq == std::string::npos ? std::string() : a.substr(eq + 1);
        if (key == "italic") {
            e.italic = true;
        } else if (key == "weight") {
            if (iequals(val, "bold")) e.bold = true;
            else if (iequals(val, "regular")) e.bold = false;
            else return bad("weight must be regular or bold, not '" + val + "'");
        } else if (key == "index") {
            if (!uiParseInt(val, e.index) || e.index < 0) return bad("index must be a whole number, not '" + val + "'");
        } else if (key == "lang") {
            if (val.empty()) return bad("lang needs a language tag");
            e.lang = val;
        } else if (key == "scale") {
            if (!uiParseFloat(val, e.scale) || e.scale <= 0.0f) return bad("scale must be a positive number, not '" + val + "'");
        } else if (key == "dy") {
            if (!uiParseFloat(val, e.dy)) return bad("dy must be a number, not '" + val + "'");
        }
        // anything else: an attribute from a newer build
    }
    return true;
}

void writeAttrs(std::string& s, const UiFontEntry& e) {
    if (e.bold) s += " weight=bold";
    if (e.italic) s += " italic";
    if (e.index != 0) s += " index=" + std::to_string(e.index);
    if (!e.lang.empty()) s += " lang=" + uiQuote(e.lang);
    if (e.scale != 1.0f) s += " scale=" + uiFormatFloat(e.scale);
    if (e.dy != 0.0f) s += " dy=" + uiFormatFloat(e.dy);
}

} // namespace

const char* uiDefaultFontSetText() {
    return R"(OCFONTS 1
DEFAULT Default
FAMILY Default
FACE @exe/Roboto-Regular.ttf
FACE @exe/Roboto-Medium.ttf weight=bold
SYSTEM auto
BITMAP legacy
)";
}

bool uiLangMatches(std::string_view uiLang, std::string_view tag) {
    if (tag.empty() || uiLang.empty()) return false;
    const std::string ui = canonUiLang(uiLang);
    const std::string t = lower(tag);
    if (ui == t) return true;
    return ui.size() > t.size() && ui.compare(0, t.size(), t) == 0 && ui[t.size()] == '-';
}

// ---- parse / write --------------------------------------------------------------------------------

bool UiFontSet::parse(std::string_view text, UiFontSetDoc& out, std::string* err) {
    out = UiFontSetDoc{};
    bool sawHeader = false;
    i32 lineNo = 0;
    const auto fail = [&](const std::string& why) {
        if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
        return false;
    };

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        line = stripComment(line);

        std::vector<std::string> t;
        if (!uiTokenize(line, t)) return fail("unterminated quote");
        if (t.empty()) continue;
        const std::string key = upper(t[0]);

        if (key == "OCFONTS") {
            sawHeader = true;
            i32 v = 1;
            if (t.size() > 1 && !uiParseInt(t[1], v)) return fail("OCFONTS needs a version number");
            out.version = v;
            if (v > 1) out.note = "written by a newer build (OCFONTS " + std::to_string(v) + "); unknown keys are skipped";
        } else if (key == "DEFAULT") {
            if (t.size() < 2) return fail("DEFAULT needs a family name");
            out.defaultFamily = t[1];
        } else if (key == "FAMILY") {
            if (t.size() < 2) return fail("FAMILY needs a name");
            UiFontFamilyDef f;
            f.name = t[1];
            out.families.push_back(std::move(f));
        } else if (key == "FACE" || key == "SYSTEM" || key == "BITMAP") {
            if (out.families.empty()) return fail(key + " before any FAMILY");
            if (t.size() < 2) return fail(key + " needs a file name");
            UiFontEntry e;
            e.source = key == "FACE" ? UiFontEntry::Source::Project
                     : key == "SYSTEM" ? UiFontEntry::Source::System : UiFontEntry::Source::Bitmap;
            e.file = t[1];
            if (!parseAttrs(t, 2, e, lineNo, err)) return false;
            out.families.back().entries.push_back(std::move(e));
        } else if (key == "LINEGAP") {
            if (out.families.empty()) return fail("LINEGAP before any FAMILY");
            f32 g = 1.0f;
            if (t.size() < 2 || !uiParseFloat(t[1], g) || g < 0.0f) return fail("LINEGAP needs a number of zero or more");
            out.families.back().lineGap = g;
        } else if (key == "OVERRIDE") {
            if (t.size() < 4) return fail("OVERRIDE needs <language> <family> <replacement>");
            out.overrides.push_back(UiFontOverride{t[1], t[2], t[3]});
        }
        // unknown keys: a record from a newer build
    }
    if (!sawHeader) {
        if (err) *err = "not an .ocfonts: no OCFONTS header line";
        return false;
    }
    return true;
}

std::string UiFontSet::write(const UiFontSetDoc& doc) {
    std::string s = "OCFONTS " + std::to_string(doc.version) + "\n";
    if (!doc.defaultFamily.empty()) s += "DEFAULT " + uiQuote(doc.defaultFamily) + "\n";
    for (const UiFontFamilyDef& f : doc.families) {
        s += "\nFAMILY " + uiQuote(f.name) + "\n";
        for (const UiFontEntry& e : f.entries) {
            switch (e.source) {
                case UiFontEntry::Source::Project: s += "FACE " + uiQuote(e.file); break;
                case UiFontEntry::Source::System: s += "SYSTEM " + uiQuote(e.file); break;
                case UiFontEntry::Source::Bitmap: s += "BITMAP " + uiQuote(e.file.empty() ? std::string("legacy") : e.file); break;
            }
            writeAttrs(s, e);
            s += "\n";
        }
        if (f.lineGap != 1.0f) s += "LINEGAP " + uiFormatFloat(f.lineGap) + "\n";
    }
    if (!doc.overrides.empty()) s += "\n";
    for (const UiFontOverride& o : doc.overrides)
        s += "OVERRIDE " + uiQuote(o.lang) + " " + uiQuote(o.from) + " " + uiQuote(o.to) + "\n";
    return s;
}

// ---- the runtime set ------------------------------------------------------------------------------

UiFontSet::UiFontSet() = default;
UiFontSet::~UiFontSet() = default;

void UiFontSet::dropMemos() {
    memo_.clear();
    orders_.clear();
}

void UiFontSet::clear() {
    slots_.clear();
    families_.clear();
    overrides_.clear();
    defaultFamily_.clear();
    warnings_.clear();
    loader_ = UiFontLoader{};
    dropMemos();
    ++epoch_;
}

void UiFontSet::setBitmapFont(const UiFont* f) {
    if (bitmap_ == f) return;
    bitmap_ = f;
    dropMemos();
    ++epoch_;
}

void UiFontSet::setLanguage(std::string_view tag) {
    const std::string canon = canonUiLang(tag);
    if (canon == language_) return;
    language_ = canon;
    dropMemos();
    ++epoch_;
}

bool UiFontSet::ensureOpen(u16 id) {
    if (id >= slots_.size()) return false;
    Slot& s = slots_[id];
    if (s.isBitmap) return false;
    if (s.state == State::Open) return true;
    if (s.state == State::Failed) return false;
    UiFontBlob blob;
    if (!loader_.open || !loader_.open(s.kind, s.file, blob)) {
        s.state = State::Failed;
        if (s.entry.source == UiFontEntry::Source::Project) warnings_.push_back("cannot open font '" + s.file + "'");
        return false;
    }
    std::string why;
    if (!s.face.open(blob, s.entry.index, &why)) {
        s.state = State::Failed;
        warnings_.push_back("font '" + s.file + "' rejected: " + why);
        return false;
    }
    s.state = State::Open;
    return true;
}

bool UiFontSet::load(const UiFontSetDoc& doc, const UiFontLoader& loader, std::string* err) {
    clear();
    if (doc.families.empty()) {
        if (err) *err = "the font set declares no FAMILY";
        return false;
    }
    loader_ = loader;
    overrides_ = doc.overrides;

    const auto add = [&](const UiFontEntry& e, const std::string& file, UiFontKind kind, bool bitmap) -> u16 {
        Slot s;
        s.entry = e;
        s.file = file;
        s.kind = kind;
        s.isBitmap = bitmap;
        slots_.push_back(std::move(s));
        return static_cast<u16>(slots_.size() - 1);
    };

    for (const UiFontFamilyDef& fd : doc.families) {
        Family f;
        f.name = fd.name;
        f.lineGap = fd.lineGap;
        // Project faces first, then OS faces, then the bitmap font, each group in authored order.
        for (const UiFontEntry::Source src : {UiFontEntry::Source::Project, UiFontEntry::Source::System, UiFontEntry::Source::Bitmap}) {
            for (const UiFontEntry& e : fd.entries) {
                if (e.source != src) continue;
                if (src == UiFontEntry::Source::Bitmap) {
                    f.slots.push_back(add(e, e.file, UiFontKind::Content, true));
                } else if (src == UiFontEntry::Source::System) {
                    if (iequals(e.file, "auto")) {
                        for (const SysCand& c : kSystemAuto) {
                            UiFontEntry x = e;
                            x.file = c.file;
                            if (x.lang.empty()) x.lang = c.lang;
                            f.slots.push_back(add(x, x.file, UiFontKind::System, false));
                        }
                    } else {
                        f.slots.push_back(add(e, e.file, UiFontKind::System, false));
                    }
                } else {
                    const bool exe = e.file.compare(0, 5, "@exe/") == 0;
                    f.slots.push_back(add(e, exe ? e.file.substr(5) : e.file, exe ? UiFontKind::Engine : UiFontKind::Content, false));
                }
            }
        }
        families_.push_back(std::move(f));
    }

    defaultFamily_ = families_.front().name;
    for (const Family& f : families_)
        if (iequals(f.name, doc.defaultFamily)) { defaultFamily_ = f.name; break; }

    // Project faces open now. If none did, try OS faces in order until one does, so "is there any
    // TrueType face" is answered here and not on the first frame.
    for (usize i = 0; i < slots_.size(); ++i)
        if (!slots_[i].isBitmap && slots_[i].entry.source == UiFontEntry::Source::Project) ensureOpen(static_cast<u16>(i));
    if (openFaceCount() == 0) {
        for (usize i = 0; i < slots_.size(); ++i) {
            if (slots_[i].isBitmap) continue;
            if (ensureOpen(static_cast<u16>(i))) break;
        }
    }
    dropMemos();
    ++epoch_;
    return true;
}

u32 UiFontSet::openFaceCount() const {
    u32 n = 0;
    for (const Slot& s : slots_) if (s.state == State::Open) ++n;
    return n;
}

bool UiFontSet::hasTrueType() const { return openFaceCount() > 0; }

const std::string& UiFontSet::familyName(u32 i) const {
    static const std::string none;
    return i < families_.size() ? families_[i].name : none;
}

f32 UiFontSet::lineGapScale(i32 family) const {
    return (family >= 0 && static_cast<usize>(family) < families_.size()) ? families_[static_cast<usize>(family)].lineGap : 1.0f;
}

i32 UiFontSet::familyIndex(std::string_view name) const {
    if (families_.empty()) return -1;
    std::string n = name.empty() ? defaultFamily_ : std::string(name);
    for (const UiFontOverride& o : overrides_) {
        if (iequals(o.from, n) && uiLangMatches(language_, o.lang)) { n = o.to; break; }
    }
    for (usize i = 0; i < families_.size(); ++i)
        if (iequals(families_[i].name, n)) return static_cast<i32>(i);
    // Unknown name: the default family (through the same override rows).
    std::string d = defaultFamily_;
    for (const UiFontOverride& o : overrides_) {
        if (iequals(o.from, d) && uiLangMatches(language_, o.lang)) { d = o.to; break; }
    }
    for (usize i = 0; i < families_.size(); ++i)
        if (iequals(families_[i].name, d)) return static_cast<i32>(i);
    for (usize i = 0; i < families_.size(); ++i)
        if (iequals(families_[i].name, defaultFamily_)) return static_cast<i32>(i);
    return 0;
}

UiFontFace* UiFontSet::face(u16 id) {
    if (id >= slots_.size() || slots_[id].state != State::Open) return nullptr;
    return &slots_[id].face;
}

const UiFontFace* UiFontSet::face(u16 id) const {
    if (id >= slots_.size() || slots_[id].state != State::Open) return nullptr;
    return &slots_[id].face;
}

const std::vector<u16>& UiFontSet::order(i32 family, bool bold, bool italic) {
    const u32 key = (static_cast<u32>(family) << 2) | (bold ? 2u : 0u) | (italic ? 1u : 0u);
    const auto it = orders_.find(key);
    if (it != orders_.end()) return it->second;

    std::vector<u16> out;
    const Family& f = families_[static_cast<usize>(family)];
    for (int pass = 0; pass < 3; ++pass) {   // exact style, then the right weight (a missing italic shears it), then the rest
        for (const UiFontEntry::Source src : {UiFontEntry::Source::Project, UiFontEntry::Source::System, UiFontEntry::Source::Bitmap}) {
            std::vector<u16> matching, others;
            for (const u16 id : f.slots) {
                const UiFontEntry& e = slots_[id].entry;
                if (e.source != src) continue;
                const int rank = (e.bold == bold && e.italic == italic) ? 0 : e.bold == bold ? 1 : 2;
                if (rank != pass) continue;
                (!e.lang.empty() && uiLangMatches(language_, e.lang) ? matching : others).push_back(id);
            }
            out.insert(out.end(), matching.begin(), matching.end());
            out.insert(out.end(), others.begin(), others.end());
        }
    }
    return orders_.emplace(key, std::move(out)).first->second;
}

UiFontSet::Resolved UiFontSet::resolve(i32 family, bool bold, bool italic, u32 cp) {
    if (family < 0 || static_cast<usize>(family) >= families_.size()) return Resolved{};
    const u64 key = (static_cast<u64>(family) << 34) | (bold ? 1ull << 33 : 0ull) | (italic ? 1ull << 32 : 0ull) | cp;
    const auto m = memo_.find(key);
    if (m != memo_.end()) return m->second;

    Resolved r;
    // Opening a face can warn but never touches the order lists, so the reference holds for the loop.
    const std::vector<u16>& ord = order(family, bold, italic);
    for (const u16 id : ord) {
        if (slots_[id].isBitmap) {
            if (bitmap_ && bitmap_->glyph(cp)) { r.face = kUiFaceBitmap; r.glyph = cp; break; }
            continue;
        }
        if (!ensureOpen(id)) continue;
        const u32 g = slots_[id].face.glyphFor(cp);
        if (g != 0) { r.face = id; r.glyph = g; break; }
    }
    memo_.emplace(key, r);
    return r;
}

u16 UiFontSet::primaryFace(i32 family, bool bold, bool italic) {
    if (family < 0 || static_cast<usize>(family) >= families_.size()) return kUiFaceNone;
    const std::vector<u16>& ord = order(family, bold, italic);
    for (const u16 id : ord)
        if (!slots_[id].isBitmap && ensureOpen(id)) return id;
    return kUiFaceNone;
}

} // namespace aver::ui
