#include "aver/world/PcgPoints.hpp"

#include "aver/core/Hash.hpp"

#include <cmath>
#include <cstring>

namespace aver::world {

// ---- Rand ---------------------------------------------------------------------------------------------------------------

u32 pcgHash(u32 x) {
    u32 z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}
u32 pcgHash2(i32 seed, i32 i) {
    return pcgHash(static_cast<u32>(seed) ^ pcgHash(static_cast<u32>(i)));
}
u32 pcgHash3(i32 seed, i32 x, i32 y, i32 z) {
    return pcgHash(static_cast<u32>(seed) ^
                   pcgHash(static_cast<u32>(x) ^ pcgHash(static_cast<u32>(y) ^ pcgHash(static_cast<u32>(z)))));
}
f32 pcgFloat01(u32 h) { return static_cast<f32>(h >> 8) / 16777216.0f; }
f32 pcgRange(i32 seed, i32 i, f32 lo, f32 hi) { return lo + (hi - lo) * pcgFloat01(pcgHash2(seed, i)); }
u32 pcgHashString(std::string_view s) {
    const u64 h = fnv1a64(s);
    return static_cast<u32>(h ^ (h >> 32));
}

// ---- PcgPoints ----------------------------------------------------------------------------------------------------------

void PcgPoints::reserve(usize n) {
    pos.reserve(n); rot.reserve(n); scale.reserve(n); density.reserve(n);
    seed.reserve(n); color.reserve(n); steepness.reserve(n); extent.reserve(n);
}

void PcgPoints::clear() {
    pos.clear(); rot.clear(); scale.clear(); density.clear();
    seed.clear(); color.clear(); steepness.clear(); extent.clear();
    for (auto& kv : fAttr) kv.second.clear();
    for (auto& kv : iAttr) kv.second.clear();
    for (auto& kv : vAttr) kv.second.clear();
}

PcgPoint PcgPoints::get(usize i) const {
    PcgPoint p;
    p.pos = pos[i]; p.rot = rot[i]; p.scale = scale[i]; p.density = density[i];
    p.seed = seed[i]; p.color = color[i]; p.steepness = steepness[i]; p.extent = extent[i];
    return p;
}

namespace {
template <class M, class V>
void pushAttrs(M& dst, const M& src, usize srcIndex, usize dstSizeBefore, const V& zero) {
    for (auto& kv : dst) {
        const auto it = src.find(kv.first);
        kv.second.push_back(it != src.end() ? it->second[srcIndex] : zero);
    }
    for (const auto& kv : src) {
        if (dst.count(kv.first)) continue;
        auto& col = dst[kv.first];
        col.assign(dstSizeBefore, zero);
        col.push_back(kv.second[srcIndex]);
    }
}
} // namespace

void PcgPoints::push(const PcgPoint& p) {
    pos.push_back(p.pos); rot.push_back(p.rot); scale.push_back(p.scale); density.push_back(p.density);
    seed.push_back(p.seed); color.push_back(p.color); steepness.push_back(p.steepness); extent.push_back(p.extent);
    for (auto& kv : fAttr) kv.second.push_back(0.0f);
    for (auto& kv : iAttr) kv.second.push_back(0);
    for (auto& kv : vAttr) kv.second.push_back(Vec3{});
}

void PcgPoints::appendFrom(const PcgPoints& src, usize i) {
    const usize before = size();
    pos.push_back(src.pos[i]); rot.push_back(src.rot[i]); scale.push_back(src.scale[i]);
    density.push_back(src.density[i]); seed.push_back(src.seed[i]); color.push_back(src.color[i]);
    steepness.push_back(src.steepness[i]); extent.push_back(src.extent[i]);
    pushAttrs(fAttr, src.fAttr, i, before, 0.0f);
    pushAttrs(iAttr, src.iAttr, i, before, 0);
    pushAttrs(vAttr, src.vAttr, i, before, Vec3{});
}

void PcgPoints::appendAll(const PcgPoints& src) {
    for (usize i = 0; i < src.size(); ++i) appendFrom(src, i);
}

PcgPoints PcgPoints::select(const std::vector<u32>& indices) const {
    PcgPoints out = emptyLike();
    out.reserve(indices.size());
    for (u32 i : indices) out.appendFrom(*this, i);
    return out;
}

PcgPoints PcgPoints::emptyLike() const {
    PcgPoints out;
    for (const auto& kv : fAttr) out.fAttr[kv.first];
    for (const auto& kv : iAttr) out.iAttr[kv.first];
    for (const auto& kv : vAttr) out.vAttr[kv.first];
    return out;
}

bool PcgPoints::hasScalar(std::string_view name) const {
    if (name == "density" || name == "steepness" || name == "seed") return true;
    const std::string n(name);
    return fAttr.count(n) || iAttr.count(n);
}

f32 PcgPoints::scalar(std::string_view name, usize i, f32 fallback) const {
    if (name == "density") return density[i];
    if (name == "steepness") return steepness[i];
    if (name == "seed") return static_cast<f32>(seed[i]);
    const std::string n(name);
    if (const auto it = fAttr.find(n); it != fAttr.end()) return it->second[i];
    if (const auto it = iAttr.find(n); it != iAttr.end()) return static_cast<f32>(it->second[i]);
    return fallback;
}

void PcgPoints::setScalar(std::string_view name, usize i, f32 v) {
    if (name == "density") { density[i] = v; return; }
    if (name == "steepness") { steepness[i] = v; return; }
    if (name == "seed") { seed[i] = static_cast<i32>(v); return; }
    auto& col = fAttr[std::string(name)];
    if (col.size() < size()) col.resize(size(), 0.0f);
    col[i] = v;
}

u64 PcgPoints::contentHash() const {
    u64 h = kFnv1a64OffsetBasis;
    const auto mix = [&h](const void* data, usize bytes) {
        const u8* b = static_cast<const u8*>(data);
        for (usize k = 0; k < bytes; ++k) { h ^= b[k]; h *= kFnv1a64Prime; }
    };
    const u64 n = size();
    mix(&n, sizeof n);
    const auto col = [&mix](const auto& v) { if (!v.empty()) mix(v.data(), v.size() * sizeof(v[0])); };
    col(pos); col(rot); col(scale); col(density); col(seed); col(color); col(steepness); col(extent);
    const auto named = [&](const char tag, const auto& m) {
        for (const auto& kv : m) { mix(&tag, 1); mix(kv.first.data(), kv.first.size()); col(kv.second); }
    };
    named('f', fAttr); named('i', iAttr); named('v', vAttr);   // std::map: already name-sorted
    return h;
}

// ---- PcgSpatialGrid -----------------------------------------------------------------------------------------------------

u64 PcgSpatialGrid::key(i32 x, i32 y, i32 z) {
    const auto f = [](i32 v) { return static_cast<u64>(static_cast<u32>(v + (1 << 20))) & 0x1FFFFFull; };
    return (f(x) << 42) | (f(y) << 21) | f(z);
}

void PcgSpatialGrid::range(const Vec3& p, f32 r, i32* lo, i32* hi) const {
    const f32 c[3] = {p.x, p.y, p.z};
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<i32>(std::floor((c[a] - r) / cell_));
        hi[a] = static_cast<i32>(std::floor((c[a] + r) / cell_));
    }
}

void PcgSpatialGrid::insert(const Vec3& p, u32 index) {
    cells_[key(static_cast<i32>(std::floor(p.x / cell_)), static_cast<i32>(std::floor(p.y / cell_)),
               static_cast<i32>(std::floor(p.z / cell_)))].push_back(index);
}

void PcgSpatialGrid::build(const std::vector<Vec3>& positions) {
    cells_.clear();
    for (usize i = 0; i < positions.size(); ++i) insert(positions[i], static_cast<u32>(i));
}

} // namespace aver::world
