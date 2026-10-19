// The saved-damage codec: round trips, refusals that never read past the buffer, the size claim, the 16-bit fallback,
// stale keys, and the tear-log replay that reconstructs a cage from its groups of broken beams.
// Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Damage.hpp"
#include "aver/softbody/Islands.hpp"
#include "aver/softbody/SoftState.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static sb::SoftState sample(u32 particles) {
    sb::SoftState s;
    s.meshKey = 0x1122334455667788ull;
    s.assetKey = 0xAABBCCDDEEFF0011ull;
    s.released = true;
    s.particleCount = particles;
    s.tearGroups = {{3, 9, 10, 400}, {2}, {7, 8, 100000}};
    s.beams = {{1, 9.5f, 0.5f, 0.0f}, {5, 10.0f, 0.0f, 3.25f}, {12345, 7.0f, 1.0f, 1.0f}};
    s.gone = {4, 5, 17};
    for (u32 i = 0; i < particles; ++i) {
        s.pos.insert(s.pos.end(), {static_cast<f32>(i) * 0.37f - 40.0f, std::sin(static_cast<f32>(i)) * 80.0f, std::cos(static_cast<f32>(i) * 0.3f) * 55.0f});
        s.vel.insert(s.vel.end(), {std::sin(static_cast<f32>(i) * 0.1f) * 120.0f, 0.0f, -static_cast<f32>(i % 7) * 9.0f});
    }
    return s;
}

// A grid cage in the XZ plane.
static sb::Cage grid(u32 cols, u32 rows) {
    sb::Cage c;
    sb::GridSpec g;
    g.cols = cols; g.rows = rows;
    g.du = Vec3{10, 0, 0}; g.dv = Vec3{0, 0, 10};
    g.material = sb::addMaterial(c, sb::Material{});
    sb::addGrid(c, g);
    sb::build(c);
    sb::recomputeMasses(c);
    return c;
}

static bool sameCage(const sb::Cage& a, const sb::Cage& b) {
    if (a.particles.size() != b.particles.size() || a.beams.size() != b.beams.size() || a.triangles.size() != b.triangles.size() ||
        a.triDead != b.triDead || a.splitLog.size() != b.splitLog.size())
        return false;
    for (usize i = 0; i < a.particles.size(); ++i) {
        const sb::Particle &p = a.particles[i], &q = b.particles[i];
        if (std::memcmp(&p.pos, &q.pos, sizeof p.pos) != 0 || p.origin != q.origin || p.pinned != q.pinned || p.freed != q.freed || p.intact != q.intact) return false;
    }
    for (usize i = 0; i < a.beams.size(); ++i) {
        const sb::Beam &p = a.beams[i], &q = b.beams[i];
        if (p.a != q.a || p.b != q.b || p.broken != q.broken) return false;
    }
    for (usize i = 0; i < a.triangles.size(); ++i)
        for (u32 k = 0; k < 3; ++k)
            if (a.triangles[i].v[k] != b.triangles[i].v[k]) return false;
    for (usize i = 0; i < a.splitLog.size(); ++i)
        if (a.splitLog[i].oldParticle != b.splitLog[i].oldParticle || a.splitLog[i].newParticle != b.splitLog[i].newParticle ||
            a.splitLog[i].triangles != b.splitLog[i].triangles)
            return false;
    return true;
}

static bool samePlan(const sb::IslandPlan& a, const sb::IslandPlan& b) {
    if (a.dust != b.dust || a.islands.size() != b.islands.size()) return false;
    for (usize i = 0; i < a.islands.size(); ++i)
        if (a.islands[i].particles != b.islands[i].particles || a.islands[i].beams != b.islands[i].beams ||
            a.islands[i].triangles != b.islands[i].triangles || a.islands[i].anchored != b.islands[i].anchored)
            return false;
    return true;
}

