// .ocrig text reader and writer. See OcRig.hpp for what the format is and why it is text.
#include "aver/formats/OcRig.hpp"

#include "aver/platform/FileSystem.hpp"

#include <cmath>
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

// "1,2,3" -> exactly `n` floats. False on anything else, INCLUDING a missing component: a vector silently
// read as (1,0,0) because two commas were forgotten is the kind of defect that shows up as a limb pointing
// somewhere odd, three files away from its cause.
bool parseFloats(const std::string& s, int n, f32* v) {
    usize start = 0;
    for (int i = 0; i < n; ++i) {
        const usize comma = s.find(',', start);
        const bool last = (i == n - 1);
        if (!last && comma == std::string::npos) return false;
        if (last && comma != std::string::npos) return false;   // an extra component
        const std::string part = s.substr(start, last ? std::string::npos : comma - start);
        if (part.empty()) return false;
        try { v[i] = std::stof(part); } catch (...) { return false; }
        start = last ? start : comma + 1;
    }
    return true;
}

bool parseVec3(const std::string& s, Vec3& out) {
    f32 v[3] = {0, 0, 0};
    if (!parseFloats(s, 3, v)) return false;
    out = Vec3{v[0], v[1], v[2]};
    return true;
}

bool parseQuat(const std::string& s, Quat& out) {
    f32 v[4] = {0, 0, 0, 1};
    if (!parseFloats(s, 4, v)) return false;
    out = Quat{v[0], v[1], v[2], v[3]};
    return true;
}

std::string vec3ToString(const Vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "%g,%g,%g", static_cast<double>(v.x), static_cast<double>(v.y),
                  static_cast<double>(v.z));
    return b;
}

std::string quatToString(const Quat& q) {
    char b[128];
    std::snprintf(b, sizeof b, "%g,%g,%g,%g", static_cast<double>(q.x), static_cast<double>(q.y),
                  static_cast<double>(q.z), static_cast<double>(q.w));
    return b;
}

std::string numToString(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
    return b;
}

// A vec3 value: `x,y,z`, `@input`, or `bone:name[+x,y,z]`.
bool parseVecValue(const std::string& v, OcRigRef& ref, Vec3& lit) {
    ref = OcRigRef{};
    if (v[0] == '@') {
        if (v.size() < 2) return false;
        ref.kind = OcRigRef::Kind::Input;
        ref.name = v.substr(1);
        return true;
    }
    if (v.compare(0, 5, "bone:") == 0) {
        std::string rest = v.substr(5);
        const usize plus = rest.find('+');
        ref.kind = OcRigRef::Kind::Bone;
        if (plus != std::string::npos) {
            if (!parseVec3(rest.substr(plus + 1), ref.offset)) return false;
            rest = rest.substr(0, plus);
        }
        if (rest.empty()) return false;
        ref.name = rest;
        return true;
    }
    return parseVec3(v, lit);
}

// A quaternion value: `x,y,z,w` or `@input`.
bool parseQuatValue(const std::string& v, OcRigRef& ref, Quat& lit) {
    ref = OcRigRef{};
    if (v[0] == '@') {
        if (v.size() < 2) return false;
        ref.kind = OcRigRef::Kind::Input;
        ref.name = v.substr(1);
        return true;
    }
    return parseQuat(v, lit);
}

std::string vecValueStr(const OcRigRef& r, const Vec3& lit) {
    switch (r.kind) {
        case OcRigRef::Kind::Input: return "@" + r.name;
        case OcRigRef::Kind::Bone: {
            std::string s = "bone:" + r.name;
            if (r.offset.x != 0.0f || r.offset.y != 0.0f || r.offset.z != 0.0f) s += "+" + vec3ToString(r.offset);
            return s;
        }
        case OcRigRef::Kind::Literal: break;
    }
    return vec3ToString(lit);
}

std::string quatValueStr(const OcRigRef& r, const Quat& lit) {
    return r.kind == OcRigRef::Kind::Input ? "@" + r.name : quatToString(lit);
}

