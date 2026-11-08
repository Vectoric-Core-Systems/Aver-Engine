#include "aver/synapse/SmartObjectAsset.hpp"

#include "aver/synapse/TextRecords.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

namespace aver::synapse {
namespace {

bool isSlotName(std::string_view s) {
    if (s.empty()) return false;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && i > 0))) return false;
    }
    return true;
}

// Every token of `text` (split on commas and whitespace) must be a valid tag name.
bool parseTags(const std::string& text, TagList& out, std::string& bad) {
    TagList r;
    usize i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || text[i] == ' ' || text[i] == '\t')) ++i;
        usize j = i;
        while (j < text.size() && text[j] != ',' && text[j] != ' ' && text[j] != '\t') ++j;
        if (j > i) {
            const std::string name = text.substr(i, j - i);
            if (!tagNameValid(name)) { bad = name; return false; }
            if (r.v.size() >= 16) { bad = "more than 16 tags"; return false; }
            bool dup = false;
            for (const std::string& e : r.v) dup = dup || e == name;
            if (!dup) r.v.push_back(name);
        }
        i = j;
    }
    out = std::move(r);
    return true;
}

} // namespace

bool parseSoDef(std::string_view text, const std::string& name, SoDef& out, std::string* why) {
    out = SoDef{};
    std::vector<TextRecord> recs;
    std::string rwhy;
    if (!readTextRecords(text, "OCSO", 1, recs, &rwhy)) {
        if (why) *why = name + ":" + rwhy.substr(5);
        return false;
    }
    SoDef d;
    d.name = name;
    std::string err;
    const auto fail = [&](u32 line, const std::string& msg) {
        err = name + ":" + std::to_string(line) + ": " + msg;
        if (why) *why = err;
        return false;
    };

    SoSlotDef* slot = nullptr;
    bool sawDesc = false, objTags = false;
    // Sub-records seen in the open slot.
    bool sEntryYaw = false, sPos = false, sYaw = false, sEntry = false, sTags = false, sReq = false, sBlock = false, sCool = false, sTtl = false, sUse = false, sRel = false;

    const auto nums = [&](const TextRecord& r, usize n, f32* v) {
        if (r.args.size() != n || !r.kv.empty()) return false;
        for (usize i = 0; i < n; ++i)
            if (!parseF32(r.args[i], v[i])) return false;
        return true;
    };

    for (const TextRecord& r : recs) {
        const std::string& k = r.keyword;
        if (k == "SLOT") {
            if (slot) return fail(r.line, "SLOT inside a SLOT: close the previous one with END");
            if (r.args.size() != 1 || !r.kv.empty()) return fail(r.line, "SLOT needs a name");
            if (!isSlotName(r.args[0])) return fail(r.line, "bad slot name '" + r.args[0] + "'");
            for (const SoSlotDef& s : d.slots)
                if (s.name == r.args[0]) return fail(r.line, "duplicate slot '" + r.args[0] + "'");
            if (d.slots.size() >= kSoMaxSlots) return fail(r.line, "more than 32 slots");
            d.slots.emplace_back();
            slot = &d.slots.back();
            slot->name = r.args[0];
            sEntryYaw = sPos = sYaw = sEntry = sTags = sReq = sBlock = sCool = sTtl = sUse = sRel = false;
        } else if (k == "END") {
            if (!slot) return fail(r.line, "END without SLOT");
            if (!r.args.empty() || !r.kv.empty()) return fail(r.line, "END takes no arguments");
            if (!slot->hasEntry) slot->entry = slot->pos;
            if (!slot->hasEntry || !sEntryYaw) slot->entryYawDeg = slot->yawDeg;   // an ENTRY without a yaw follows YAW
            slot = nullptr;
        } else if (k == "DESC") {
            if (slot) return fail(r.line, "DESC belongs before the first SLOT");
            if (sawDesc) return fail(r.line, "duplicate DESC");
            sawDesc = true;
            for (usize i = 0; i < r.args.size(); ++i) d.desc += (i ? " " : "") + r.args[i];
        } else if (k == "TAGS" || k == "REQUIRES" || k == "BLOCKEDBY") {
            if (k != "TAGS" && !slot) return fail(r.line, k + " is only valid inside a SLOT");
            if (!r.kv.empty()) return fail(r.line, k + " takes a list of tags");
            TagList* dst = nullptr;
            bool* seen = nullptr;
            if (!slot) { dst = &d.tags; seen = &objTags; }
            else if (k == "TAGS") { dst = &slot->tags; seen = &sTags; }
            else if (k == "REQUIRES") { dst = &slot->requireAll; seen = &sReq; }
            else { dst = &slot->blockedBy; seen = &sBlock; }
            if (*seen) return fail(r.line, "duplicate " + k);
            *seen = true;
            std::string bad;
            if (!parseTags(r.rest, *dst, bad)) return fail(r.line, "bad tag '" + bad + "'");
        } else if (!slot) {
            return fail(r.line, "unknown record '" + k + "'");
        } else if (k == "POS") {
            if (sPos) return fail(r.line, "duplicate POS");
            sPos = true;
            f32 v[3];
            if (!nums(r, 3, v)) return fail(r.line, "POS needs x y z");
            slot->pos = Vec3{v[0], v[1], v[2]};
        } else if (k == "YAW") {
            if (sYaw) return fail(r.line, "duplicate YAW");
            sYaw = true;
            f32 v[1];
            if (!nums(r, 1, v)) return fail(r.line, "YAW needs degrees");
            slot->yawDeg = v[0];
        } else if (k == "ENTRY") {
            if (sEntry) return fail(r.line, "duplicate ENTRY");
            sEntry = true;
            f32 v[4];
            if (nums(r, 3, v)) {
                slot->entry = Vec3{v[0], v[1], v[2]};
                sEntryYaw = false;
            } else if (nums(r, 4, v)) {
                slot->entry = Vec3{v[0], v[1], v[2]};
                slot->entryYawDeg = v[3];
                sEntryYaw = true;
            } else {
                return fail(r.line, "ENTRY needs x y z [yaw]");
            }
            slot->hasEntry = true;
        } else if (k == "COOLDOWN" || k == "CLAIMTTL") {
            bool& seen = k == "COOLDOWN" ? sCool : sTtl;
            if (seen) return fail(r.line, "duplicate " + k);
            seen = true;
            f32 v[1];
            if (!nums(r, 1, v)) return fail(r.line, k + " needs seconds");
            if (v[0] < 0) return fail(r.line, k + " must not be negative");
            (k == "COOLDOWN" ? slot->cooldownSec : slot->claimTtlSec) = v[0];
        } else if (k == "USE") {
            if (sUse) return fail(r.line, "duplicate USE");
            sUse = true;
            if (!r.kv.empty() || r.args.empty()) return fail(r.line, "USE needs none, event <Name>, bt <path> or statetree <path>");
            std::string m = r.args[0];
            for (char& c : m) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (m == "none" && r.args.size() == 1) {
                slot->use = SoUse::None;
            } else if ((m == "event" || m == "bt" || m == "statetree") && r.args.size() == 2 && !r.args[1].empty()) {
                slot->use = m == "event" ? SoUse::Event : m == "bt" ? SoUse::Bt : SoUse::StateTree;
                slot->useArg = r.args[1];
                if (slot->use == SoUse::Event && !isSlotName(slot->useArg)) return fail(r.line, "bad event name '" + slot->useArg + "'");
            } else {
                return fail(r.line, "USE " + r.args[0] + " is missing its argument");
            }
        } else if (k == "RELEASE") {
            if (sRel) return fail(r.line, "duplicate RELEASE");
            sRel = true;
            std::string m = r.args.size() == 1 ? r.args[0] : std::string();
            for (char& c : m) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (m == "auto") slot->autoRelease = true;
            else if (m == "manual") slot->autoRelease = false;
            else return fail(r.line, "RELEASE must be auto or manual");
        } else {
            return fail(r.line, "unknown record '" + k + "'");
        }
    }
    if (slot) return fail(recs.back().line, "SLOT '" + slot->name + "' is not closed with END");
    out = std::move(d);
    return true;
}

