// IES (LM-63) parser, lookup and GPU table bake. See IesProfile.hpp.
#include "aver/formats/IesProfile.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr usize kMaxAngles = 4096;
constexpr usize kMaxValues = 4u * 1024u * 1024u;

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// Keyword value for `[KEY] value` lines, else empty.
std::string keywordValue(std::string_view line, std::string_view key) {
    if (!startsWithCI(line, key)) return {};
    return std::string(trim(line.substr(key.size())));
}

// The file's horizontal angle folded into the range the table actually stores.
f32 unfoldHorizontal(const std::vector<f32>& H, f32 h) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const f32 lo = H.front(), hi = H.back();
    if (lo <= 1e-3f) {
        if (hi <= 90.5f) {                 // quadrant symmetry
            if (h > 180.0f) h = 360.0f - h;
            if (h > 90.0f)  h = 180.0f - h;
        } else if (hi <= 180.5f) {         // bilateral about the 0-180 plane
            if (h > 180.0f) h = 360.0f - h;
        }
    } else if (std::fabs(lo - 90.0f) < 1e-3f && std::fabs(hi - 270.0f) < 1e-3f) {
        if (h < 90.0f)       h = 180.0f - h;   // bilateral about the 90-270 plane
        else if (h > 270.0f) h = 540.0f - h;
    }
    return h;
}

// Bracket of `x` in ascending `a`: index i and fraction t in [0,1], clamped at the ends.
void bracket(const std::vector<f32>& a, f32 x, usize& i, f32& t) {
    if (a.size() == 1 || x <= a.front()) { i = 0; t = 0.0f; return; }
    if (x >= a.back()) { i = a.size() - 2; t = 1.0f; return; }
    const auto it = std::upper_bound(a.begin(), a.end(), x);
    i = static_cast<usize>(it - a.begin()) - 1;
    const f32 span = a[i + 1] - a[i];
    t = span > 0.0f ? (x - a[i]) / span : 0.0f;
}

} // namespace

bool parseIes(std::string_view text, IesProfile& out, std::string* err) {
    IesProfile p;

    // Header: keyword lines until TILT=. Everything after it is numbers.
    usize pos = 0;
    bool sawTilt = false;
    std::string tilt;
    while (pos < text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = trim(text.substr(pos, eol - pos));
        pos = std::min(eol + 1, text.size());
        if (line.empty()) continue;
        if (startsWithCI(line, "TILT=")) {
            sawTilt = true;
            tilt = std::string(trim(line.substr(5)));
            break;
        }
        if (line.front() == '[') {
            if (p.name.empty()) p.name = keywordValue(line, "[LUMINAIRE]");
            if (p.name.empty()) p.name = keywordValue(line, "[TEST]");
        }
    }
    if (!sawTilt) return fail(err, "not an IES file: no TILT= line");

    // Numbers: commas count as spaces (LM-63 allows both).
    std::string body(text.substr(pos));
    for (char& c : body) if (c == ',') c = ' ';
    const std::vector<std::string_view> tok = splitWhitespace(body);
    usize at = 0;
    const auto next = [&](f64& v) -> bool {
        if (at >= tok.size()) return false;
        const std::string_view t = tok[at++];
        f64 x{};
        const auto r = std::from_chars(t.data(), t.data() + t.size(), x);
        if (r.ec != std::errc{} || !std::isfinite(x)) return false;
        v = x;
        return true;
    };
    const auto bad = [&](const char* what) { return fail(err, std::string("IES: ") + what); };

    f64 v = 0.0;
    if (equalsCI(tilt, "INCLUDE")) {
        f64 n = 0.0;
        if (!next(v) || !next(n)) return bad("truncated TILT block");
        if (n < 0.0 || n > 4096.0) return bad("bad TILT pair count");
        for (i32 i = 0; i < static_cast<i32>(n) * 2; ++i)
            if (!next(v)) return bad("truncated TILT block");
    }

    f64 lamps, lumens, mult, nV, nH, ptype, units, w, l, h, ballast, future, watts;
    if (!next(lamps) || !next(lumens) || !next(mult) || !next(nV) || !next(nH) || !next(ptype) ||
        !next(units) || !next(w) || !next(l) || !next(h) || !next(ballast) || !next(future) ||
        !next(watts))
        return bad("truncated photometric header");
    if (static_cast<i32>(ptype) != 1)
        return bad("photometric type A/B is not supported; export the profile as Type C");
    if (nV < 1.0 || nH < 1.0 || nV > static_cast<f64>(kMaxAngles) || nH > static_cast<f64>(kMaxAngles) ||
        nV * nH > static_cast<f64>(kMaxValues))
        return bad("angle counts out of range");

    const usize nv = static_cast<usize>(nV), nh = static_cast<usize>(nH);
    p.lampCount = std::max(1, static_cast<i32>(lamps));
    p.lumensPerLamp = lumens > 0.0 ? static_cast<f32>(lumens) : 0.0f;
    p.multiplier = static_cast<f32>(mult > 0.0 ? mult : 1.0);
    p.inputWatts = static_cast<f32>(watts);

    p.vertical.resize(nv);
    p.horizontal.resize(nh);
    for (usize i = 0; i < nv; ++i) { if (!next(v)) return bad("truncated vertical angles"); p.vertical[i] = static_cast<f32>(v); }
    for (usize i = 0; i < nh; ++i) { if (!next(v)) return bad("truncated horizontal angles"); p.horizontal[i] = static_cast<f32>(v); }
    for (usize i = 0; i < nv; ++i) {
        if (p.vertical[i] < -1e-3f || p.vertical[i] > 180.001f) return bad("vertical angle outside 0..180");
        if (i && p.vertical[i] < p.vertical[i - 1]) return bad("vertical angles not ascending");
    }
    for (usize i = 0; i < nh; ++i) {
        if (p.horizontal[i] < -1e-3f || p.horizontal[i] > 360.001f) return bad("horizontal angle outside 0..360");
        if (i && p.horizontal[i] < p.horizontal[i - 1]) return bad("horizontal angles not ascending");
    }

    p.candela.resize(nv * nh);
    for (usize i = 0; i < p.candela.size(); ++i) {
        if (!next(v)) return bad("truncated candela table");
        const f32 c = std::max(0.0f, static_cast<f32>(v)) * p.multiplier;
        p.candela[i] = c;
        p.maxCandela = std::max(p.maxCandela, c);
    }
    if (!(p.maxCandela > 0.0f)) return bad("profile has no emission");

    out = std::move(p);
    return true;
}