bool parseBool01(const std::string& v, bool& out) {
    if (v == "1" || v == "true")  { out = true;  return true; }
    if (v == "0" || v == "false") { out = false; return true; }
    return false;
}

bool refFinite(const OcRigRef& r) {
    return std::isfinite(r.offset.x) && std::isfinite(r.offset.y) && std::isfinite(r.offset.z);
}

bool isChainKind(OcRigOpKind k) {
    return k == OcRigOpKind::TwoBoneIk || k == OcRigOpKind::Fabrik || k == OcRigOpKind::Ccd;
}

} // namespace

const OcRigInput* OcRigData::input(const std::string& n) const {
    for (const OcRigInput& i : inputs)
        if (i.name == n) return &i;
    return nullptr;
}

bool OcRigData::needsV2() const {
    if (!inputs.empty()) return true;
    for (const OcRigOp& op : ops) {
        if (op.kind != OcRigOpKind::TwoBoneIk && op.kind != OcRigOpKind::AimAt) return true;
        if (op.targetRef.kind != OcRigRef::Kind::Literal || op.hintRef.kind != OcRigRef::Kind::Literal ||
            op.rotRef.kind != OcRigRef::Kind::Literal || !op.weightInput.empty() || op.hasTipRot)
            return true;
    }
    return false;
}