std::string writeSoDef(const SoDef& d) {
    std::string s = "OCSO 1\n";
    if (!d.desc.empty()) s += "DESC " + quoteToken(d.desc) + "\n";
    if (!d.tags.v.empty()) {
        s += "TAGS";
        for (const std::string& t : d.tags.v) s += " " + t;
        s += "\n";
    }
    const auto list = [&](const char* key, const TagList& t) {
        if (t.v.empty()) return;
        s += std::string("  ") + key;
        for (const std::string& e : t.v) s += " " + e;
        s += "\n";
    };
    for (const SoSlotDef& sl : d.slots) {
        s += "SLOT " + sl.name + "\n";
        if (sl.pos.x != 0 || sl.pos.y != 0 || sl.pos.z != 0) s += "  POS " + fmtF32(sl.pos.x) + " " + fmtF32(sl.pos.y) + " " + fmtF32(sl.pos.z) + "\n";
        if (sl.yawDeg != 0) s += "  YAW " + fmtF32(sl.yawDeg) + "\n";
        if (sl.hasEntry) {
            s += "  ENTRY " + fmtF32(sl.entry.x) + " " + fmtF32(sl.entry.y) + " " + fmtF32(sl.entry.z);
            if (sl.entryYawDeg != sl.yawDeg) s += " " + fmtF32(sl.entryYawDeg);
            s += "\n";
        }
        list("TAGS", sl.tags);
        list("REQUIRES", sl.requireAll);
        list("BLOCKEDBY", sl.blockedBy);
        if (sl.cooldownSec != 0) s += "  COOLDOWN " + fmtF32(sl.cooldownSec) + "\n";
        if (sl.claimTtlSec != 0) s += "  CLAIMTTL " + fmtF32(sl.claimTtlSec) + "\n";
        if (sl.use != SoUse::None)
            s += std::string("  USE ") + (sl.use == SoUse::Event ? "event " : sl.use == SoUse::Bt ? "bt " : "statetree ") + quoteToken(sl.useArg) + "\n";
        if (!sl.autoRelease) s += "  RELEASE manual\n";
        s += "END\n";
    }
    return s;
}

bool loadSoDef(const std::string& path, SoDef& out, std::string* why) {
    out = SoDef{};
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) {
        if (why) *why = path + ": cannot open";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!parseSoDef(ss.str(), path, out, why)) return false;
    out.path = path;
    return true;
}

bool saveSoDef(const std::string& path, const SoDef& in, std::string* why) {
    std::ofstream f(aver::nativePath(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = path + ": cannot write";
        return false;
    }
    const std::string text = writeSoDef(in);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (why) *why = path + ": write failed";
        return false;
    }
    return true;
}

SoDef soStarter() {
    SoDef d;
    d.name = "Bench";
    d.desc = "Park bench";
    d.tags.v = {"Furniture.Seat"};
    SoSlotDef a;
    a.name = "sit_left";
    a.pos = Vec3{-40, 0, 45};
    a.entry = Vec3{80, 0, 0};
    a.entryYawDeg = 180;
    a.hasEntry = true;
    a.tags.v = {"Activity.Sit"};
    a.cooldownSec = 3;
    a.claimTtlSec = 20;
    SoSlotDef b = a;
    b.name = "sit_right";
    b.pos = Vec3{40, 0, 45};
    d.slots = {a, b};
    return d;
}

} // namespace aver::synapse
