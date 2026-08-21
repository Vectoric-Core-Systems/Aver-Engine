// Reader and writer for .ocbt. See the header for the node/parent-index shape and why node 0 is
// always the root.

#include "aver/formats/OcBt.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkBNOD = avrFourCC("BNOD");
constexpr u32 kChunkSTRT = avrFourCC("STRT");
constexpr u32 kSubtypeBt = avrFourCC("OCBT");

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// Little-endian byte packing, copied rather than shared -- OcNav.cpp's own comment on this exact
// duplication applies here unchanged: every .oc* format carries its own tiny W/R pair.
struct W {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    i32 i32v() { return static_cast<i32>(u32v()); }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

} // namespace

bool OcBtData::valid() const {
    if (nodes.empty()) return false;
    if (nodes[0].parent != kOcBtNoParent) return false;   // node 0 IS the root, unconditionally
    for (usize i = 1; i < nodes.size(); ++i) {
        const i32 p = nodes[i].parent;
        // Every non-root node must have a real parent -- kOcBtNoParent is reserved for node 0 alone,
        // or a malformed file could describe a forest instead of a tree.
        if (p == kOcBtNoParent) return false;
        if (p < 0 || usize(p) >= nodes.size()) return false;
        if (usize(p) >= i) return false;   // PARENTS BEFORE CHILDREN, .ocskel's own contract
    }
    for (const OcBtNode& n : nodes) {
        const bool needsName = n.kind == OcBtNodeKind::Condition || n.kind == OcBtNodeKind::Action;
        if (needsName == n.name.empty()) return false;   // named iff Condition/Action, never otherwise
    }
    return true;
}

bool writeOcBt(const OcBtData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocbt: refusing to write an invalid tree -- node 0 must be the only "
                         "parentless node, every other parent index must precede its own node, and "
                         "a node is named if and only if it is a Condition or an Action");

    AvrStringTable strt;
    std::vector<u8> bnod;
    {
        W w{bnod};
        w.u32v(static_cast<u32>(in.nodes.size()));
        for (const OcBtNode& n : in.nodes) {
            w.u32v(static_cast<u32>(n.kind));
            w.i32v(n.parent);
            w.u32v(strt.add(n.name));
            for (int k = 0; k < 4; ++k) w.f32v(n.params[k]);
            w.u32v(strt.add(n.stringParam));
        }
    }

    Avr1File f;
    f.subtype = kSubtypeBt;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkBNOD, std::move(bnod), kAvrChunkRequired);
    f.add(kChunkSTRT, strt.bytes());
    return writeAvr1(f, out, why);
}

bool parseOcBt(const u8* bytes, usize size, OcBtData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kSubtypeBt) return fail(why, ".ocbt: container subtype is not OCBT");

    const AvrChunk* bnod = f.find(kChunkBNOD);
    if (!bnod) return fail(why, ".ocbt: no BNOD chunk");

    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    R r{bnod->data.data(), bnod->data.data() + bnod->data.size()};
    const u32 count = r.u32v();
    if (!r.ok) return fail(why, ".ocbt: truncated BNOD header");
    if (count == 0) return fail(why, ".ocbt: NodeCount is 0");

    out.nodes.clear();
    out.nodes.resize(count);
    for (u32 i = 0; i < count; ++i) {
        OcBtNode& n = out.nodes[i];
        const u32 kindRaw = r.u32v();
        if (kindRaw > static_cast<u32>(OcBtNodeKind::Action))
            return fail(why, ".ocbt: node " + std::to_string(i) + " has an unknown kind " +
                             std::to_string(kindRaw));
        n.kind = static_cast<OcBtNodeKind>(kindRaw);
        n.parent = r.i32v();
        n.name = std::string(strt.get(r.u32v()));
        for (int k = 0; k < 4; ++k) n.params[k] = r.f32v();
        n.stringParam = std::string(strt.get(r.u32v()));
    }
    if (!r.ok) return fail(why, ".ocbt: truncated node table");
    if (!out.valid())
        return fail(why, ".ocbt: the file parses but its tree does not hold together");
    return true;
}

bool loadOcBt(const std::string& path, OcBtData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocbt: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocbt: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocbt: short read on " + path);
    return parseOcBt(bytes.data(), bytes.size(), out, why);
}

bool saveOcBt(const std::string& path, const OcBtData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcBt(in, bytes, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // NOT atomic, matching .ocnav's own choice and for the identical reason stated there: a baked
    // behaviour tree is build output an author regenerates from the editor tab, not the one save a
    // player asked the game to keep.
    if (!writeFileBytes(path, bytes.data(), bytes.size()))
        return fail(why, ".ocbt: cannot write " + path);
    return true;
}

} // namespace aver::fmt