bool loadIes(const std::string& path, IesProfile& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) return fail(err, "could not read " + path);
    return parseIes(text, out, err);
}

f32 iesCandela(const IesProfile& p, f32 vDeg, f32 hDeg) {
    if (!p.valid()) return 0.0f;
    const f32 eps = 1e-3f;
    if (vDeg < p.vertical.front() - eps || vDeg > p.vertical.back() + eps) return 0.0f;

    usize vi, hi;
    f32 vt, ht;
    bracket(p.vertical, vDeg, vi, vt);
    const f32 h = p.horizontal.size() == 1 ? p.horizontal.front() : unfoldHorizontal(p.horizontal, hDeg);
    bracket(p.horizontal, h, hi, ht);

    const usize nv = p.vertical.size();
    const usize v1 = std::min(vi + 1, nv - 1);
    const usize h1 = std::min(hi + 1, p.horizontal.size() - 1);
    const auto at = [&](usize hh, usize vv) { return p.candela[hh * nv + vv]; };
    const f32 a = at(hi, vi) + (at(hi, v1) - at(hi, vi)) * vt;
    const f32 b = at(h1, vi) + (at(h1, v1) - at(h1, vi)) * vt;
    return a + (b - a) * ht;
}

bool bakeIesTable(const IesProfile& p, IesTable& out) {
    out = IesTable{};
    if (!p.valid()) return false;

    std::vector<f32> raw(static_cast<usize>(kIesTableV) * kIesTableH);
    f64 weighted = 0.0, weights = 0.0;
    f32 peak = 0.0f;
    for (u32 j = 0; j < kIesTableV; ++j) {
        const f32 vDeg = static_cast<f32>(j) * 180.0f / static_cast<f32>(kIesTableV - 1);
        const f64 sinT = std::sin(static_cast<f64>(vDeg) * kPi / 180.0);
        for (u32 i = 0; i < kIesTableH; ++i) {
            const f32 hDeg = static_cast<f32>(i) * 360.0f / static_cast<f32>(kIesTableH);
            const f32 c = iesCandela(p, vDeg, hDeg);
            raw[static_cast<usize>(j) * kIesTableH + i] = c;
            weighted += static_cast<f64>(c) * sinT;
            weights += sinT;
            peak = std::max(peak, c);
        }
    }
    const f64 mean = weights > 0.0 ? weighted / weights : 0.0;
    if (!(mean > 1e-9)) return false;

    const f32 inv = static_cast<f32>(1.0 / mean);
    for (f32& c : raw) c *= inv;
    out.relative = std::move(raw);
    out.peakOverMean = peak * inv;
    return true;
}

u16 floatToHalf(f32 f) {
    u32 x;
    std::memcpy(&x, &f, sizeof x);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 mant = x & 0x7FFFFFu;
    const u32 e8 = (x >> 23) & 0xFFu;
    if (e8 == 0xFFu) return static_cast<u16>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    const i32 exp = static_cast<i32>(e8) - 127 + 15;
    if (exp >= 31) return static_cast<u16>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<u16>(sign);
        const u32 m = mant | 0x800000u;
        const u32 shift = static_cast<u32>(14 - exp);
        u32 half = m >> shift;
        const u32 rem = m & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (half & 1u))) ++half;
        return static_cast<u16>(sign | half);
    }
    u32 half = (static_cast<u32>(exp) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;   // a carry into the exponent is correct
    return static_cast<u16>(sign | half);
}

std::vector<u16> iesTableToHalf(const IesTable& t) {
    std::vector<u16> h(t.relative.size());
    for (usize i = 0; i < h.size(); ++i) h[i] = floatToHalf(t.relative[i]);
    return h;
}

} // namespace aver::fmt