bool OcRigData::valid(std::string* why) const {
    for (usize i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name.empty()) return fail(why, "an INPUT has no name");
        for (usize j = 0; j < i; ++j)
            if (inputs[j].name == inputs[i].name) return fail(why, "duplicate INPUT '" + inputs[i].name + "'");
    }
    // A reference must name an input of the type the slot wants.
    auto refOk = [&](const OcRigRef& r, OcRigInputType want, const char* slot) -> bool {
        if (r.kind == OcRigRef::Kind::Literal) return true;
        if (r.kind == OcRigRef::Kind::Bone) {
            if (r.name.empty() || !refFinite(r)) return fail(why, std::string("bad bone: reference in ") + slot);
            return want == OcRigInputType::Vec3 ? true : fail(why, std::string("bone: cannot be used for ") + slot);
        }
        const OcRigInput* in = input(r.name);
        if (!in) return fail(why, "@" + r.name + " is not a declared INPUT (" + slot + ")");
        if (in->type != want) return fail(why, "@" + r.name + " has the wrong type for " + slot);
        return true;
    };
    for (const OcRigOp& op : ops) {
        if (op.weightInput.empty() && (op.weight < 0.0f || op.weight > 1.0f)) return fail(why, "an op weight is outside [0,1]");
        if (!op.weightInput.empty()) {
            const OcRigInput* in = input(op.weightInput);
            if (!in) return fail(why, "weight=@" + op.weightInput + " is not a declared INPUT");
            if (in->type != OcRigInputType::Float) return fail(why, "weight=@" + op.weightInput + " must be a float INPUT");
        }
        if (op.root.empty()) return fail(why, "an op has no root / bone name");
        switch (op.kind) {
            case OcRigOpKind::TwoBoneIk:
                if (op.mid.empty() || op.tip.empty()) return fail(why, "a two-bone chain needs root, mid and tip");
                // A chain that names the same bone twice is not a chain.
                if (op.root == op.mid || op.mid == op.tip || op.root == op.tip) return fail(why, "a chain names a bone twice");
                break;
            case OcRigOpKind::AimAt: break;
            case OcRigOpKind::Fabrik:
            case OcRigOpKind::Ccd:
                if (op.tip.empty() || op.root == op.tip) return fail(why, "fabrik / ccd need distinct root and tip");
                if (op.iterations > 64) return fail(why, "iter must be 1..64");
                if (!std::isfinite(op.tolerance) || op.tolerance < 0.0f || !std::isfinite(op.maxStepDeg) || op.maxStepDeg < 0.0f)
                    return fail(why, "tol / step must be finite and >= 0");
                break;
            case OcRigOpKind::CopyBone:
                if (op.dst.empty()) return fail(why, "copybone needs dst");
                if (op.dst == op.root) return fail(why, "copybone copies a bone onto itself");
                break;
            case OcRigOpKind::SetBone: break;
        }
        if (!refOk(op.targetRef, OcRigInputType::Vec3, "goal / at / pos")) return false;
        if (!refOk(op.hintRef, OcRigInputType::Vec3, "pole / axis")) return false;
        if (!refOk(op.rotRef, OcRigInputType::Quat, "tiprot / rot")) return false;
        if (op.rotRef.kind == OcRigRef::Kind::Bone) return fail(why, "a rotation cannot be a bone: reference");
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
            if (t[1] != "1" && t[1] != "2") return fail(err, "unsupported .ocrig version '" + t[1] + "'" + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(err, "the first record must be OCRIG 1 or OCRIG 2" + at);

        if (t[0] == "NAME") {
            if (t.size() < 2) return fail(err, "NAME needs a value" + at);
            out.name = t[1];
            continue;
        }

        if (t[0] == "INPUT") {
            if (t.size() < 3) return fail(err, "INPUT needs a name and a type" + at);
            OcRigInput inp;
            inp.name = t[1];
            if (t[2] == "float")     inp.type = OcRigInputType::Float;
            else if (t[2] == "vec3") inp.type = OcRigInputType::Vec3;
            else if (t[2] == "quat") inp.type = OcRigInputType::Quat;
            else return fail(err, "unknown INPUT type '" + t[2] + "'" + at);
            for (usize i = 3; i < t.size(); ++i) {
                std::string v;
                if (!(v = valueOf(t[i], "space")).empty()) {
                    if (v == "world") inp.world = true;
                    else if (v == "model") inp.world = false;
                    else return fail(err, "INPUT space must be world or model" + at);
                    continue;
                }
                if (!(v = valueOf(t[i], "default")).empty()) {
                    bool ok = false;
                    if (inp.type == OcRigInputType::Float) {
                        try { inp.defFloat = std::stof(v); ok = true; } catch (...) {}
                    } else if (inp.type == OcRigInputType::Vec3) {
                        ok = parseVec3(v, inp.defVec);
                    } else {
                        ok = parseQuat(v, inp.defQuat);
                    }
                    if (!ok) return fail(err, "bad INPUT default '" + v + "'" + at);
                    continue;
                }
                return fail(err, "unrecognised INPUT attribute '" + t[i] + "'" + at);
            }
            out.inputs.push_back(std::move(inp));
            continue;
        }

        if (t[0] == "OP") {
            if (t.size() < 2) return fail(err, "OP needs a kind" + at);
            OcRigOp op;
            if (t[1] == "twobone")       op.kind = OcRigOpKind::TwoBoneIk;
            else if (t[1] == "aim")      op.kind = OcRigOpKind::AimAt;
            else if (t[1] == "fabrik")   op.kind = OcRigOpKind::Fabrik;
            else if (t[1] == "ccd")      op.kind = OcRigOpKind::Ccd;
            else if (t[1] == "copybone") op.kind = OcRigOpKind::CopyBone;
            else if (t[1] == "setbone")  op.kind = OcRigOpKind::SetBone;
            else return fail(err, "unknown op kind '" + t[1] + "'" + at);

            const bool copy = op.kind == OcRigOpKind::CopyBone, set = op.kind == OcRigOpKind::SetBone;
            if (set) { op.copyPos = false; op.copyRot = false; }   // set only what was given

            for (usize i = 2; i < t.size(); ++i) {
                std::string v;
                if (!(v = valueOf(t[i], "root")).empty())   { op.root = v; continue; }
                if (!(v = valueOf(t[i], "bone")).empty())   { op.root = v; continue; }   // aim / setbone spelling
                if (!(v = valueOf(t[i], "src")).empty())    { op.root = v; continue; }   // copybone source
                if (!(v = valueOf(t[i], "dst")).empty())    { op.dst = v;  continue; }
                if (!(v = valueOf(t[i], "mid")).empty())    { op.mid = v;  continue; }
                if (!(v = valueOf(t[i], "tip")).empty())    { op.tip = v;  continue; }
                if (!(v = valueOf(t[i], "goal")).empty() || !(v = valueOf(t[i], "at")).empty()) {
                    if (!parseVecValue(v, op.targetRef, op.target)) return fail(err, "bad value '" + v + "'" + at);
                    continue;
                }
                if (!(v = valueOf(t[i], "pole")).empty() || !(v = valueOf(t[i], "axis")).empty()) {
                    if (!parseVecValue(v, op.hintRef, op.hint)) return fail(err, "bad value '" + v + "'" + at);
                    op.hasPole = true;
                    continue;
                }
                if (!(v = valueOf(t[i], "weight")).empty()) {
                    if (v[0] == '@') {
                        if (v.size() < 2) return fail(err, "bad weight '" + v + "'" + at);
                        op.weightInput = v.substr(1);
                    } else {
                        try { op.weight = std::stof(v); } catch (...) { return fail(err, "bad weight '" + v + "'" + at); }
                    }
                    continue;
                }
                if (!(v = valueOf(t[i], "pos")).empty()) {
                    if (copy) {
                        if (!parseBool01(v, op.copyPos)) return fail(err, "pos must be 0 or 1" + at);
                    } else if (set) {
                        if (!parseVecValue(v, op.targetRef, op.target)) return fail(err, "bad value '" + v + "'" + at);
                        op.copyPos = true;
                    } else {
                        return fail(err, "pos= is only for copybone and setbone" + at);
                    }
                    continue;
                }
                if (!(v = valueOf(t[i], "rot")).empty()) {
                    if (copy) {
                        if (!parseBool01(v, op.copyRot)) return fail(err, "rot must be 0 or 1" + at);
                    } else if (set) {
                        if (!parseQuatValue(v, op.rotRef, op.rotation)) return fail(err, "bad value '" + v + "'" + at);
                        op.copyRot = true;
                    } else {
                        return fail(err, "rot= is only for copybone and setbone" + at);
                    }
                    continue;
                }
                if (!(v = valueOf(t[i], "tiprot")).empty()) {
                    if (!parseQuatValue(v, op.rotRef, op.rotation)) return fail(err, "bad value '" + v + "'" + at);
                    op.hasTipRot = true;
                    continue;
                }
                if (!(v = valueOf(t[i], "tiprotw")).empty()) {
                    try { op.rotWeight = std::stof(v); } catch (...) { return fail(err, "bad tiprotw '" + v + "'" + at); }
                    continue;
                }
                if (!(v = valueOf(t[i], "space")).empty()) {
                    if (v == "model") op.modelSpace = true;
                    else if (v == "local") op.modelSpace = false;
                    else return fail(err, "space must be model or local" + at);
                    continue;
                }
                if (!(v = valueOf(t[i], "iter")).empty()) {
                    try { op.iterations = static_cast<u32>(std::stoul(v)); } catch (...) { return fail(err, "bad iter '" + v + "'" + at); }
                    continue;
                }
                if (!(v = valueOf(t[i], "tol")).empty()) {
                    try { op.tolerance = std::stof(v); } catch (...) { return fail(err, "bad tol '" + v + "'" + at); }
                    continue;
                }
                if (!(v = valueOf(t[i], "step")).empty()) {
                    try { op.maxStepDeg = std::stof(v); } catch (...) { return fail(err, "bad step '" + v + "'" + at); }
                    continue;
                }
                if (!(v = valueOf(t[i], "limits")).empty()) {
                    if (!parseBool01(v, op.limits)) return fail(err, "limits must be 0 or 1" + at);
                    continue;
                }
                return fail(err, "unrecognised attribute '" + t[i] + "'" + at);
            }
            // The chain solvers' defaults are filled in so the written form is explicit and stable.
            if (op.kind == OcRigOpKind::Fabrik || op.kind == OcRigOpKind::Ccd) {
                const bool ccd = op.kind == OcRigOpKind::Ccd;
                if (op.iterations == 0) op.iterations = ccd ? 12u : 10u;
                if (op.tolerance == 0.0f) op.tolerance = ccd ? 0.5f : 0.1f;
                if (op.maxStepDeg == 0.0f) op.maxStepDeg = 30.0f;
            }
            out.ops.push_back(std::move(op));
            continue;
        }

        return fail(err, "unknown record '" + t[0] + "'" + at);
    }

    if (!sawHeader) return fail(err, "empty or missing OCRIG header");
    // Checked here rather than left to the caller: a rig that parses but is not valid is a file the
    // loader would accept and the runtime would then have to second-guess.
    std::string why;
    if (!out.valid(&why)) return fail(err, "the rig parsed but is not valid: " + why);
    return true;
}