int main() {
    AVER_INFO("SoftStateTest");

    AVER_INFO("encode, decode, encode");
    {
        const sb::SoftState s = sample(100);
        const std::string text = sb::encodeSoftState(s);
        check(text.rfind("SB1 1122334455667788 aabbccddeeff0011 ", 0) == 0, "the header carries both keys in hex");
        sb::SoftState back;
        sb::SoftStateError err = sb::SoftStateError::Corrupt;
        check(sb::decodeSoftState(text, back, &err) && err == sb::SoftStateError::None, "it decodes");
        check(back.meshKey == s.meshKey && back.assetKey == s.assetKey && back.released && back.particleCount == 100, "keys, flag and count");
        check(back.tearGroups == s.tearGroups && back.gone == s.gone, "the tear log and the gone list");
        bool beams = back.beams.size() == 3;
        for (usize i = 0; beams && i < 3; ++i)
            beams = back.beams[i].beam == s.beams[i].beam && back.beams[i].rest == s.beams[i].rest && back.beams[i].plastic == s.beams[i].plastic && back.beams[i].damage == s.beams[i].damage;
        check(beams, "the beam deltas");
        check(back.pos == s.pos && back.vel == s.vel, "positions and velocities are exact below the quantiser threshold");
        check(sb::encodeSoftState(back) == text, "encode(decode(encode)) is byte identical");
        check(!sb::softStateStale(back, s.meshKey, s.assetKey), "the keys match");
        check(sb::softStateStale(back, s.meshKey + 1, s.assetKey) && sb::softStateStale(back, s.meshKey, s.assetKey ^ 1), "a different mesh key or asset key is stale");
    }

    AVER_INFO("an empty state");
    {
        sb::SoftState s;
        s.particleCount = 0;
        sb::SoftState back;
        check(sb::decodeSoftState(sb::encodeSoftState(s), back) && back.particleCount == 0 && back.tearGroups.empty() && !back.released, "no particles, no groups");
    }

    AVER_INFO("refusals");
    {
        const std::string text = sb::encodeSoftState(sample(40));
        sb::SoftState out;
        sb::SoftStateError err = sb::SoftStateError::None;
        std::string wrong = text;
        wrong[2] = '2';
        check(!sb::decodeSoftState(wrong, out, &err) && err == sb::SoftStateError::BadHeader, "a wrong magic is a bad header");
        check(!sb::decodeSoftState("SB1 zz", out, &err) && err == sb::SoftStateError::BadHeader, "a short header too");
        std::string badKey = text;
        badKey[6] = 'g';
        check(!sb::decodeSoftState(badKey, out, &err) && err == sb::SoftStateError::BadHeader, "a non-hex key too");
        std::string badB64 = text;
        badB64[50] = '!';
        check(!sb::decodeSoftState(badB64, out, &err) && err == sb::SoftStateError::BadBase64, "an invalid base64 character");
        check(!sb::decodeSoftState(text.substr(0, text.size() - 1), out, &err) && err == sb::SoftStateError::BadBase64, "a base64 length that is not a multiple of four");

        // Cut at every multiple of four: none may succeed or read past the end.
        bool allRefused = true;
        const usize bodyStart = 38;
        for (usize cut = bodyStart; cut < text.size(); cut += 4) allRefused = allRefused && !sb::decodeSoftState(text.substr(0, cut), out);
        check(allRefused, "every truncation is refused");
        // Padding may only close the text, so the trailing-bytes fixture needs a body that is a whole number of base64 groups.
        sb::SoftState whole = sample(40);
        while (sb::encodeSoftState(whole).back() == '=') whole.gone.push_back(whole.gone.back() + 1);
        check(!sb::decodeSoftState(sb::encodeSoftState(whole) + "AAAA", out, &err) && err == sb::SoftStateError::Corrupt, "trailing bytes are corrupt");
        check(!sb::decodeSoftState(std::string(sb::kSoftStateMaxText + 1, 'A'), out, &err) && err == sb::SoftStateError::TooLarge, "over the channel cap is refused first");

        // A flag byte with undefined bits, built by hand: "SB1 ..." + base64 of 0x80 then zeros.
        sb::SoftState s;
        std::string good = sb::encodeSoftState(s);
        const std::string head = good.substr(0, 38);
        check(!sb::decodeSoftState(head + "gAAAAAA=", out, &err), "undefined flag bits are refused");

        // Fuzz: flip every character of the base64 body one at a time; nothing may crash, and what decodes must re-encode.
        u32 survived = 0;
        for (usize i = bodyStart; i < text.size(); ++i) {
            std::string t = text;
            t[i] = t[i] == 'A' ? 'B' : 'A';
            sb::SoftState x;
            if (sb::decodeSoftState(t, x)) ++survived;
        }
        check(survived < text.size() - bodyStart, "a one-character flip is not always accepted blindly (and never crashes)");
    }

    AVER_INFO("size");
    {
        sb::SoftState s = sample(4096);
        const std::string text = sb::encodeSoftState(s);
        check(text.size() < 150u * 1024u, "a 4096-particle cage fits in 150 KB of text");
        check(text.size() < sb::kSoftStateMaxText, "far below the channel cap");
        sb::SoftState back;
        check(sb::decodeSoftState(text, back) && back.particleCount == 4096, "and decodes");
    }

    AVER_INFO("the 16-bit fallback");
    {
        const sb::SoftState s = sample(5000);
        const std::string q = sb::encodeSoftState(s, true);
        sb::SoftState back;
        check(sb::decodeSoftState(q, back) && back.particleCount == 5000, "a forced quantised block decodes");
        f32 worstPos = 0.0f, worstVel = 0.0f;
        for (usize i = 0; i < s.pos.size(); ++i) worstPos = std::max(worstPos, std::fabs(s.pos[i] - back.pos[i]));
        for (usize i = 0; i < s.vel.size(); ++i) worstVel = std::max(worstVel, std::fabs(s.vel[i] - back.vel[i]));
        AVER_INFO("  worst position error {:.5f} cm, velocity {:.5f} cm/s", worstPos, worstVel);
        check(worstPos < 0.05f, "positions stay within 0.05 cm");
        check(worstVel < 0.02f, "velocities stay within 1/32767 of the fastest");
        check(q.size() < sb::encodeSoftState(s).size(), "and it is smaller than the raw block");
        // Over the raw limit the codec switches by itself.
        const sb::SoftState big = sample(22000);
        sb::SoftState bigBack;
        check(sb::decodeSoftState(sb::encodeSoftState(big), bigBack) && bigBack.particleCount == 22000, "a cage over 512 KB of raw state takes the 16-bit path on its own");
    }

    AVER_INFO("merging tear groups");
    {
        std::vector<std::vector<u32>> groups;
        for (u32 i = 0; i < 100; ++i) groups.push_back({i * 2, i * 2 + 1});
        const auto merged = sb::mergeTearGroups(groups, 64);
        check(merged.size() <= 64 && !merged.empty(), "at most 64 groups remain");
        std::vector<u32> flat, flatMerged;
        for (const auto& g : groups) flat.insert(flat.end(), g.begin(), g.end());
        for (const auto& g : merged) flatMerged.insert(flatMerged.end(), g.begin(), g.end());
        check(flat == flatMerged, "consecutive groups, every id kept in order");
        check(sb::mergeTearGroups(groups, 200).size() == 100, "under the cap nothing is merged");
    }

    AVER_INFO("tear-log replay equality");
    {
        sb::Cage a = grid(9, 9);
        a.particles[0].pinned = true;
        a.particles[8].pinned = true;
        sb::recomputeMasses(a);
        sb::Cage b = a;   // the fresh cage the loader rebuilds: identical before any tear

        // Slits, not holes: a disc cut leaves every boundary fan in one piece and duplicates (almost) nothing, while a broken
        // path splits the fan of each vertex it passes through. Each tearWithin below breaks the one beam whose midpoint it sits on.
        struct Slit { Vec3 first; Vec3 step; u32 beams; };
        const Slit slits[3] = {{{25, 0, 40}, {10, 0, 0}, 4}, {{60, 0, 25}, {0, 0, 10}, 4}, {{15, 0, 65}, {10, 0, 0}, 5}};
        std::vector<std::vector<u32>> log;
        u32 totalCreated = 0;
        for (u32 round = 0; round < 3; ++round) {
            std::vector<u32> ids;
            for (u32 k = 0; k < slits[round].beams; ++k) sb::tearWithin(a, slits[round].first + slits[round].step * static_cast<f32>(k), 3.0f, &ids);
            totalCreated += sb::resolveTears(a);
            std::sort(ids.begin(), ids.end());
            log.push_back(ids);
        }
        check(totalCreated > 4, "three incremental rounds duplicate particles");

        // Through the codec, as a save would carry it.
        sb::SoftState s;
        s.particleCount = static_cast<u32>(a.particles.size());
        s.tearGroups = log;
        sb::SoftState back;
        check(sb::decodeSoftState(sb::encodeSoftState(s), back) && back.tearGroups == log, "the log survives the codec");

        for (const std::vector<u32>& group : back.tearGroups) {
            for (const u32 bi : group) sb::breakBeam(b, bi);
            sb::resolveTears(b);
        }
        check(sameCage(a, b), "replaying the groups gives identical particles, beam endpoints, dead triangles and split log");
        check(samePlan(sb::planIslands(a, 3), sb::planIslands(b, 3)), "and the same islands and dust");

        // One merged group reaches the same pieces, though duplicate numbering may differ.
        sb::Cage c = grid(9, 9);
        c.particles[0].pinned = true;
        c.particles[8].pinned = true;
        sb::recomputeMasses(c);
        for (const std::vector<u32>& g : sb::mergeTearGroups(log, 1))
            for (const u32 bi : g) sb::breakBeam(c, bi);
        sb::resolveTears(c);
        check(sb::planIslands(c, 3).islands.size() == sb::planIslands(a, 3).islands.size(), "a merged replay reaches the same number of pieces");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
