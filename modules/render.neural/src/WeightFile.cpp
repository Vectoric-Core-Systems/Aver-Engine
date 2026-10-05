#include "aver/render/neural/WeightFile.hpp"

#include "aver/render/neural/MlpReference.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <utility>

namespace aver::render::neural {

namespace {

constexpr usize kFixedWords = 8;   // magic, version, kind, headerBytes, inChannels, layerCount, flags, reserved
constexpr usize kLayerWords = 8;
constexpr u32   kFlagIoAffine = 1u;
constexpr usize kMaxFileBytes = 16u * 1024u * 1024u;   // far above 8 layers x 64 x 64 x 9 weights

const std::array<u32, 256>& crcTable() {
    static const std::array<u32, 256> t = [] {
        std::array<u32, 256> a{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            a[i] = c;
        }
        return a;
    }();
    return t;
}

void putU32(std::vector<u8>& b, u32 v) {
    const usize at = b.size();
    b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

void putF32(std::vector<u8>& b, f32 v) {
    const usize at = b.size();
    b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

// Bounds-checked little-endian reader over the file bytes.
struct Reader {
    const u8* p;
    usize size, pos = 0;
    bool ok = true;
    u32 u32v() {
        if (pos + 4 > size) { ok = false; return 0; }
        u32 v;
        std::memcpy(&v, p + pos, 4);
        pos += 4;
        return v;
    }
    f32 f32v() {
        const u32 v = u32v();
        f32 f;
        std::memcpy(&f, &v, 4);
        return f;
    }
};

bool readFile(const std::string& path, std::vector<u8>& bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    if (size < 0 || static_cast<usize>(size) > kMaxFileBytes) return false;
    bytes.resize(static_cast<usize>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return static_cast<bool>(f);
}

u32 outChannels(const ConvNetDesc& d) { return d.layers.back().cout; }

}  // namespace

u32 crc32Ieee(std::span<const u8> bytes) {
    const auto& t = crcTable();
    u32 c = 0xFFFFFFFFu;
    for (u8 b : bytes) c = t[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool peekWeightFile(const std::string& path, u32& version, u32& kind) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    u32 h[3] = {};
    f.read(reinterpret_cast<char*>(h), sizeof(u32) * 2);
    if (f.gcount() < static_cast<std::streamsize>(sizeof(u32) * 2) || h[0] != kWeightFileMagic) return false;
    if (h[1] == kWeightFileVersion) { version = h[1]; kind = kWeightKindMlp; return true; }
    f.read(reinterpret_cast<char*>(h + 2), sizeof(u32));
    if (f.gcount() < static_cast<std::streamsize>(sizeof(u32))) return false;
    version = h[1];
    kind = h[2];
    return true;
}

bool saveConvWeightFile(const std::string& path, const ConvNetDesc& d, std::span<const f32> weights,
                        const ConvIoAffine* io) {
    if (!validate(d)) return false;
    const ConvLayout lay = ConvLayout::make(d);
    if (weights.size() != lay.total) return false;
    const u32 outC = outChannels(d);
    if (io && (io->inScale.size() != d.inChannels || io->inBias.size() != d.inChannels ||
               io->outScale.size() != outC || io->outBias.size() != outC))
        return false;

    const u32 layerCount = static_cast<u32>(d.layers.size());
    const u32 headerBytes = static_cast<u32>(sizeof(u32) * (kFixedWords + kLayerWords * layerCount) +
                                             (io ? sizeof(f32) * (2u * d.inChannels + 2u * outC) : 0u));
    std::vector<u8> b;
    b.reserve(headerBytes + 4 + weights.size() * 4 + 4);
    putU32(b, kWeightFileMagic);
    putU32(b, kWeightFileVersion2);
    putU32(b, kWeightKindConvNet);
    putU32(b, headerBytes);
    putU32(b, d.inChannels);
    putU32(b, layerCount);
    putU32(b, io ? kFlagIoAffine : 0u);
    putU32(b, 0u);
    for (u32 l = 0; l < layerCount; ++l) {
        const ConvLayerDesc& c = d.layers[l];
        putU32(b, c.cin);
        putU32(b, c.cout);
        putU32(b, c.kernel);
        putU32(b, c.stride);
        putU32(b, static_cast<u32>(c.act));
        putU32(b, c.bias ? 1u : 0u);
        putU32(b, lay.layerSize[l]);
        putU32(b, 0u);
    }
    if (io) {
        for (f32 v : io->inScale) putF32(b, v);
        for (f32 v : io->inBias) putF32(b, v);
        for (f32 v : io->outScale) putF32(b, v);
        for (f32 v : io->outBias) putF32(b, v);
    }
    putU32(b, lay.total);
    for (f32 v : weights) putF32(b, v);
    putU32(b, crc32Ieee(b));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

bool loadConvWeightFile(const std::string& path, ConvNetDesc& d, std::vector<f32>& weights, ConvIoAffine* io) {
    std::vector<u8> bytes;
    if (!readFile(path, bytes) || bytes.size() < 8 + 4) return false;

    // Magic, version and kind first so a wrong file reports as one, then the CRC over all but its own word.
    Reader r{bytes.data(), bytes.size() - 4};
    if (r.u32v() != kWeightFileMagic || r.u32v() != kWeightFileVersion2 || r.u32v() != kWeightKindConvNet)
        return false;
    u32 stored;
    std::memcpy(&stored, bytes.data() + bytes.size() - 4, 4);
    if (crc32Ieee(std::span<const u8>(bytes.data(), bytes.size() - 4)) != stored) return false;

    const u32 headerBytes = r.u32v();
    ConvNetDesc nd;
    nd.inChannels = r.u32v();
    const u32 layerCount = r.u32v();
    const u32 flags = r.u32v();
    r.u32v();   // reserved
    if (!r.ok || layerCount < 1 || layerCount > kConvMaxLayers || (flags & ~kFlagIoAffine) != 0) return false;

    std::vector<u32> declared(layerCount);
    for (u32 l = 0; l < layerCount; ++l) {
        ConvLayerDesc c;
        c.cin = r.u32v();
        c.cout = r.u32v();
        c.kernel = r.u32v();
        c.stride = r.u32v();
        const u32 act = r.u32v();
        const u32 bias = r.u32v();
        declared[l] = r.u32v();
        r.u32v();   // reserved
        if (!r.ok || act > 1u || bias > 1u) return false;
        c.act = static_cast<Activation>(act);
        c.bias = bias != 0;
        nd.layers.push_back(c);
    }
    if (!validate(nd)) return false;
    const ConvLayout lay = ConvLayout::make(nd);
    for (u32 l = 0; l < layerCount; ++l)
        if (declared[l] != lay.layerSize[l]) return false;

    const u32 outC = outChannels(nd);
    ConvIoAffine affine;
    if ((flags & kFlagIoAffine) != 0) {
        affine.inScale.resize(nd.inChannels);
        affine.inBias.resize(nd.inChannels);
        affine.outScale.resize(outC);
        affine.outBias.resize(outC);
        for (std::vector<f32>* v : {&affine.inScale, &affine.inBias, &affine.outScale, &affine.outBias})
            for (f32& x : *v) x = r.f32v();
    }
    if (!r.ok || r.pos != headerBytes) return false;

    const u32 total = r.u32v();
    if (!r.ok || total != lay.total) return false;
    if (r.size - r.pos != static_cast<usize>(total) * 4) return false;   // truncated or trailing bytes
    std::vector<f32> w(total);
    std::memcpy(w.data(), bytes.data() + r.pos, static_cast<usize>(total) * 4);

    nd.seed = d.seed;   // not stored in the file; keep the caller's
    d = std::move(nd);
    weights = std::move(w);
    if (io) *io = std::move(affine);
    return true;
}

}  // namespace aver::render::neural
