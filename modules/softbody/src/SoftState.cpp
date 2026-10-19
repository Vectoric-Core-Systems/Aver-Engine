// The softbody save codec. See SoftState.hpp.
#include "aver/softbody/SoftState.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aver::softbody {
namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr u32 kMaxParticles = 1u << 20;

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const u32 v = (static_cast<u32>(static_cast<u8>(in[i])) << 16) | (static_cast<u32>(static_cast<u8>(in[i + 1])) << 8) | static_cast<u8>(in[i + 2]);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i + 1 == in.size()) {
        const u32 v = static_cast<u32>(static_cast<u8>(in[i])) << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const u32 v = (static_cast<u32>(static_cast<u8>(in[i])) << 16) | (static_cast<u32>(static_cast<u8>(in[i + 1])) << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64Decode(std::string_view in, std::string& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    out.reserve(in.size() / 4 * 3);
    for (usize i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        const int a = b64Value(in[i]), b = b64Value(in[i + 1]);
        if (a < 0 || b < 0) return false;
        const bool pad2 = in[i + 2] == '=', pad3 = in[i + 3] == '=';
        if ((pad2 || pad3) && !last) return false;
        if (pad2 && !pad3) return false;
        const int c = pad2 ? 0 : b64Value(in[i + 2]);
        const int d = pad3 ? 0 : b64Value(in[i + 3]);
        if (c < 0 || d < 0) return false;
        const u32 v = (static_cast<u32>(a) << 18) | (static_cast<u32>(b) << 12) | (static_cast<u32>(c) << 6) | static_cast<u32>(d);
        out += static_cast<char>((v >> 16) & 0xFF);
        if (!pad2) out += static_cast<char>((v >> 8) & 0xFF);
        if (!pad3) out += static_cast<char>(v & 0xFF);
    }
    return true;
}

struct Writer {
    std::string b;
    void u8v(u32 v) { b += static_cast<char>(v & 0xFF); }
    void u16v(u32 v) { u8v(v); u8v(v >> 8); }
    void u32v(u32 v) { u16v(v); u16v(v >> 16); }
    void f32v(f32 f) { u32 v; std::memcpy(&v, &f, 4); u32v(v); }
    void varint(u32 v) {
        while (v >= 0x80) { u8v((v & 0x7F) | 0x80); v >>= 7; }
        u8v(v);
    }
};

struct Reader {
    const u8* p;
    usize n;
    usize i = 0;
    bool ok = true;
    usize left() const { return n - i; }
    u32 u8v() { if (i + 1 > n) { ok = false; return 0; } return p[i++]; }
    u32 u16v() { const u32 a = u8v(); return a | (u8v() << 8); }
    u32 u32v() { const u32 a = u16v(); return a | (u16v() << 16); }
    f32 f32v() { const u32 v = u32v(); f32 f; std::memcpy(&f, &v, 4); return f; }
    u32 varint() {
        u64 v = 0;
        for (u32 shift = 0; shift <= 28; shift += 7) {
            const u32 byte = u8v();
            if (!ok) return 0;
            v |= static_cast<u64>(byte & 0x7F) << shift;
            if (!(byte & 0x80)) {
                if (v > 0xFFFFFFFFull) { ok = false; return 0; }
                return static_cast<u32>(v);
            }
        }
        ok = false;
        return 0;
    }
};

void sortUnique(std::vector<u32>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

void putDeltas(Writer& w, const std::vector<u32>& ids) {
    u32 prev = 0;
    for (usize k = 0; k < ids.size(); ++k) {
        w.varint(ids[k] - (k == 0 ? 0u : prev));
        prev = ids[k];
    }
}

bool readDeltas(Reader& r, u32 count, std::vector<u32>& out) {
    out.clear();
    if (count > r.left()) return false;   // every id takes at least one byte
    out.reserve(count);
    u64 prev = 0;
    for (u32 k = 0; k < count; ++k) {
        const u32 d = r.varint();
        if (!r.ok) return false;
        if (k > 0 && d == 0) return false;   // strictly ascending
        prev = (k == 0 ? 0 : prev) + d;
        if (prev > 0xFFFFFFFFull) return false;
        out.push_back(static_cast<u32>(prev));
    }
    return true;
}

bool finite3(const f32* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

std::string hex16(u64 v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool parseHex16(std::string_view t, u64& out) {
    if (t.size() != 16) return false;
    u64 v = 0;
    for (const char c : t) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = (v << 4) | static_cast<u64>(d);
    }
    out = v;
    return true;
}

} // namespace

const char* softStateErrorText(SoftStateError e) {
    switch (e) {
        case SoftStateError::TooLarge:  return "the saved damage is larger than a state channel holds";
        case SoftStateError::BadHeader: return "not a soft-body save (the header is wrong)";
        case SoftStateError::BadBase64: return "the saved damage is not valid base64";
        case SoftStateError::Truncated: return "the saved damage ends early";
        case SoftStateError::Corrupt:   return "the saved damage is corrupt";
        default:                        return "";
    }
}

std::vector<std::vector<u32>> mergeTearGroups(const std::vector<std::vector<u32>>& groups, u32 maxGroups) {
    const u32 n = static_cast<u32>(groups.size());
    if (maxGroups == 0) maxGroups = 1;
    if (n <= maxGroups) return groups;
    const u32 per = (n + maxGroups - 1) / maxGroups;
    std::vector<std::vector<u32>> out;
    for (u32 i = 0; i < n; i += per) {
        std::vector<u32> merged;
        for (u32 k = i; k < std::min(n, i + per); ++k) merged.insert(merged.end(), groups[k].begin(), groups[k].end());
        sortUnique(merged);
        out.push_back(std::move(merged));
    }
    return out;
}

std::string encodeSoftState(const SoftState& s, bool forceQuantised) {
    Writer w;
    const u32 n = s.particleCount;
    const bool quantised = forceQuantised || static_cast<usize>(n) * 24u > kSoftStateRawLimit;
    w.u8v((s.released ? 1u : 0u) | (quantised ? 2u : 0u));
    w.u32v(n);

    w.varint(static_cast<u32>(s.tearGroups.size()));
    for (const std::vector<u32>& g : s.tearGroups) {
        std::vector<u32> ids = g;
        sortUnique(ids);
        w.varint(static_cast<u32>(ids.size()));
        putDeltas(w, ids);
    }

    std::vector<SoftStateBeam> beams = s.beams;
    std::sort(beams.begin(), beams.end(), [](const SoftStateBeam& a, const SoftStateBeam& b) { return a.beam < b.beam; });
    beams.erase(std::unique(beams.begin(), beams.end(), [](const SoftStateBeam& a, const SoftStateBeam& b) { return a.beam == b.beam; }), beams.end());
    w.varint(static_cast<u32>(beams.size()));
    u32 prev = 0;
    for (usize k = 0; k < beams.size(); ++k) {
        w.varint(beams[k].beam - (k == 0 ? 0u : prev));
        prev = beams[k].beam;
        w.f32v(beams[k].rest); w.f32v(beams[k].plastic); w.f32v(beams[k].damage);
    }

    std::vector<u32> gone = s.gone;
    sortUnique(gone);
    w.varint(static_cast<u32>(gone.size()));
    putDeltas(w, gone);

    const auto at = [&](const std::vector<f32>& v, u32 i, u32 axis) { return static_cast<usize>(i) * 3 + axis < v.size() ? v[static_cast<usize>(i) * 3 + axis] : 0.0f; };
    if (!quantised) {
        for (u32 i = 0; i < n; ++i) {
            for (u32 a = 0; a < 3; ++a) w.f32v(at(s.pos, i, a));
            for (u32 a = 0; a < 3; ++a) w.f32v(at(s.vel, i, a));
        }
    } else {
        f32 lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, maxVel = 0.0f;
        for (u32 i = 0; i < n; ++i) {
            for (u32 a = 0; a < 3; ++a) {
                const f32 p = at(s.pos, i, a);
                if (i == 0) lo[a] = hi[a] = p;
                lo[a] = std::min(lo[a], p); hi[a] = std::max(hi[a], p);
                maxVel = std::max(maxVel, std::fabs(at(s.vel, i, a)));
            }
        }
        for (u32 a = 0; a < 3; ++a) w.f32v(lo[a]);
        for (u32 a = 0; a < 3; ++a) w.f32v(hi[a]);
        w.f32v(maxVel);
        for (u32 i = 0; i < n; ++i) {
            for (u32 a = 0; a < 3; ++a) {
                const f32 span = hi[a] - lo[a];
                const f32 t = span > 0.0f ? (at(s.pos, i, a) - lo[a]) / span : 0.0f;
                w.u16v(static_cast<u32>(std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f)));
            }
            for (u32 a = 0; a < 3; ++a) {
                const f32 t = maxVel > 0.0f ? at(s.vel, i, a) / maxVel : 0.0f;
                w.u16v(static_cast<u32>(static_cast<u16>(static_cast<i16>(std::lround(std::clamp(t, -1.0f, 1.0f) * 32767.0f)))));
            }
        }
    }
    return "SB1 " + hex16(s.meshKey) + " " + hex16(s.assetKey) + " " + base64Encode(w.b);
}

bool decodeSoftState(std::string_view text, SoftState& out, SoftStateError* err) {
    const auto fail = [&](SoftStateError e) {
        if (err) *err = e;
        return false;
    };
    if (err) *err = SoftStateError::None;
    if (text.size() > kSoftStateMaxText) return fail(SoftStateError::TooLarge);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\0')) text.remove_suffix(1);
    // "SB1 " + 16 + " " + 16 + " " = 4 + 16 + 1 + 16 + 1
    if (text.size() < 38 || text.substr(0, 4) != "SB1 " || text[20] != ' ' || text[37] != ' ') return fail(SoftStateError::BadHeader);
    SoftState s;
    if (!parseHex16(text.substr(4, 16), s.meshKey) || !parseHex16(text.substr(21, 16), s.assetKey)) return fail(SoftStateError::BadHeader);
    std::string body;
    if (!base64Decode(text.substr(38), body)) return fail(SoftStateError::BadBase64);

    Reader r{reinterpret_cast<const u8*>(body.data()), body.size()};
    const u32 flags = r.u8v();
    if (!r.ok) return fail(SoftStateError::Truncated);
    if (flags & ~3u) return fail(SoftStateError::Corrupt);
    s.released = (flags & 1u) != 0;
    const bool quantised = (flags & 2u) != 0;
    s.particleCount = r.u32v();
    if (!r.ok) return fail(SoftStateError::Truncated);
    if (s.particleCount > kMaxParticles) return fail(SoftStateError::Corrupt);

    const u32 groups = r.varint();
    if (!r.ok) return fail(SoftStateError::Truncated);
    if (groups > r.left()) return fail(SoftStateError::Corrupt);
    s.tearGroups.resize(groups);
    for (u32 g = 0; g < groups; ++g) {
        const u32 count = r.varint();
        if (!r.ok) return fail(SoftStateError::Truncated);
        if (!readDeltas(r, count, s.tearGroups[g])) return fail(r.ok ? SoftStateError::Corrupt : SoftStateError::Truncated);
    }

    const u32 beamCount = r.varint();
    if (!r.ok) return fail(SoftStateError::Truncated);
    if (static_cast<u64>(beamCount) * 13u > r.left()) return fail(SoftStateError::Corrupt);
    s.beams.reserve(beamCount);
    u64 prevBeam = 0;
    for (u32 k = 0; k < beamCount; ++k) {
        const u32 d = r.varint();
        SoftStateBeam b;
        b.rest = r.f32v(); b.plastic = r.f32v(); b.damage = r.f32v();
        if (!r.ok) return fail(SoftStateError::Truncated);
        if (k > 0 && d == 0) return fail(SoftStateError::Corrupt);
        prevBeam = (k == 0 ? 0 : prevBeam) + d;
        if (prevBeam > 0xFFFFFFFFull || !std::isfinite(b.rest) || !std::isfinite(b.plastic) || !std::isfinite(b.damage)) return fail(SoftStateError::Corrupt);
        b.beam = static_cast<u32>(prevBeam);
        s.beams.push_back(b);
    }

    const u32 goneCount = r.varint();
    if (!r.ok) return fail(SoftStateError::Truncated);
    if (!readDeltas(r, goneCount, s.gone)) return fail(r.ok ? SoftStateError::Corrupt : SoftStateError::Truncated);

    const u32 n = s.particleCount;
    s.pos.assign(static_cast<usize>(n) * 3, 0.0f);
    s.vel.assign(static_cast<usize>(n) * 3, 0.0f);
    if (!quantised) {
        if (static_cast<u64>(n) * 24u > r.left()) return fail(SoftStateError::Truncated);
        for (u32 i = 0; i < n; ++i) {
            for (u32 a = 0; a < 3; ++a) s.pos[static_cast<usize>(i) * 3 + a] = r.f32v();
            for (u32 a = 0; a < 3; ++a) s.vel[static_cast<usize>(i) * 3 + a] = r.f32v();
            if (!r.ok) return fail(SoftStateError::Truncated);
            if (!finite3(&s.pos[static_cast<usize>(i) * 3]) || !finite3(&s.vel[static_cast<usize>(i) * 3])) return fail(SoftStateError::Corrupt);
        }
    } else {
        f32 lo[3], hi[3];
        for (f32& f : lo) f = r.f32v();
        for (f32& f : hi) f = r.f32v();
        const f32 maxVel = r.f32v();
        if (!r.ok) return fail(SoftStateError::Truncated);
        if (!finite3(lo) || !finite3(hi) || !std::isfinite(maxVel) || maxVel < 0.0f) return fail(SoftStateError::Corrupt);
        if (static_cast<u64>(n) * 12u > r.left()) return fail(SoftStateError::Truncated);
        for (u32 i = 0; i < n; ++i) {
            for (u32 a = 0; a < 3; ++a) s.pos[static_cast<usize>(i) * 3 + a] = lo[a] + (hi[a] - lo[a]) * (static_cast<f32>(r.u16v()) / 65535.0f);
            for (u32 a = 0; a < 3; ++a) s.vel[static_cast<usize>(i) * 3 + a] = maxVel * (static_cast<f32>(static_cast<i16>(static_cast<u16>(r.u16v()))) / 32767.0f);
        }
        if (!r.ok) return fail(SoftStateError::Truncated);
    }
    if (r.left() != 0) return fail(SoftStateError::Corrupt);
    out = std::move(s);
    return true;
}

} // namespace aver::softbody
