// OcSequenceTest -- the level-sequence records (SEQUENCE / SEQTRACK / SEQKEY / ENDSEQUENCE) of an
// .ocworld (FORMAT_SPECS.md section 11). A sequence lives inside the level, so what matters here is that
// it round-trips byte-stably, that a track's target keeps naming the SAME placement when the writer
// renumbers placements depth-first, and that a level without sequences writes exactly what it did before.
// Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

namespace {

fmt::OcWorldPlacement place(const char* asset, i32 parent = -1) {
    fmt::OcWorldPlacement p;
    p.asset = asset;
    p.parent = parent;
    return p;
}

fmt::OcSeqKey key(f64 t, fmt::OcSeqInterp in, std::initializer_list<f64> vals) {
    fmt::OcSeqKey k;
    k.t = t;
    k.interp = in;
    usize i = 0;
    for (const f64 v : vals) k.v[i++] = v;
    return k;
}

bool sameKey(const fmt::OcSeqKey& a, const fmt::OcSeqKey& b, usize n) {
    if (a.t != b.t || a.interp != b.interp) return false;
    for (usize i = 0; i < n; ++i) if (a.v[i] != b.v[i]) return false;
    return true;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// In memory: [0] "lamp" is a child of [1] "table", which is a root; [2] "floor" is a root. The writer
// emits table, lamp, floor -- so memory index 0 (lamp) comes back as index 1, and 1 as 0.
fmt::OcWorldData makeLevel() {
    fmt::OcWorldData w;
    w.name = "SeqLevel";
    w.placements.push_back(place("lamp.ocmesh", 1));
    w.placements.push_back(place("table.ocmesh"));
    w.placements.push_back(place("floor.ocmesh"));

    fmt::OcSequence sq;
    sq.name = "Fly Through";   // a space: must survive as one token
    sq.length = 12.5;
    sq.loop = false;
    sq.autoplay = true;
    sq.camera = false;

    fmt::OcSeqTrack lamp;
    lamp.kind = fmt::OcSeqTrackKind::Transform;
    lamp.target = 0;
    lamp.keys.push_back(key(0.0, fmt::OcSeqInterp::Smooth, {1, 2, 3, 90, 0, 0, 1, 1, 1}));
    lamp.keys.push_back(key(4.5, fmt::OcSeqInterp::Linear, {10, 2, 3, 180, 15, -5, 2, 2, 2}));
    sq.tracks.push_back(lamp);

    fmt::OcSeqTrack cam;
    cam.kind = fmt::OcSeqTrackKind::Camera;
    cam.keys.push_back(key(0.0, fmt::OcSeqInterp::Smooth, {-500, 0, 150, 0, -10}));
    cam.keys.push_back(key(6.0, fmt::OcSeqInterp::Step, {500, 100, 200, 45, 0}));
    cam.keys.push_back(key(12.5, fmt::OcSeqInterp::Linear, {0, 0, 300, 90, -30}));
    sq.tracks.push_back(cam);

    fmt::OcSeqTrack glow;
    glow.kind = fmt::OcSeqTrackKind::Material;
    glow.target = 2;
    glow.keys.push_back(key(0.0, fmt::OcSeqInterp::Smooth, {1, 0.5, 0.25, 0}));
    glow.keys.push_back(key(3.0, fmt::OcSeqInterp::Smooth, {1, 1, 1, 8}));
    sq.tracks.push_back(glow);

    fmt::OcSeqTrack table;
    table.kind = fmt::OcSeqTrackKind::Transform;
    table.target = 1;
    table.keys.push_back(key(1.0, fmt::OcSeqInterp::Step, {0, 0, 0, 0, 0, 0, 1, 1, 1}));
    sq.tracks.push_back(table);

    w.sequences.push_back(sq);

    fmt::OcSequence second;
    second.name.clear();
    second.length = 3;
    w.sequences.push_back(second);
    return w;
}

} // namespace

int main() {
    AVER_INFO("=== .ocworld level sequences ===");

    const fmt::OcWorldData src = makeLevel();
    const std::string text1 = fmt::writeOcworld(src);

    fmt::OcWorldData back;
    std::string err;
    check(fmt::parseOcworld(text1, back, &err), "the written level parses: " + err);

    // ---- round trip -------------------------------------------------------------------------------
    check(back.sequences.size() == 2, "both sequences come back");
    if (back.sequences.size() == 2) {
        const fmt::OcSequence& a = back.sequences[0];
        check(a.name == "Fly Through", "the name (with a space) survives percent-encoding");
        check(a.length == 12.5 && !a.loop && a.autoplay && !a.camera, "length / loop / autoplay / camera survive");
        check(a.tracks.size() == 4, "all four tracks come back");
        if (a.tracks.size() == 4) {
            using K = fmt::OcSeqTrackKind;
            check(a.tracks[0].kind == K::Transform && a.tracks[1].kind == K::Camera &&
                  a.tracks[2].kind == K::Material && a.tracks[3].kind == K::Transform,
                  "track kinds, in order: transform, camera, material, transform");
            check(a.tracks[1].target == -1, "a camera track has no target");

            const fmt::OcSeqTrack& lamp = src.sequences[0].tracks[0];
            check(a.tracks[0].keys.size() == 2 && sameKey(a.tracks[0].keys[0], lamp.keys[0], 9) &&
                  sameKey(a.tracks[0].keys[1], lamp.keys[1], 9),
                  "transform keys round-trip (t, interp, 9 values)");
            const fmt::OcSeqTrack& cam = src.sequences[0].tracks[1];
            check(a.tracks[1].keys.size() == 3 && sameKey(a.tracks[1].keys[0], cam.keys[0], 5) &&
                  sameKey(a.tracks[1].keys[1], cam.keys[1], 5) && sameKey(a.tracks[1].keys[2], cam.keys[2], 5),
                  "camera keys round-trip (t, interp, 5 values)");
            const fmt::OcSeqTrack& glow = src.sequences[0].tracks[2];
            check(a.tracks[2].keys.size() == 2 && sameKey(a.tracks[2].keys[0], glow.keys[0], 4) &&
                  sameKey(a.tracks[2].keys[1], glow.keys[1], 4),
                  "material keys round-trip (t, interp, 4 values)");
        }
        check(back.sequences[1].name.empty() && back.sequences[1].length == 3 && back.sequences[1].tracks.empty(),
              "an unnamed, track-less second sequence round-trips");
    }

    // ---- byte stability ---------------------------------------------------------------------------
    const std::string text2 = fmt::writeOcworld(back);
    check(text1 == text2, "write(parse(write(x))) == write(x)");
    fmt::OcWorldData back2;
    check(fmt::parseOcworld(text2, back2, &err) && fmt::writeOcworld(back2) == text2,
          "and a third pass changes nothing");

    // ---- target remap -----------------------------------------------------------------------------
    // The writer emitted table, lamp, floor; each track must still name the same placement.
    check(back.placements.size() == 3 && back.placements[0].asset == "table.ocmesh" &&
          back.placements[1].asset == "lamp.ocmesh" && back.placements[2].asset == "floor.ocmesh",
          "the writer renumbered the placements depth-first (table, lamp, floor)");
    check(back.placements.size() == 3 && back.placements[1].parent == 0,
          "and the lamp is still the table's child");
    if (back.sequences.size() == 2 && back.sequences[0].tracks.size() == 4 && back.placements.size() == 3) {
        const auto& tr = back.sequences[0].tracks;
        const auto& pl = back.placements;
        check(tr[0].target == 1 && pl[static_cast<usize>(tr[0].target)].asset == "lamp.ocmesh",
              "the lamp's track (memory 0) now targets emit index 1, still the lamp");
        check(tr[3].target == 0 && pl[static_cast<usize>(tr[3].target)].asset == "table.ocmesh",
              "the table's track (memory 1) now targets emit index 0, still the table");
        check(tr[2].target == 2 && pl[static_cast<usize>(tr[2].target)].asset == "floor.ocmesh",
              "the floor's material track keeps index 2");
    }
    check(contains(text1, "SEQTRACK transform target 1") && contains(text1, "SEQTRACK transform target 0") &&
          contains(text1, "SEQTRACK material target 2") && contains(text1, "SEQTRACK camera\n"),
          "the file carries the emit indices, and no target on a camera track");

    // ---- a track whose target is out of range is dropped ------------------------------------------
    {
        fmt::OcWorldData w = makeLevel();
        w.sequences[0].tracks[0].target = 99;   // lamp track: out of range
        w.sequences[0].tracks[2].target = -1;   // material track: unset
        fmt::OcWorldData r;
        check(fmt::parseOcworld(fmt::writeOcworld(w), r, &err), "a level with dangling targets still writes and parses");
        check(r.sequences.size() == 2 && r.sequences[0].tracks.size() == 2,
              "the two dangling transform/material tracks are dropped, camera and table survive");
        if (r.sequences.size() == 2 && r.sequences[0].tracks.size() == 2) {
            check(r.sequences[0].tracks[0].kind == fmt::OcSeqTrackKind::Camera &&
                  r.sequences[0].tracks[1].kind == fmt::OcSeqTrackKind::Transform,
                  "in their original order");
        }
    }

    // ---- unsorted keys come back sorted by time, and the writer already emits them sorted -----------
    {
        fmt::OcWorldData w;
        w.name = "Unsorted";
        fmt::OcSequence sq;
        fmt::OcSeqTrack tr;
        tr.kind = fmt::OcSeqTrackKind::Camera;
        tr.keys.push_back(key(5.0, fmt::OcSeqInterp::Smooth, {5, 0, 0, 0, 0}));
        tr.keys.push_back(key(1.0, fmt::OcSeqInterp::Smooth, {1, 0, 0, 0, 0}));
        tr.keys.push_back(key(3.0, fmt::OcSeqInterp::Smooth, {3, 0, 0, 0, 0}));
        sq.tracks.push_back(tr);
        w.sequences.push_back(sq);
        const std::string a = fmt::writeOcworld(w);
        fmt::OcWorldData r;
        check(fmt::parseOcworld(a, r, &err), "unsorted-keys level parses");
        check(r.sequences.size() == 1 && r.sequences[0].tracks.size() == 1 &&
              r.sequences[0].tracks[0].keys.size() == 3 &&
              r.sequences[0].tracks[0].keys[0].t == 1.0 && r.sequences[0].tracks[0].keys[1].t == 3.0 &&
              r.sequences[0].tracks[0].keys[2].t == 5.0,
              "keys are sorted by t");
        check(fmt::writeOcworld(r) == a, "and byte-stable even when the in-memory keys were unsorted");
    }

    // ---- unknown / stray records are ignored ------------------------------------------------------
    {
        const std::string stray =
            "OCWORLD 1\n"
            "NAME Stray\n"
            "SEQKEY 1 smooth 0 0 0 0 0\n"             // before any SEQUENCE
            "SEQTRACK camera\n"                       // before any SEQUENCE
            "ENDSEQUENCE\n"                           // closes nothing
            "SEQUENCE name A length 4\n"
            "SEQKEY 2 smooth 1 1 1 1 1\n"             // no track open yet
            "SEQTRACK nonsense target 0\n"            // unknown kind
            "SEQKEY 3 smooth 9 9 9 9 9\n"             // belongs to the unknown track
            "SEQTRACK camera\n"
            "SEQKEY 1 LINEAR 1 2 3 4 5\n"             // case-insensitive keywords
            "SEQKEY 0.5 step 7 8\n"                   // short: missing values keep OcSeqKey defaults
            "ENDSEQUENCE\n"
            "SEQKEY 9 smooth 0 0 0 0 0\n"             // after ENDSEQUENCE
            "SEQTRACK camera\n"                       // after ENDSEQUENCE
            "WIBBLE 1 2 3\n"
            "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
            "SEQUENCE name B length 2\n"              // never closed: kept
            "SEQTRACK transform target 0\n"
            "SEQKEY 0 smooth 1 2 3\n";
        fmt::OcWorldData r;
        check(fmt::parseOcworld(stray, r, &err), "stray sequence records do not fail the parse: " + err);
        check(r.placements.size() == 1, "and the placement after them still parses");
        check(r.sequences.size() == 2, "two sequences: A and the unclosed B");
        if (r.sequences.size() == 2) {
            const fmt::OcSequence& a = r.sequences[0];
            check(a.name == "A" && a.length == 4 && a.loop && a.autoplay && a.camera,
                  "A: stated length kept, unstated loop/autoplay/camera keep their defaults");
            check(a.tracks.size() == 1 && a.tracks[0].kind == fmt::OcSeqTrackKind::Camera &&
                  a.tracks[0].keys.size() == 2,
                  "A: only the camera track and its two keys survive");
            if (a.tracks.size() == 1 && a.tracks[0].keys.size() == 2) {
                const auto& k0 = a.tracks[0].keys[0];
                const auto& k1 = a.tracks[0].keys[1];
                check(k0.t == 0.5 && k0.interp == fmt::OcSeqInterp::Step && k0.v[0] == 7 && k0.v[1] == 8 &&
                      k0.v[2] == 0 && k0.v[3] == 0 && k0.v[4] == 0,
                      "a short SEQKEY keeps defaults for the values it omits, and keys sort by t");
                check(k1.t == 1.0 && k1.interp == fmt::OcSeqInterp::Linear && k1.v[4] == 5,
                      "keywords parse case-insensitively");
            }
            const fmt::OcSequence& b = r.sequences[1];
            check(b.name == "B" && b.tracks.size() == 1 && b.tracks[0].target == 0 && b.tracks[0].keys.size() == 1 &&
                  b.tracks[0].keys[0].v[2] == 3,
                  "the unclosed sequence at EOF is kept with its track and key");
        }
        // A BEGIN/END pair after sequence records still belongs to the placement parser.
        const std::string nested =
            "OCWORLD 1\nSEQUENCE name N\nENDSEQUENCE\nPLACE p.ocmesh 0 0 0 0 0 0 1\nBEGIN\nCHILD c.ocmesh 1 0 0 0 0 0 1\nEND\n";
        fmt::OcWorldData n;
        check(fmt::parseOcworld(nested, n, &err) && n.placements.size() == 2 && n.placements[1].parent == 0,
              "sequence records beside BEGIN/END nesting do not disturb it");
    }

    // ---- a level without sequences writes what it always did --------------------------------------
    {
        fmt::OcWorldData w;
        w.name = "Old";
        w.placements.push_back(place("a.ocmesh"));
        w.placements.back().x = 1; w.placements.back().y = 2; w.placements.back().z = 3;
        w.placements.push_back(place("b.ocmesh", 0));
        w.placements.back().z = 5;
        const std::string out = fmt::writeOcworld(w);

        fmt::OcWorldData r;
        check(fmt::parseOcworld(out, r, &err) && r.contentId != 0, "an old-style level parses");
        char id[32];
        std::snprintf(id, sizeof id, "0x%016llX", static_cast<unsigned long long>(r.contentId));
        const std::string golden =
            std::string("OCWORLD 1\n"
                        "# Written by the Aver Engine editor. Centimetres, +X forward, +Y right, +Z up.\n"
                        "ID ") + id + "\n"
            "NAME Old\n"
            "BUILD 0\n"
            "ALGO 3\n"
            "\n"
            "PLACE a.ocmesh 1 2 3 0 0 0 1\n"
            "BEGIN\n"
            "  CHILD b.ocmesh 0 0 5 0 0 0 1\n"
            "END\n";
        check(out == golden, "no SEQUENCE block, and the placement section is exactly the old text");
        check(!contains(out, "SEQ"), "no SEQ record of any kind is written");
        check(fmt::writeOcworld(r) == out, "and it still round-trips byte-identically");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== PASS: {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== FAIL: {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
