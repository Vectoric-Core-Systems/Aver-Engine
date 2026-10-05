#include "aver/render/denoise/Nrd2Dataset.hpp"

#include "aver/render/neural/WeightFile.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace aver::render::denoise {

namespace {

void putU32(std::vector<u8>& b, u32 v) {
    for (u32 i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}

template <typename T>
void putArray(std::vector<u8>& b, const std::vector<T>& v) {
    const usize n = v.size() * sizeof(T);
    const usize at = b.size();
    b.resize(at + n);
    if (n) std::memcpy(b.data() + at, v.data(), n);
}

struct Reader {
    const std::vector<u8>& b;
    usize at = 0;
    bool u32v(u32& v) {
        if (at + 4 > b.size()) return false;
        v = static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 | static_cast<u32>(b[at + 2]) << 16 |
            static_cast<u32>(b[at + 3]) << 24;
        at += 4;
        return true;
    }
    template <typename T>
    bool array(std::vector<T>& v, usize count) {
        const usize n = count * sizeof(T);
        if (at + n > b.size()) return false;
        v.resize(count);
        if (n) std::memcpy(v.data(), b.data() + at, n);
        at += n;
        return true;
    }
};

bool fail(std::string* why, const char* what) {
    if (why) *why = what;
    return false;
}

// Shape rules shared by the writer and the reader.
bool shapeOk(const Nrd2Pose& p, std::string* why) {
    if (p.tilesX == 0 || p.tilesY == 0) return fail(why, "empty tile grid");
    if (p.halfW != 4 * p.tilesX || p.halfH != 4 * p.tilesY) return fail(why, "half resolution is not 4 x tiles");
    if (p.frames == 0 || p.frames > 64) return fail(why, "frame count out of range");
    if (p.channels != kNrd2FeatureCount) return fail(why, "channel count is not 12");
    if (p.scene.size() > 1024) return fail(why, "scene id too long");
    return true;
}

}  // namespace

u16 nrd2F32ToF16(f32 v) {
    u32 x;
    std::memcpy(&x, &v, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 ax = x & 0x7FFFFFFFu;
    if (ax >= 0x7F800000u) return static_cast<u16>(sign | (ax > 0x7F800000u ? 0x7E00u : 0x7C00u));
    if (ax >= 0x477FF000u) return static_cast<u16>(sign | 0x7C00u);   // rounds past 65504
    if (ax < 0x38800000u) {                                            // half subnormal or zero
        if (ax < 0x33000000u) return static_cast<u16>(sign);
        const u32 e = ax >> 23;
        const u32 m = (ax & 0x7FFFFFu) | 0x800000u;
        const u32 shift = 126u - e;                                    // 14..24
        u32 h = m >> shift;
        const u32 rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<u16>(sign | h);
    }
    u32 h = ((ax - 0x38000000u) >> 13);
    const u32 rem = ax & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<u16>(sign | h);
}

f32 nrd2F16ToF32(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 x;
    if (e == 0) {
        if (m == 0) {
            x = sign;
        } else {   // subnormal: normalise
            u32 mm = m, ee = 113;
            while (!(mm & 0x400u)) { mm <<= 1; --ee; }
            x = sign | (ee << 23) | ((mm & 0x3FFu) << 13);
        }
    } else if (e == 31) {
        x = sign | 0x7F800000u | (m << 13);
    } else {
        x = sign | ((e + 112u) << 23) | (m << 13);
    }
    f32 v;
    std::memcpy(&v, &x, 4);
    return v;
}

bool writeNrd2Pose(const std::string& path, const Nrd2Pose& p, std::string* why) {
    if (!shapeOk(p, why)) return false;
    const usize tiles = static_cast<usize>(p.tilesX) * p.tilesY;
    if (p.features.size() != static_cast<usize>(p.frames) * p.channels * p.halfW * p.halfH)
        return fail(why, "feature count disagrees with the shape");
    if (p.theta.size() != 12 * tiles || p.weights.size() != 2 * tiles || p.losses.size() != kNrd2PoseLosses * tiles)
        return fail(why, "tile plane count disagrees with the shape");
    std::vector<u8> b;
    b.reserve(64 + p.scene.size() + p.features.size() * 2 + tiles * 18 * 4);
    for (u32 v : {kNrd2PoseMagic, kNrd2PoseVersion, p.stageBVersion, p.poseIndex, p.heldOut ? 1u : 0u, p.halfW,
                  p.halfH, p.tilesX, p.tilesY, p.frames, p.channels, static_cast<u32>(p.scene.size())})
        putU32(b, v);
    b.insert(b.end(), p.scene.begin(), p.scene.end());
    while (b.size() & 3u) b.push_back(0);
    putArray(b, p.features);
    putArray(b, p.theta);
    putArray(b, p.weights);
    putArray(b, p.losses);
    putU32(b, neural::crc32Ieee(b));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(why, "cannot open the file for writing");
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) return fail(why, "write failed");
    return true;
}

bool readNrd2Pose(const std::string& path, Nrd2Pose& out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(why, "cannot open the file");
    const std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52) return fail(why, "truncated header");
    Reader r{b};
    u32 magic = 0, version = 0, heldOut = 0, sceneBytes = 0;
    Nrd2Pose p;
    r.u32v(magic); r.u32v(version);
    if (magic != kNrd2PoseMagic) return fail(why, "not an N2PS file");
    if (version != kNrd2PoseVersion) return fail(why, "unsupported version");
    r.u32v(p.stageBVersion); r.u32v(p.poseIndex); r.u32v(heldOut);
    r.u32v(p.halfW); r.u32v(p.halfH); r.u32v(p.tilesX); r.u32v(p.tilesY);
    r.u32v(p.frames); r.u32v(p.channels); r.u32v(sceneBytes);
    if (heldOut > 1) return fail(why, "bad held-out flag");
    p.heldOut = heldOut != 0;
    if (sceneBytes > 1024 || r.at + sceneBytes > b.size()) return fail(why, "bad scene id");
    p.scene.assign(reinterpret_cast<const char*>(b.data() + r.at), sceneBytes);
    r.at += (sceneBytes + 3u) & ~3u;
    if (!shapeOk(p, why)) return false;
    const usize tiles = static_cast<usize>(p.tilesX) * p.tilesY;
    const usize feats = static_cast<usize>(p.frames) * p.channels * p.halfW * p.halfH;
    if (b.size() != r.at + feats * 2 + tiles * (12 + 2 + kNrd2PoseLosses) * 4 + 4)
        return fail(why, "size disagrees with the shape (truncated or trailing bytes)");
    u32 stored = 0;
    std::memcpy(&stored, b.data() + b.size() - 4, 4);
    if (neural::crc32Ieee(std::span<const u8>(b.data(), b.size() - 4)) != stored) return fail(why, "bad CRC");
    if (!r.array(p.features, feats) || !r.array(p.theta, 12 * tiles) || !r.array(p.weights, 2 * tiles) ||
        !r.array(p.losses, kNrd2PoseLosses * tiles))
        return fail(why, "truncated body");
    out = std::move(p);
    return true;
}

}  // namespace aver::render::denoise
