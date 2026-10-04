// .ocrig text reader and writer. See OcRig.hpp for what the format is and why it is text.
#include "aver/formats/OcRig.hpp"

#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aver::fmt {

namespace {

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// Splits on whitespace, dropping a trailing comment. A '#' only starts a comment at the beginning of
// a TOKEN, so a value may contain one without being truncated.
std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string t;
    while (in >> t) {
        if (!t.empty() && t[0] == '#') break;
        out.push_back(t);
    }
    return out;
}

// `key=value` -> the value, or empty when the token is not that key.
std::string valueOf(const std::string& tok, const char* key) {
    const std::string k = std::string(key) + "=";
    if (tok.size() <= k.size() || tok.compare(0, k.size(), k) != 0) return {};
    return tok.substr(k.size());
}

// "1,2,3" -> Vec3. False on anything else, INCLUDING a missing component: a vector silently read as
// (1,0,0) because two commas were forgotten is the kind of defect that shows up as a limb pointing
// somewhere odd, three files away from its cause.
bool parseVec3(const std::string& s, Vec3& out) {
    f32 v[3] = {0, 0, 0};
    usize start = 0;
    for (int i = 0; i < 3; ++i) {
        const usize comma = s.find(',', start);
        const bool last = (i == 2);
        if (!last && comma == std::string::npos) return false;
        if (last && comma != std::string::npos) return false;   // a fourth component
        const std::string part = s.substr(start, last ? std::string::npos : comma - start);
        if (part.empty()) return false;
        try { v[i] = std::stof(part); } catch (...) { return false; }
        start = last ? start : comma + 1;
    }
    out = Vec3{v[0], v[1], v[2]};
    return true;
}

std::string vec3ToString(const Vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "%g,%g,%g", static_cast<double>(v.x), static_cast<double>(v.y),
                  static_cast<double>(v.z));
    return b;
}

} // namespace

bool OcRigData::valid() const {
    for (const OcRigOp& op : ops) {
        if (op.weight < 0.0f || op.weight > 1.0f) return false;
        if (op.root.empty()) return false;
        if (op.kind == OcRigOpKind::TwoBoneIk) {
            if (op.mid.empty() || op.tip.empty()) return false;
            // A chain that names the same bone twice is not a chain.
            if (op.root == op.mid || op.mid == op.tip || op.root == op.tip) return false;
        }
    }
    return true;
}

bool parseOcRig(std::string_view text, OcRigData& out, std::string* err) {
    out = OcRigData{};

    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    bool sawHeader = false;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> t = tokens(line);
        if (t.empty()) continue;

        const std::string at = " (line " + std::to_string(lineNo) + ")";

        if (t[0] == "OCRIG") {
            if (t.size() < 2) return fail(err, "OCRIG needs a version" + at);
            if (t[1] != "1") return fail(err, "unsupported .ocrig version '" + t[1] + "'" + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(err, "the first record must be OCRIG 1" + at);

        if (t[0] == "NAME") {
            if (t.size() < 2) return fail(err, "NAME needs a value" + at);
            out.name = t[1];
            continue;
        }

        if (t[0] == "OP") {
            if (t.size() < 2) return fail(err, "OP needs a kind" + at);
            OcRigOp op;
            if (t[1] == "twobone")   op.kind = OcRigOpKind::TwoBoneIk;
            else if (t[1] == "aim")  op.kind = OcRigOpKind::AimAt;
            else return fail(err, "unknown op kind '" + t[1] + "'" + at);

            bool sawHint = false;
            for (usize i = 2; i < t.size(); ++i) {
                std::string v;
                if (!(v = valueOf(t[i], "root")).empty())   { op.root = v; continue; }
                if (!(v = valueOf(t[i], "bone")).empty())   { op.root = v; continue; }   // aim's spelling
                if (!(v = valueOf(t[i], "mid")).empty())    { op.mid = v;  continue; }
                if (!(v = valueOf(t[i], "tip")).empty())    { op.tip = v;  continue; }
                if (!(v = valueOf(t[i], "goal")).empty() || !(v = valueOf(t[i], "at")).empty()) {
                    if (!parseVec3(v, op.target)) return fail(err, "bad vector '" + v + "'" + at);
                    continue;
                }
                if (!(v = valueOf(t[i], "pole")).empty() || !(v = valueOf(t[i], "axis")).empty()) {
                    if (!parseVec3(v, op.hint)) return fail(err, "bad vector '" + v + "'" + at);
                    sawHint = true;
                    continue;
                }
                if (!(v = valueOf(t[i], "weight")).empty()) {
                    try { op.weight = std::stof(v); } catch (...) {
                        return fail(err, "bad weight '" + v + "'" + at);
                    }
                    continue;
                }
                return fail(err, "unrecognised attribute '" + t[i] + "'" + at);
            }
            (void)sawHint;
            out.ops.push_back(std::move(op));
            continue;
        }

        return fail(err, "unknown record '" + t[0] + "'" + at);
    }

    if (!sawHeader) return fail(err, "empty or missing OCRIG header");
    // Checked here rather than left to the caller: a rig that parses but is not valid is a file the
    // loader would accept and the runtime would then have to second-guess.
    if (!out.valid()) return fail(err, "the rig parsed but is not valid (a missing bone name, a "
                                       "repeated bone in one chain, or a weight outside [0,1])");
    return true;
}

bool loadOcRig(const std::string& path, OcRigData& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, "cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseOcRig(ss.str(), out, err);
}

std::string writeOcRig(const OcRigData& rig) {
    std::ostringstream o;
    o << "OCRIG 1\n";
    if (!rig.name.empty()) o << "NAME " << rig.name << "\n";
    for (const OcRigOp& op : rig.ops) {
        if (op.kind == OcRigOpKind::TwoBoneIk) {
            o << "OP twobone root=" << op.root << " mid=" << op.mid << " tip=" << op.tip
              << " goal=" << vec3ToString(op.target) << " pole=" << vec3ToString(op.hint);
        } else {
            o << "OP aim bone=" << op.root
              << " at=" << vec3ToString(op.target) << " axis=" << vec3ToString(op.hint);
        }
        char w[32];
        std::snprintf(w, sizeof w, "%g", static_cast<double>(op.weight));
        o << " weight=" << w << "\n";
    }
    return o.str();
}

bool saveOcRig(const std::string& path, const OcRigData& rig, std::string* err) {
    // Atomic rather than truncate-then-write; see writeFileBytesAtomic in FileSystem.hpp. A .ocrig is
    // authored text a user typed, overwritten in place, so a half-written one is lost work.
    const std::string text = writeOcRig(rig);
    if (!writeFileTextAtomic(path, text)) return fail(err, "write failed for " + path);
    return true;
}

} // namespace aver::fmt