bool loadOcRig(const std::string& path, OcRigData& out, std::string* err) {
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) return fail(err, "cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseOcRig(ss.str(), out, err);
}

std::string writeOcRig(const OcRigData& rig) {
    std::ostringstream o;
    o << (rig.needsV2() ? "OCRIG 2\n" : "OCRIG 1\n");
    if (!rig.name.empty()) o << "NAME " << rig.name << "\n";
    for (const OcRigInput& in : rig.inputs) {
        o << "INPUT " << in.name << " ";
        switch (in.type) {
            case OcRigInputType::Float: o << "float default=" << numToString(in.defFloat); break;
            case OcRigInputType::Vec3:
                o << "vec3 space=" << (in.world ? "world" : "model") << " default=" << vec3ToString(in.defVec);
                break;
            case OcRigInputType::Quat:
                o << "quat space=" << (in.world ? "world" : "model") << " default=" << quatToString(in.defQuat);
                break;
        }
        o << "\n";
    }
    for (const OcRigOp& op : rig.ops) {
        const char* space = op.modelSpace ? "model" : "local";
        switch (op.kind) {
            case OcRigOpKind::TwoBoneIk:
                o << "OP twobone root=" << op.root << " mid=" << op.mid << " tip=" << op.tip
                  << " goal=" << vecValueStr(op.targetRef, op.target) << " pole=" << vecValueStr(op.hintRef, op.hint);
                break;
            case OcRigOpKind::AimAt:
                o << "OP aim bone=" << op.root << " at=" << vecValueStr(op.targetRef, op.target)
                  << " axis=" << vecValueStr(op.hintRef, op.hint);
                break;
            case OcRigOpKind::Fabrik:
            case OcRigOpKind::Ccd:
                o << "OP " << (op.kind == OcRigOpKind::Ccd ? "ccd" : "fabrik") << " root=" << op.root << " tip=" << op.tip
                  << " goal=" << vecValueStr(op.targetRef, op.target);
                if (op.hasPole) o << " pole=" << vecValueStr(op.hintRef, op.hint);
                o << " iter=" << op.iterations << " tol=" << numToString(op.tolerance);
                if (op.kind == OcRigOpKind::Ccd) o << " step=" << numToString(op.maxStepDeg);
                o << " limits=" << (op.limits ? 1 : 0);
                break;
            case OcRigOpKind::CopyBone:
                o << "OP copybone dst=" << op.dst << " src=" << op.root << " space=" << space
                  << " pos=" << (op.copyPos ? 1 : 0) << " rot=" << (op.copyRot ? 1 : 0);
                break;
            case OcRigOpKind::SetBone:
                o << "OP setbone bone=" << op.root;
                if (op.copyPos) o << " pos=" << vecValueStr(op.targetRef, op.target);
                if (op.copyRot) o << " rot=" << quatValueStr(op.rotRef, op.rotation);
                o << " space=" << space;
                break;
        }
        if (isChainKind(op.kind) && op.hasTipRot) {
            o << " tiprot=" << quatValueStr(op.rotRef, op.rotation);
            if (op.rotWeight != 1.0f) o << " tiprotw=" << numToString(op.rotWeight);
        }
        if (!op.weightInput.empty()) o << " weight=@" << op.weightInput << "\n";
        else o << " weight=" << numToString(op.weight) << "\n";
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
