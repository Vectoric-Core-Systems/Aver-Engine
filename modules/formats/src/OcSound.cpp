// Reader and writer for .ocsnd. See the header for why edges are their own records and why every
// link must run from a lower node index to a higher one.

#include "aver/formats/OcSound.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkSHDR = avrFourCC("SHDR");
constexpr u32 kChunkSNOD = avrFourCC("SNOD");
constexpr u32 kChunkSLNK = avrFourCC("SLNK");
constexpr u32 kSubtypeSound = avrFourCC("SND ");   // padded to four, as .ocnav's "NAV " is

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

struct W {
    std::vector<u8>& b;
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

} // namespace

bool OcSoundData::valid() const {
    if (nodes.empty()) return false;
    if (outputNode >= nodes.size()) return false;
    if (durationSec <= 0.0f) return false;
    for (const OcSoundLink& l : links) {
        if (l.fromNode >= nodes.size() || l.toNode >= nodes.size()) return false;
        // SOURCES BEFORE CONSUMERS -- the whole cycle guard, in one comparison. See the header.
        if (l.fromNode >= l.toNode) return false;
        if (l.toInput >= ocSoundInputCount(nodes[l.toNode].kind)) return false;
    }
    return true;
}

bool writeOcSound(const OcSoundData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocsnd: refusing to write an invalid graph -- the output node must exist, "
                         "the duration must be positive, and every link must run from a lower node "
                         "index to a higher one and feed an input its target actually has");

    std::vector<u8> shdr;
    {
        W w{shdr};
        w.u32v(in.outputNode);
        w.f32v(in.durationSec);
    }

    std::vector<u8> snod;
    {
        W w{snod};
        w.u32v(static_cast<u32>(in.nodes.size()));
        for (const OcSoundNode& n : in.nodes) {
            w.u32v(static_cast<u32>(n.kind));
            for (int k = 0; k < 4; ++k) w.f32v(n.params[k]);
        }
    }

    std::vector<u8> slnk;
    {
        W w{slnk};
        w.u32v(static_cast<u32>(in.links.size()));
        for (const OcSoundLink& l : in.links) {
            w.u32v(l.fromNode);
            w.u32v(l.toNode);
            w.u32v(l.toInput);
        }
    }

    Avr1File f;
    f.subtype = kSubtypeSound;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkSHDR, std::move(shdr), kAvrChunkRequired);
    f.add(kChunkSNOD, std::move(snod), kAvrChunkRequired);
    // A graph of ONE node is legal and needs no links, so SLNK is written but not required on read.
    f.add(kChunkSLNK, std::move(slnk));
    return writeAvr1(f, out, why);
}

bool parseOcSound(const u8* bytes, usize size, OcSoundData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kSubtypeSound) return fail(why, ".ocsnd: container subtype is not SND");

    const AvrChunk* shdr = f.find(kChunkSHDR);
    const AvrChunk* snod = f.find(kChunkSNOD);
    if (!shdr) return fail(why, ".ocsnd: no SHDR chunk");
    if (!snod) return fail(why, ".ocsnd: no SNOD chunk");

    R h{shdr->data.data(), shdr->data.data() + shdr->data.size()};
    out.outputNode  = h.u32v();
    out.durationSec = h.f32v();
    if (!h.ok) return fail(why, ".ocsnd: truncated SHDR");

    R n{snod->data.data(), snod->data.data() + snod->data.size()};
    const u32 nodeCount = n.u32v();
    if (!n.ok) return fail(why, ".ocsnd: truncated SNOD header");
    if (nodeCount == 0) return fail(why, ".ocsnd: NodeCount is 0");
    // BOUNDED AGAINST THE CHUNK before resizing, matching parseOcNav's own guard: a corrupt count
    // could otherwise ask for an enormous allocation on its way to failing.
    constexpr usize kNodeBytes = 4 + 4 * 4;
    if (static_cast<u64>(nodeCount) * kNodeBytes > snod->data.size())
        return fail(why, ".ocsnd: SNOD claims " + std::to_string(nodeCount) +
                         " nodes, more than the chunk can hold");

    out.nodes.clear();
    out.nodes.resize(nodeCount);
    for (u32 i = 0; i < nodeCount; ++i) {
        const u32 kindRaw = n.u32v();
        if (kindRaw > static_cast<u32>(OcSoundNodeKind::Multiply))
            return fail(why, ".ocsnd: node " + std::to_string(i) + " has an unknown kind " +
                             std::to_string(kindRaw));
        out.nodes[i].kind = static_cast<OcSoundNodeKind>(kindRaw);
        for (int k = 0; k < 4; ++k) out.nodes[i].params[k] = n.f32v();
    }
    if (!n.ok) return fail(why, ".ocsnd: truncated node table");

    out.links.clear();
    if (const AvrChunk* slnk = f.find(kChunkSLNK)) {
        R l{slnk->data.data(), slnk->data.data() + slnk->data.size()};
        const u32 linkCount = l.u32v();
        if (!l.ok) return fail(why, ".ocsnd: truncated SLNK header");
        constexpr usize kLinkBytes = 12;
        if (static_cast<u64>(linkCount) * kLinkBytes > slnk->data.size())
            return fail(why, ".ocsnd: SLNK claims " + std::to_string(linkCount) +
                             " links, more than the chunk can hold");
        out.links.resize(linkCount);
        for (u32 i = 0; i < linkCount; ++i) {
            out.links[i].fromNode = l.u32v();
            out.links[i].toNode   = l.u32v();
            out.links[i].toInput  = l.u32v();
        }
        if (!l.ok) return fail(why, ".ocsnd: truncated link table");
    }

    if (!out.valid())
        return fail(why, ".ocsnd: the file parses but its graph does not hold together");
    return true;
}

bool loadOcSound(const std::string& path, OcSoundData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocsnd: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocsnd: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocsnd: short read on " + path);
    return parseOcSound(bytes.data(), bytes.size(), out, why);
}

bool saveOcSound(const std::string& path, const OcSoundData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcSound(in, bytes, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // NOT atomic, matching .ocnav and .ocbt for the reason those give: a procedural sound is
    // authored build output, not the one artefact a player asked the game to keep.
    if (!writeFileBytes(path, bytes.data(), bytes.size()))
        return fail(why, ".ocsnd: cannot write " + path);
    return true;
}

} // namespace aver::fmt
