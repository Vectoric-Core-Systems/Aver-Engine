// Golden test for the .oc* format loaders. Pass one or more .ocbeam / .ocmap paths;
// it parses each, prints a summary, and checks invariants. Exit code = failure count.
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/assets/AssetId.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Hash.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

// Loads one .ocbeam, prints what it holds, and checks it is non-empty and identified.
static void testBeam(const std::string& path) {
    AVER_INFO("=== .ocbeam: {} ===", path);
    fmt::OcBeamData b;
    std::string err;
    if (!fmt::loadOcbeam(path, b, &err)) {
        AVER_ERROR("   load failed: {}", err);
        ++g_failures;
        return;
    }
    AVER_INFO("   objectId=0x{:016X}  materials={}  nodes={}  beams={}  panels={}  parts={}",
              b.objectId, b.materials.size(), b.nodes.size(), b.beams.size(), b.panels.size(), b.parts.size());
    AVER_INFO("   importScale={:.3f}  glb={}  rig={}  collision={}  skippedRows={}",
              b.importScale, b.hasEmbeddedGlb, b.hasRig, b.hasCollision, b.skippedRows);
    if (!b.materials.empty()) {
        const auto& m = b.materials.front();
        AVER_INFO("   material[0]: {} axial={} break={}N behavior={}",
                  m.name, m.axialStiffness, m.breakForceN, fmt::beamBehaviorName(m.behavior));
    }
    check(!b.nodes.empty(), "has nodes");
    check(b.objectId != kInvalidObjectId, "objectId assigned");
    check(!b.beams.empty(), "has beams");
}

// Loads one .ocmap, prints its placements, and checks it has a collision source.
static void testMap(const std::string& path) {
    AVER_INFO("=== .ocmap: {} ===", path);
    fmt::OcMapData m;
    std::string err;
    if (!fmt::loadOcmap(path, m, &err)) {
        AVER_ERROR("   load failed: {}", err);
        ++g_failures;
        return;
    }
    AVER_INFO("   name='{}'  id=0x{:016X}  build={}  algo={}  client='{}'",
              m.name, m.contentId, m.build, m.algo, m.clientUmap);
    AVER_INFO("   surfaces={}  placements={} (place={} deform={})  ground={} killZ={:.1f}",
              m.surfaces.size(), m.placements.size(), m.placeCount(), m.deformCount(), m.hasGround, m.killZ);
    for (const auto& p : m.placements) {
        AVER_INFO("   {} '{}' id=0x{:016X} @ ({:.3f},{:.3f},{:.3f}) yaw={:.1f}{}",
                  p.deform ? "DEFORM" : "PLACE ", p.asset, p.objectId, p.x, p.y, p.z, p.yaw,
                  p.deform ? (" mat=" + p.material) : "");
    }
    std::string why;
    const bool serverValid = fmt::ocmapIsServerValid(m, &why);
    AVER_INFO("   server-valid={}{}", serverValid, serverValid ? "" : (" (" + why + ")"));

    check(!m.placements.empty() || m.hasGround, "has a collision source");
    if (m.name == "demoworld") {
        check(m.contentId == 0x376B85BC4D1A03BAull, "demoworld ID == 0x376B85BC4D1A03BA (parsed from file)");
    }
}

// Recomputes fnv1a64 over three known vectors, independent of any test file.
static void checkFnv() {
    AVER_INFO("=== fnv1a64 self-check ===");
    check(fnv1a64("demoworld") == 0x376B85BC4D1A03BAull, "fnv1a64(\"demoworld\") == 0x376B85BC4D1A03BA");
    check(fnv1a64("") == 0xCBF29CE484222325ull, "fnv1a64(\"\") == the offset basis (empty input)");
    check(fnv1a64("Meshes/sphere.ocmesh") == 672114764054563281ull,
          "fnv1a64(\"Meshes/sphere.ocmesh\") agrees with the C# ObjectId");
}


// Round-trips a .ocproject manifest: what the writer does not own must survive, and writing twice
// must reproduce the file byte for byte.
static void checkOcproject() {
    using namespace aver::fmt;

    // SkyForge's manifest as it looks on disk, hand-edits and all.
    const std::string original =
        "OCPROJECT 1\n"
        "# Created by the Aver Engine editor. This project lives OUTSIDE the engine tree and\n"
        "# references it; see the engine's docs/PROJECTS.md.\n"
        "NAME SkyForge\n"
        "ENGINE Aver 0.1.0\n"
        "CONTENT Content\n"
        "STARTMAP Maps/Default.ocworld\n"
        "# AUTHOR <your name>\n";

    ProjectDesc d;
    std::string err;
    check(parseOcproject(original, d, &err), "the existing manifest parses");
    check(d.name == "SkyForge", "with its name");
    check(!d.hasRenderSettings(), "and states no render settings at all");
    check(d.giQuality == -1, "an absent RENDER.GI is -1, not 0 -- 0 would mean 'GI off'");

    d.giQuality = 3;
    d.voxelResolution = 256;
    d.giIntensity = 1.25f;
    d.giMaxDistance = 3500.0f;
    const std::string written = writeOcproject(d, original);

    check(written.find("# Created by the Aver Engine editor.") != std::string::npos,
          "the hand-written comment block survives a write");
    check(written.find("# references it; see the engine's docs/PROJECTS.md.") != std::string::npos,
          "including its second line");
    check(written.find("# AUTHOR <your name>") != std::string::npos,
          "and a commented-out key is not resurrected as a real one");
    check(written.rfind("OCPROJECT 1", 0) == 0, "the header is still the first line");

    ProjectDesc back;
    check(parseOcproject(written, back, &err), "what was written parses again");
    check(back.name == d.name && back.startMap == d.startMap && back.contentRoot == d.contentRoot,
          "and the original keys round-trip");
    check(back.giQuality == 3 && back.voxelResolution == 256, "the new integer settings round-trip");
    check(std::fabs(back.giIntensity - 1.25f) < 1.0e-6f, "and the float ones, exactly");
    check(std::fabs(back.giMaxDistance - 3500.0f) < 1.0e-3f, "including the large one");

    const std::string again = writeOcproject(back, written);
    check(again == written, "writing an unchanged manifest reproduces it byte for byte");

    // ---- the render keys the settings pages gained, and the two new sections ------------------
    //
    // EVERY ONE OF THESE COSTS FIVE PLACES in this format -- the field, hasRenderSettings, the
    // parse branch, isOwnedKey, and the writer -- and missing the fourth is invisible until a save
    // strips a key it never replaced. The byte-stable second write below is what catches that.
    // A VERSION FROM THE FUTURE IS REFUSED, not read as version 1. The number was parsed and
    // round-tripped and never compared to anything, so an OCPROJECT 2 would have been read as if
    // every key still meant what it means today and then written back out having silently dropped
    // whatever this build did not understand. Both halves are asserted: the refusal, and that the
    // CURRENT version still opens -- a ceiling that rejects everything is the easy over-correction.
    {
        ProjectDesc future;
        std::string ferr;
        check(!parseOcproject("OCPROJECT 99\nNAME FromTheFuture\n", future, &ferr),
              "an OCPROJECT version above this build's ceiling is REFUSED");
        check(ferr.find("newer build") != std::string::npos,
              "and the message says why rather than blaming the syntax: " + ferr);
        ProjectDesc current;
        check(parseOcproject("OCPROJECT 1\nNAME Current\n", current, &ferr),
              "while the current version still parses");
    }

    {
        ProjectDesc r;
        check(parseOcproject("OCPROJECT 1\nNAME R\n"
                             "RENDER.GICONES 9\n"
                             "RENDER.REFRACTIONMODE 2\nRENDER.REFRACTIONSTRENGTH 0.75\n"
                             "RENDER.REFRACTIONEDGEFADE 0.2\n"
                             "RENDER.LODSELECT 1\nRENDER.LODTHRESHOLD 2.5\n"
                             "RENDER.OCCLUSIONCULL 1\nRENDER.DEPTHPREPASS 1\n", r, &err),
              "the five new render keys parse");
        check(r.giCones == 9 && r.refractionMode == 2, "cone count and refraction mode survive");
        check(std::fabs(r.refractionStrength - 0.75f) < 1e-6f &&
              std::fabs(r.refractionEdgeFade - 0.2f) < 1e-6f, "and both refraction floats");
        check(r.lodSelect == 1 && std::fabs(r.lodThresholdPx - 2.5f) < 1e-6f, "and LOD select plus threshold");
        check(r.occlusionCull == 1 && r.depthPrepass == 1, "and the two culling toggles");
        check(r.hasRenderSettings(), "and hasRenderSettings sees them -- it had to grow with them");

        const std::string w = writeOcproject(r, "");
        ProjectDesc b2;
        check(parseOcproject(w, b2, &err), "they write and parse back");
        check(b2.giCones == 9 && b2.occlusionCull == 1 && b2.depthPrepass == 1, "with the same values");
        check(writeOcproject(b2, w) == w, "and a second write is byte-identical -- isOwnedKey covers them");
    }
    {
        // ---- the four the UI could set and the file could not hold ----------------------------
        //
        // MSAA, mesh shaders and the GI volume were live controls in Project Settings that applied
        // immediately and were gone on the next open, because nothing captured them and no key
        // existed. RENDER.GIUPDATEINTERVAL closes a different hole: --gi-update-interval had a flag
        // and no key, while giUpdateInterval is tier-derived, so a project open could silently
        // re-derive over the flag.
        ProjectDesc n;
        check(parseOcproject("OCPROJECT 1\nNAME N\n"
                             "RENDER.MSAA 8\nRENDER.MESHSHADERS 1\n"
                             "RENDER.GIUPDATEINTERVAL 4\n"
                             "RENDER.GIVOLUME -250 100 300 1800\n", n, &err),
              "the four newest render keys parse");
        check(n.msaa == 8 && n.meshShaders == 1, "MSAA and mesh shaders survive");
        check(n.giUpdateInterval == 4, "and the GI update interval");
        check(n.hasGiVolume, "the GI volume reports itself present");
        // A NEGATIVE CENTRE COMPONENT, deliberately: this is why the volume needs a presence flag
        // rather than appendKey's "negative means unstated" rule, exactly as gravity does below.
        check(std::fabs(n.giCenter[0] + 250.0f) < 1e-3f &&
              std::fabs(n.giCenter[1] - 100.0f) < 1e-3f &&
              std::fabs(n.giCenter[2] - 300.0f) < 1e-3f, "with a negative centre component intact");
        check(std::fabs(n.giExtent - 1800.0f) < 1e-3f, "and a scalar extent -- the volume is a cube");
        check(n.hasRenderSettings(), "and hasRenderSettings grew with them");

        const std::string w = writeOcproject(n, "");
        check(w.find("RENDER.MSAA") != std::string::npos, "MSAA is written");
        check(w.find("RENDER.GIVOLUME") != std::string::npos, "and the GI volume");
        ProjectDesc b4;
        check(parseOcproject(w, b4, &err), "they parse back");
        check(b4.msaa == 8 && b4.giUpdateInterval == 4 && b4.hasGiVolume, "with the same values");
        // THE FOURTH OF THE FIVE PLACES: without an isOwnedKey entry the writer would append a
        // SECOND copy of each key beside the one it copied through, and only this catches it.
        check(writeOcproject(b4, w) == w, "and a second write is byte-identical -- isOwnedKey covers them");
    }
    {
        // RENDER.TAA: 0 is a stated value (off), unlike an absent key (engine default, on).
        ProjectDesc t;
        check(parseOcproject("OCPROJECT 1\nNAME T\nRENDER.TAA 0\n", t, &err), "RENDER.TAA parses");
        check(t.taa == 0 && t.hasRenderSettings(), "TAA off is a stated render setting");
        const std::string w = writeOcproject(t, "");
        check(w.find("RENDER.TAA 0") != std::string::npos, "and is written back");
        check(writeOcproject(t, w) == w, "once -- isOwnedKey covers it");
    }
    {
        // RENDER.NEURAA: NeuRAA edge AA; absent leaves it off.
        ProjectDesc t;
        check(parseOcproject("OCPROJECT 1\nNAME T\nRENDER.NEURAA 1\n", t, &err), "RENDER.NEURAA parses");
        check(t.neuraa == 1 && t.hasRenderSettings(), "NeuRAA on is a stated render setting");
        const std::string w = writeOcproject(t, "");
        check(w.find("RENDER.NEURAA 1") != std::string::npos, "and is written back");
        check(writeOcproject(t, w) == w, "once -- isOwnedKey covers it");
    }
    {
        // RENDER.NEURALDENOISE (retired NRD v1): still loads, states nothing, is dropped on the next save.
        const char* legacy = "OCPROJECT 1\nNAME T\nRENDER.NEURALDENOISE 1\n";
        ProjectDesc t;
        check(parseOcproject(legacy, t, &err), "a legacy RENDER.NEURALDENOISE loads");
        check(!t.hasRenderSettings(), "and states no render setting");
        check(writeOcproject(t, legacy).find("RENDER.NEURALDENOISE") == std::string::npos, "and the next save drops it");
    }
    {
        // ---- THE POST CHAIN, which the file could not hold at all ----------------------------
        //
        // docs/RUNTIME-DEDUP.md records the gap as an owner decision in one line: "A project cannot
        // author exposure/bloom/tonemap, so a shipped game uses compiled defaults." A packaged game
        // therefore rendered with rhi::PostSettings' compiled numbers no matter what the author had
        // set in the editor, because the only other channels were command-line flags no shipped
        // build passes. These four keys are that missing channel.
        ProjectDesc p;
        check(parseOcproject("OCPROJECT 1\nNAME P\n"
                             "RENDER.EXPOSURE 2.5\nRENDER.BLOOM 0.25\n"
                             "RENDER.AUTOEXPOSURE 0\nRENDER.TONEMAP 1\n", p, &err),
              "the four post keys parse");
        check(std::fabs(p.postExposure - 2.5f) < 1e-6f, "the exposure survives");
        check(std::fabs(p.postBloom - 0.25f) < 1e-6f, "and the bloom intensity");
        check(p.postAutoExposure == 0, "auto-exposure OFF is a stated value, not an absence");
        check(p.postTonemap == 1, "and the tone curve is a selector rather than another knob");
        check(p.hasRenderSettings(), "and hasRenderSettings grew with them");

        const std::string w = writeOcproject(p, "");
        ProjectDesc b;
        check(parseOcproject(w, b, &err), "they write and parse back");
        check(std::fabs(b.postExposure - 2.5f) < 1e-6f && b.postTonemap == 1, "with the same values");
        check(b.postAutoExposure == 0,
              "auto-exposure still off -- PostSettings defaults it ON, so a lost 0 turns itself back on");
        check(writeOcproject(b, w) == w, "and a second write is byte-identical -- isOwnedKey covers them");

        // A BLOOM OF EXACTLY ZERO IS THE WHOLE REASON THE SENTINEL IS NEGATIVE. Zero means "no
        // bloom", which builds no pyramid and records no pass at all (rhi::PostSettings,
        // RHI.hpp:151-152), so it is a performance decision as much as a look -- and the "0 means
        // the manifest never said" rule the integer keys could have used would have made it the one
        // value this format could not express. Asserted on a manifest whose ONLY render key is that
        // zero, because that is the case where an unstated-vs-stated mix-up has nothing else to
        // hide behind.
        ProjectDesc z;
        check(parseOcproject("OCPROJECT 1\nNAME Z\nRENDER.BLOOM 0\n", z, &err),
              "a manifest whose only render key is a zero bloom parses");
        check(z.postBloom == 0.0f, "the zero is READ as a zero, not as 'unstated'");
        check(z.hasRenderSettings(), "and RENDER.BLOOM 0 alone counts as stating render settings");
        const std::string zw = writeOcproject(z, "");
        check(zw.find("RENDER.BLOOM 0\n") != std::string::npos,
              "the zero is WRITTEN rather than skipped as if it were the sentinel");
        ProjectDesc zb;
        check(parseOcproject(zw, zb, &err), "and parses back");
        check(zb.postBloom == 0.0f, "still exactly zero after a full round trip");
        check(writeOcproject(zb, zw) == zw, "byte-stable second write");

        // AND SILENCE STAYS SILENT. An absent key leaves the sentinel and writes no line at all, so
        // saving a project cannot give it a post chain it never authored -- the same property the
        // audio mix is checked for below, and the one that keeps every manifest written before
        // these keys existed meaning exactly what it meant.
        ProjectDesc none;
        check(parseOcproject("OCPROJECT 1\nNAME N\n", none, &err), "a manifest with no post keys parses");
        check(none.postExposure < 0.0f && none.postBloom < 0.0f &&
              none.postAutoExposure == -1 && none.postTonemap == -1,
              "every post field sits at its 'not stated' sentinel");
        check(!none.hasRenderSettings(), "and the manifest states no render settings at all");
        const std::string nw = writeOcproject(none, "");
        check(nw.find("RENDER.EXPOSURE") == std::string::npos &&
              nw.find("RENDER.BLOOM") == std::string::npos &&
              nw.find("RENDER.AUTOEXPOSURE") == std::string::npos &&
              nw.find("RENDER.TONEMAP") == std::string::npos,
              "and writing it back adds no post line -- the compiled defaults stay the answer");
    }
    {
        // GRAVITY POINTS DOWN, which is exactly why it needs a presence flag and not appendKey's
        // "negative means unstated" rule. A sentinel here would make the only value anybody would
        // ever write unwritable.
        ProjectDesc g;
        check(parseOcproject("OCPROJECT 1\nNAME G\nPHYSICS.GRAVITY 0 0 -980\nPHYSICS.FIXEDSTEP 0.0083\n",
                             g, &err), "a physics section parses");
        check(g.hasGravity && std::fabs(g.gravity[2] + 980.0f) < 1e-3f, "with a NEGATIVE gravity, which a sentinel could not express");
        check(std::fabs(g.fixedStep - 0.0083f) < 1e-6f, "and the fixed step");
        check(g.hasPhysicsSettings(), "and the section reports itself present");

        const std::string w = writeOcproject(g, "");
        check(w.find("PHYSICS.GRAVITY") != std::string::npos, "gravity is written");
        ProjectDesc b3;
        check(parseOcproject(w, b3, &err), "and parses back");
        check(b3.hasGravity && std::fabs(b3.gravity[2] + 980.0f) < 1e-3f, "still negative");
        check(writeOcproject(b3, w) == w, "byte-stable second write");

        // A PARTIAL VECTOR IS NO VECTOR: two axes silently keeping a default the author thought
        // they had replaced is worse than the line being ignored.
        ProjectDesc part;
        check(parseOcproject("OCPROJECT 1\nNAME P\nPHYSICS.GRAVITY 0 0\n", part, &err),
              "a two-component gravity still parses the file");
        check(!part.hasGravity, "but is NOT taken as a gravity");
    }
    {
        // ZERO IS THE POINT for a mix: a project shipping with music muted has to be able to say
        // so, and a `< 0 means unstated` rule would quietly turn that into 1.
        ProjectDesc a;
        check(parseOcproject("OCPROJECT 1\nNAME A\nAUDIO.MASTER 0.8\nAUDIO.BUS 1 0 0.5 0.25\n", a, &err),
              "an audio mix parses");
        check(a.hasAudioMix && std::fabs(a.masterVolume - 0.8f) < 1e-6f, "with its master volume");
        check(std::fabs(a.busVolume[1]) < 1e-6f, "and a MUTED music bus, which a sentinel would have erased");
        check(std::fabs(a.busVolume[3] - 0.25f) < 1e-6f, "and the UI bus");

        const std::string w = writeOcproject(a, "");
        ProjectDesc b4;
        check(parseOcproject(w, b4, &err), "the mix writes and parses back");
        check(b4.hasAudioMix && std::fabs(b4.busVolume[1]) < 1e-6f, "music still muted after a round trip");
        check(writeOcproject(b4, w) == w, "byte-stable second write");

        ProjectDesc none;
        check(parseOcproject("OCPROJECT 1\nNAME N\n", none, &err), "a manifest with no audio parses");
        check(!none.hasAudioMix, "and reports no opinion rather than a default mix");
        check(writeOcproject(none, "").find("AUDIO.") == std::string::npos,
              "and writing it back adds no AUDIO line -- a project cannot grow a mix by being saved");
    }

    const std::string future = written + "COOKTARGET WindowsClient\n";
    ProjectDesc f;
    check(parseOcproject(future, f, &err), "a manifest with an unknown key still parses");
    const std::string refuture = writeOcproject(f, future);
    check(refuture.find("COOKTARGET WindowsClient") != std::string::npos,
          "and the unknown key survives being written back");

    ProjectDesc n;
    n.name = "Fresh";
    n.engineName = "Aver";
    n.engineMinVersion = "0.1.0";
    n.startMap = "Maps/Default.ocworld";
    const std::string fresh = writeOcproject(n, "");
    ProjectDesc nb;
    check(parseOcproject(fresh, nb, &err), "a manifest written from nothing parses");
    check(nb.name == "Fresh", "and carries its name");

    // A PREAMBLE IS NOT A MANIFEST, and writing over one must still produce a header.
    //
    // THIS BROKE NEW PROJECT COMPLETELY. Only the empty-existing branch emitted OCPROJECT; the
    // preserve-existing branch copied a header through if it found one and wrote none if it did
    // not. ProjectScaffold::manifestText passes three comment lines as `existing`, so every
    // project the editor scaffolded came out with all its keys and no header, was refused by
    // loadOcproject a moment later ("not an .ocproject: no OCPROJECT header line") from the very
    // function that had just written it, and the scaffold guard then deleted the half-made folder.
    // Creating a project simply did not work, and the test above passed the whole time because it
    // only ever wrote from "".
    {
        const std::string preamble = "# Created by the editor.\n# AUTHOR <your name>\n";
        const std::string withPre  = writeOcproject(n, preamble);
        check(withPre.rfind("OCPROJECT ", 0) == 0,
              "a manifest written over a comment-only preamble starts with the header");
        ProjectDesc pb;
        std::string perr;
        check(parseOcproject(withPre, pb, &perr),
              "and therefore parses: " + perr);
        check(pb.name == "Fresh", "carrying its name");
        check(withPre.find("# AUTHOR <your name>") != std::string::npos,
              "while still preserving the unowned lines it was given");

        // And exactly one header, not one per save: writing over its own output must be stable.
        const std::string again = writeOcproject(pb, withPre);
        usize headers = 0;
        for (usize at = again.find("OCPROJECT "); at != std::string::npos;
             at = again.find("OCPROJECT ", at + 1)) ++headers;
        check(headers == 1, "re-writing its own output keeps exactly one header, got " +
                            std::to_string(headers));
    }

    // EVERY RENDER KEY IS A THIRD LIST AWAY FROM DUPLICATING ITSELF. A key is read in
    // parseOcproject, written by appendKey, AND named in isOwnedKey -- and only the third one
    // stops the writer copying the author's existing line through as "unowned text" while
    // appending its own. Miss it and the manifest grows a second copy of the key on every single
    // save, which parses fine and looks fine until someone opens the file.
    //
    // Checked by COUNTING, and for every render key rather than only the newest two: a test that
    // asserts the value round-trips passes just as happily with the key present twice.
    ProjectDesc rs;
    rs.name = "Rendered";
    rs.engineName = "Aver";
    rs.engineMinVersion = "0.1.0";
    rs.giQuality = 2;
    rs.rayTracing = 2;
    rs.pathTracing = 0;
    rs.voxelResolution = 128;
    rs.giIntensity = 1.0f;
    rs.giMaxDistance = 2000.0f;
    rs.rtShadowRays = 1;
    rs.rtPixelsPerRayTile = 1;
    rs.rtShadowDenoise = 0;
    rs.rtRenderMode = 0;
    rs.ptBounces = 1;
    rs.ptMode = 1;
    // THESE TWO ARE THE REASON THE LOOP BELOW MISSED A LIVE BUG. Both are skipped by appendKey
    // unless set -- backend is guarded on !empty(), frameBudgetMs defaults to -1.0f and the f32
    // overload returns early on a negative -- so a fixture that leaves them at their defaults
    // never emits the keys, and a duplication check that never sees a key cannot catch it
    // duplicating. They are exactly the two newest render settings.
    rs.backend = "vulkan";
    rs.frameBudgetMs = 16.7f;
    // THE POST KEYS JOIN THE DUPLICATION SWEEP, with bloom at a deliberate ZERO: that is the value
    // appendKey emits where every other unset key is skipped, so it is the one that would prove a
    // missing isOwnedKey entry by growing a second copy on every save while still reading back
    // correctly. A value the writer never emits cannot be caught duplicating itself.
    rs.postExposure = 2.0f;
    rs.postBloom = 0.0f;
    rs.postAutoExposure = 0;
    rs.postTonemap = 0;
    check(rs.hasRenderSettings(), "a desc stating render settings says so");

    const std::string once = writeOcproject(rs, "");
    const std::string twice = writeOcproject(rs, once);
    const std::string thrice = writeOcproject(rs, twice);
    check(twice == once && thrice == twice,
          "re-saving an unchanged manifest with every render key set is byte-stable");

    const auto countKey = [](const std::string& text, const char* key) {
        usize n = 0;
        for (usize at = 0; (at = text.find(key, at)) != std::string::npos; ++at) ++n;
        return n;
    };
    for (const char* key : {"RENDER.GI ", "RENDER.RAYTRACING", "RENDER.PATHTRACING",
                             "RENDER.VOXELRES", "RENDER.GIINTENSITY", "RENDER.GIDISTANCE",
                             "RENDER.RTSHADOWRAYS", "RENDER.RTPIXELSPERRAY",
                             "RENDER.RTSHADOWDENOISE", "RENDER.RTRENDERMODE", "RENDER.PTBOUNCES",
                             "RENDER.PTMODE", "RENDER.BACKEND", "RENDER.FRAMEBUDGETMS",
                             "RENDER.EXPOSURE", "RENDER.BLOOM", "RENDER.AUTOEXPOSURE",
                             "RENDER.TONEMAP"}) {
        check(countKey(thrice, key) == 1,
              std::string("after three saves, ") + key + " appears exactly once");
    }

    ProjectDesc rb;
    check(parseOcproject(thrice, rb, &err), "the thrice-written manifest still parses");
    check(rb.rtRenderMode == 0 && rb.ptBounces == 1 && rb.ptMode == 1,
          "and the ray-driven keys read back the values they were written with");
    check(rb.rtShadowDenoise == 0 && rb.rtShadowRays == 1,
          "alongside the RT keys that predate them");
    check(rb.backend == "vulkan", "and the backend the author chose survives three saves");
    check(rb.postBloom == 0.0f && rb.postAutoExposure == 0,
          "and a zero bloom with auto-exposure off survives three saves AS ZEROS, not as 'unstated'");

    // THE SYMPTOM A USER ACTUALLY SEES, which counting alone does not describe: a CHANGED value
    // reverting. An unowned duplicate is not merely untidy -- the writer splices its owned block in
    // at the first owned key (near NAME) while the author's original line stays further down, and
    // parseOcproject is last-write-wins, so the OLD line is the one that wins on reload. Switching
    // the renderer would appear to work, save, and come back as the previous choice.
    ProjectDesc changed = rs;
    changed.backend = "d3d12";
    changed.frameBudgetMs = 8.3f;
    const std::string afterChange = writeOcproject(changed, once);
    ProjectDesc reread;
    check(parseOcproject(afterChange, reread, &err), "a manifest saved after a change parses");
    check(reread.backend == "d3d12",
          "and CHANGING the renderer sticks -- the previous value must not win on reload");
    check(reread.frameBudgetMs > 8.0f && reread.frameBudgetMs < 8.6f,
          "and so does changing the frame budget");

    // A ZERO IS A REAL ANSWER, NOT AN ABSENT KEY -- the same distinction the giQuality check at
    // the top of this function makes. rtRenderMode 0 means "the rasteriser finds the first
    // surface", which is a choice; -1 means the manifest never said.
    ProjectDesc silent;
    check(parseOcproject("OCPROJECT 1\nNAME Quiet\n", silent, &err), "a bare manifest parses");
    check(silent.rtRenderMode == -1 && silent.ptBounces == -1,
          "an absent ray-driven key is -1, not 0 -- 0 would mean 'raster, deliberately'");
    check(!silent.hasRenderSettings(), "and it states no render settings");
}

// Checks the .ocworld environment records: SUN in both spellings, the new SKY record, and the
// round trip. The reader and the writer had NO coverage at all before this.
static void checkOcworld() {
    AVER_INFO("=== .ocworld environment records ===");
    using namespace fmt;
    std::string err;

    {
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "SUN dir -0.3 -0.4 -0.85 color 1 0.98 0.92 lux 90000\n"
                           "SKY model physical mie 0.008 multiscatter 1.4 steps 24 aerial 6\n"
                           "FOG exp density 0.00014 color 0.7 0.78 0.88\n", w, &err),
              "a world with SUN, SKY and FOG parses");
        check(w.hasSun && w.hasSky && w.hasFog, "and reports all three present");
        check(std::fabs(w.sunDir[2] + 0.85) < 1e-9, "the sun vector is taken verbatim");
        check(w.sunLux == 90000.0, "including its lux");
        check(w.skyPhysical, "the sky model reads as physical");
        check(std::fabs(w.skyMieScatter - 0.008) < 1e-12, "the Mie override survives");
        check(w.skyViewSteps == 24 && w.skyAerialSteps == 6, "and both step counts");
    }
    {
        // Degrees are the form a person authors a time of day in, so they must reach the vector.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSUN elev 30 azim 90\n", w, &err),
              "SUN accepts elevation and azimuth instead of a vector");
        check(std::fabs(w.sunDir[2] - 0.5) < 1e-6, "30 degrees of elevation puts z at sin(30)");
        check(std::fabs(w.sunDir[0]) < 1e-6 && std::fabs(w.sunDir[1] - std::cos(30.0 * 3.14159265358979 / 180.0)) < 1e-6,
              "and a 90-degree bearing puts the rest on +Y");
    }
    {
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSKY model authored\n", w, &err), "an authored sky parses");
        check(!w.skyPhysical, "and selects the two-colour dome");
    }
    {
        // The record is optional and must stay so: every level written before it existed has none.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "a world with no SUN or SKY still parses");
        check(!w.hasSun && !w.hasSky, "and says so rather than inventing defaults as authored values");
    }
    {
        OcWorldData w;
        w.name = "RoundTrip";
        w.hasSun = true;  w.sunDir[0] = -0.5481; w.sunDir[1] = 0.3838; w.sunDir[2] = 0.7431;
        w.hasSky = true;  w.skyPhysical = true; w.skyMieScatter = 0.004; w.skyViewSteps = 32;
        w.hasFog = true;  w.fogDensity = 4e-6;
        const std::string text = writeOcworld(w);
        OcWorldData b;
        check(parseOcworld(text, b, &err), "what the writer produced parses again");
        check(b.hasSun && b.hasSky, "with both records still present");
        for (int i = 0; i < 3; ++i)
            check(std::fabs(b.sunDir[i] - w.sunDir[i]) < 1e-9, "the sun vector round-trips exactly");
        check(b.skyPhysical && b.skyViewSteps == 32, "and so does the sky model");
        // The elevation rides along as a comment for a reader; it must not be read back as data.
        check(text.find("# elev") != std::string::npos, "the written SUN line carries a readable elevation");
        check(writeOcworld(b) == text, "and a second write reproduces the first byte for byte");
    }

    // ---- the environment fields added for the editor's Details panel -------------------------
    //
    // Roughly forty controls under Sun, Sky and Fog were live in the viewport and lost on exit,
    // because the format had five of them. These check the ones whose failure mode is silent.
    {
        // EVERY NEW TOKEN, GIVEN EXPLICITLY, so nothing below is a struct default in disguise.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "SUN dir 0 0 1 lux 50000 kelvin 6500 angular 1.25\n"
                           "SKY model authored zenith 0.1 0.2 0.3 horizon 0.4 0.5 0.6 dome 0.8"
                           " ground 0.11 0.12 0.13 groundblend 0.5 skylight 1.75"
                           " mieextinction 0.0031 miephase 0.7 rayleighkm 9 miekm 1.4"
                           " planetkm 6000 airkm 55\n"
                           "FOG exp density 8e-6 color 0.7 0.8 0.9 falloff 0.004 height 1200"
                           " start 250 maxopacity 0.85\n"
                           "CLOUDS on coverage 0.7 density 2 bottom 90000 top 210000"
                           " feature 32000 wind 12 -34\n", w, &err),
              "a world with every environment token parses");
        check(w.sunTemperatureK == 6500.0 && w.sunAngularDeg == 1.25, "the sun's temperature and disk size survive");
        check(w.skyZenith[0] == 0.1 && w.skyHorizon[2] == 0.6 && w.skyDomeExponent == 0.8,
              "so does the authored dome");
        check(w.skyGroundAlbedo[1] == 0.12 && w.skyGroundBlend == 0.5 && w.skyLight == 1.75,
              "and the ground and the sky light");
        check(w.skyMiePhaseG == 0.7 && w.skyRayleighKm == 9.0 && w.skyPlanetKm == 6000.0,
              "and the air parameters");
        check(w.fogFalloff == 0.004 && w.fogHeight == 1200.0 && w.fogStart == 250.0 && w.fogMaxOpacity == 0.85,
              "and all four height-fog values");
        check(w.hasClouds && w.cloudsEnabled && w.cloudCoverage == 0.7 && w.cloudTop == 210000.0,
              "and the CLOUDS record");
        // A NEGATIVE WIND IS THE POINT: cloudWind is a direction, so any `>= 0 means set` rule
        // would have made half the compass unauthorable.
        check(w.cloudWind[0] == 12.0 && w.cloudWind[1] == -34.0, "including a negative wind component");
    }
    {
        // "SPOKE ABOUT CLOUDS" AND "HAS CLOUDS" ARE TWO FACTS. Collapsing them would make an
        // authored overcast level that the author turned OFF come back overcast on the next load,
        // because "off" and "never mentioned" would be the same state and the writer omits the
        // second. This is the check that a single bool cannot pass.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nCLOUDS off coverage 0.9\n", w, &err), "CLOUDS off parses");
        check(w.hasClouds && !w.cloudsEnabled, "and says the level SPOKE about clouds and turned them off");
        const std::string text = writeOcworld(w);
        check(text.find("CLOUDS off") != std::string::npos, "so the written file still carries the record");

        OcWorldData none;
        check(parseOcworld("OCWORLD 1\nNAME T\n", none, &err), "a world with no CLOUDS parses");
        check(!none.hasClouds, "and reports no opinion rather than 'clouds off'");
        check(writeOcworld(none).find("CLOUDS") == std::string::npos,
              "and writing it back adds no CLOUDS line -- a level cannot grow a record by being saved");
    }
    {
        // ZERO IS AN AUTHORED VALUE for fogHeight, fogStart and the cloud bounds -- they are world
        // Z and a distance. The token's PRESENCE is the flag, so a zero must round-trip as a zero
        // rather than being read back as the engine's default.
        OcWorldData w;
        w.name = "Zeroes";
        w.hasFog = true;  w.fogHeight = 0.0; w.fogStart = 0.0; w.fogFalloff = 0.0; w.fogMaxOpacity = 0.0;
        w.hasSky = true;  w.skyGroundBlend = 0.0; w.skyLight = 0.0;
        w.hasClouds = true; w.cloudBottom = 0.0; w.cloudWind[0] = 0.0; w.cloudWind[1] = 0.0;
        OcWorldData b;
        check(parseOcworld(writeOcworld(w), b, &err), "a world whose environment is all zeroes parses");
        check(b.fogHeight == 0.0 && b.fogStart == 0.0 && b.fogMaxOpacity == 0.0,
              "and a zero fog height, start and max opacity come back as zero, not as defaults");
        check(b.skyGroundBlend == 0.0 && b.skyLight == 0.0, "so does a ground blend and sky light of zero");
        check(b.cloudBottom == 0.0 && b.cloudWind[0] == 0.0, "and a zero cloud base and wind");
    }
    {
        // BYTE-STABLE SECOND WRITE over the full environment. This is the check that catches BOTH
        // halves of the mirror hazard for free: a token added to the parser and not the writer
        // disappears here, and one added to the writer and not the parser comes back different.
        OcWorldData w;
        w.name = "EnvRoundTrip";
        w.hasSun = true;  w.sunTemperatureK = 5200.0; w.sunAngularDeg = 0.61;
        w.hasSky = true;  w.skyPhysical = false; w.skyZenith[1] = 0.375; w.skyLight = 1.4;
                          w.skyMieExtinction = 0.0033; w.skyPlanetKm = 6100.0;
        w.hasFog = true;  w.fogFalloff = 0.0021; w.fogHeight = -400.0; w.fogMaxOpacity = 0.9;
        w.hasClouds = true; w.cloudsEnabled = true; w.cloudFeatureSize = 24000.0; w.cloudWind[1] = -88.0;
        const std::string text = writeOcworld(w);
        OcWorldData b;
        check(parseOcworld(text, b, &err), "the full environment writes and parses again");
        check(writeOcworld(b) == text, "and a second write reproduces the first byte for byte");
        // A NEGATIVE FOG HEIGHT is a level whose fog thins going up from below sea level, which is
        // ordinary; it is also the value a `>= 0` sentinel would have silently discarded.
        check(b.fogHeight == -400.0, "including a fog height below zero");
    }
    {
        // OLD FILES ARE THE COMMON CASE. Every level in the tree predates these tokens, and each
        // must load with the engine's own defaults rather than with zeroes -- which is what makes
        // "the token's presence is the flag" safe in the first place.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSUN dir 0 0 1 lux 90000\nSKY model physical\n"
                           "FOG exp density 4e-6\n", w, &err),
              "a world written before these tokens existed still parses");
        check(w.sunTemperatureK == 0.0, "an absent kelvin leaves the sun on its authored colour");
        check(w.sunAngularDeg == 0.545, "an absent angular size keeps the real sun's disk");
        check(w.fogMaxOpacity == 1.0, "an absent max opacity does not become zero and erase the fog");
        check(w.skyLight == 1.0, "an absent sky light does not put the ambient term at zero");
        check(!w.hasClouds, "and no CLOUDS record means no opinion about clouds");
    }
}

// Checks the BEGIN / CHILD / END nesting: that a hierarchy survives a round trip, that the records
// do not disturb a material the way braces would have, that a second write is byte-stable, and that
// an unbalanced file FAILS with a message rather than loading a wrong scene.
static void checkOcworldNesting() {
    AVER_INFO("=== .ocworld BEGIN/CHILD/END nesting ===");
    using namespace fmt;
    std::string err;

    const char* kNested =
        "OCWORLD 1\nNAME Nest\n"
        "PLACE  Meshes/table.ocmesh  0 0 0  0 0 0  1  M_Wood\n"
        "BEGIN\n"
        "  CHILD  Meshes/lamp.ocmesh  10 0 80  0 0 0  1  M_Brass\n"
        "  BEGIN\n"
        "    CHILD  Meshes/bulb.ocmesh  0 0 12  0 0 0  1  M_Glass\n"
        "  END\n"
        "  CHILD  Meshes/book.ocmesh  -5 0 80  0 0 0  1  M_Paper\n"
        "END\n";

    {
        OcWorldData w;
        check(parseOcworld(kNested, w, &err), "a nested world parses");
        check(w.placements.size() == 4, "with all four placements, flat in memory");
        check(w.placements[0].parent == -1, "the table is a root");
        check(w.placements[1].parent == 0, "the lamp hangs from the table");
        check(w.placements[2].parent == 1, "the bulb hangs from the LAMP, not the table -- BEGIN "
                                           "attaches to the most recent placement, so depth is explicit");
        check(w.placements[3].parent == 0, "and the book is the lamp's SIBLING, back at the table");

        // THE PROPERTY BRACES WOULD HAVE BROKEN. `PLACE ... {` hits PLACE's material catch-all in a
        // build that predates nesting, so the placement's material silently becomes "{" and the next
        // save writes that back. Word records cannot do that: they are separate lines.
        check(w.placements[0].material == "M_Wood"  && w.placements[1].material == "M_Brass",
              "every material is intact -- no record was swallowed as a surface name");
        check(w.placements[2].material == "M_Glass" && w.placements[3].material == "M_Paper",
              "including the deepest child's and the sibling's");
        check(w.placements[1].z == 80.0 && w.placements[2].z == 12.0,
              "and a child's transform is stored as given, parent-relative");
    }
    {
        // BYTE-STABLE SECOND WRITE, which catches parse-without-write and write-without-parse for
        // free -- the same fixture shape checkOcworld already uses for the flat records.
        OcWorldData w;
        check(parseOcworld(kNested, w, &err), "the nested world parses again");
        const std::string text = writeOcworld(w);
        check(text.find("BEGIN") != std::string::npos && text.find("CHILD ") != std::string::npos,
              "the writer emits the nesting rather than flattening it");
        OcWorldData b;
        check(parseOcworld(text, b, &err), "what the writer produced parses");
        check(b.placements.size() == 4, "with the same four placements");
        for (usize i = 0; i < 4; ++i)
            check(b.placements[i].parent == w.placements[i].parent,
                  "and the same parent for each -- the nesting IS the parent relation");
        check(writeOcworld(b) == text, "and a second write reproduces the first byte for byte");
    }
    {
        // THE PARSER'S NEW FAILURE MODE. Silently adopting the rest of a level as children of one
        // table leg is exactly the outcome a dangling BEGIN must not have.
        OcWorldData w;
        err.clear();
        check(!parseOcworld("OCWORLD 1\nNAME T\nPLACE a.ocmesh 0 0 0 0 0 0 1\nBEGIN\n"
                            "  CHILD b.ocmesh 0 0 1 0 0 0 1\n", w, &err),
              "a BEGIN that is never closed FAILS the parse");
        check(err.find("unbalanced") != std::string::npos, "and says so -- the message names the cause");

        err.clear();
        check(!parseOcworld("OCWORLD 1\nNAME T\nPLACE a.ocmesh 0 0 0 0 0 0 1\nEND\n", w, &err),
              "an END with no BEGIN FAILS too");
        err.clear();
        check(!parseOcworld("OCWORLD 1\nNAME T\nCHILD a.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "and so does a CHILD with nothing to parent it to");
        err.clear();
        check(!parseOcworld("OCWORLD 1\nNAME T\nBEGIN\n", w, &err),
              "and a BEGIN with no placement before it");
    }
    {
        // A FLAT LEVEL IS UNCHANGED, which is what every file in the tree is today: no BEGIN, no
        // CHILD, and a writer that emits neither.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME Flat\nPLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n"
                           "PLACE b.ocmesh 1 2 3 0 0 0 2 M_B\n", w, &err),
              "a flat world still parses");
        check(w.placements[0].parent == -1 && w.placements[1].parent == -1, "with every placement a root");
        const std::string text = writeOcworld(w);
        check(text.find("BEGIN") == std::string::npos && text.find("CHILD") == std::string::npos,
              "and writing it back adds no nesting -- a level cannot grow a hierarchy by being saved");
    }
    {
        // NON-UNIFORM SCALE ON A CHILD picks CHILDG, mirroring PLACE/PLACEG. Getting this wrong
        // would silently make a stretched child uniform on the next save.
        OcWorldData w;
        w.name = "Childg";
        OcWorldPlacement a; a.asset = "a.ocmesh";
        OcWorldPlacement b; b.asset = "b.ocmesh"; b.parent = 0; b.sx = 1; b.sy = 2; b.sz = 3;
        w.placements.push_back(a); w.placements.push_back(b);
        const std::string text = writeOcworld(w);
        check(text.find("CHILDG") != std::string::npos, "a non-uniformly scaled child writes as CHILDG");
        OcWorldData r;
        check(parseOcworld(text, r, &err), "and parses back");
        check(r.placements.size() == 2 && r.placements[1].parent == 0, "still parented");
        check(r.placements[1].sy == 2.0 && r.placements[1].sz == 3.0, "with its scale intact");
    }
    {
        // A CYCLE cannot come out of the parser, but writeOcworld also serves callers that built the
        // vector by hand. The walk must terminate and must not lose the rest of the level.
        OcWorldData w;
        w.name = "Cycle";
        OcWorldPlacement a; a.asset = "a.ocmesh"; a.parent = 1;
        OcWorldPlacement b; b.asset = "b.ocmesh"; b.parent = 0;
        OcWorldPlacement c; c.asset = "c.ocmesh";
        w.placements.push_back(a); w.placements.push_back(b); w.placements.push_back(c);
        const std::string text = writeOcworld(w);
        check(text.find("c.ocmesh") != std::string::npos,
              "a placement outside a hand-built cycle is still written -- the walk terminates");
        OcWorldData r;
        check(parseOcworld(text, r, &err), "and what came out still parses");
    }
}

// Checks PLACE's `name` token: the editor's outliner label, added so an F2/Details/Outliner rename
// survives a save (SandboxLevelEdit.cpp's saveLevel / SandboxLevelLoad.cpp's loadLevel). Modelled on
// `class` right down to the contract: absent means absent, a level written before this field existed
// round-trips byte-identically, and the value is a single percent-encoded token because the
// whitespace tokenizer (TextScan.hpp's splitWhitespace) has no quoting.
static void checkOcworldPlacementName() {
    AVER_INFO("=== .ocworld PLACE name token ===");
    using namespace fmt;
    std::string err;

    {
        // A PLAIN NAME needs no escaping at all, and must not disturb the material or `class` beside
        // it -- the three are parsed by the same token loop, keyword before the material catch-all.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "PLACE a.ocmesh 0 0 0 0 0 0 1 M_Wood name Doorway\n"
                           "PLACE b.ocmesh 1 0 0 0 0 0 1 class PlayerStart name Spawn\n",
                           w, &err), "PLACE lines with a plain name parse");
        check(w.placements.size() == 2, "and keep both placements");
        check(w.placements[0].name == "Doorway" && w.placements[0].material == "M_Wood",
              "placement 0: the name is read back verbatim, without eating the material");
        check(w.placements[1].name == "Spawn" && w.placements[1].className == "PlayerStart",
              "placement 1: name and class coexist, each keeping its own argument");

        const std::string text = writeOcworld(w);
        check(text.find("name Doorway") != std::string::npos &&
              text.find("name Spawn") != std::string::npos,
              "the written text carries both names");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.placements[0].name == "Doorway" && back.placements[1].name == "Spawn",
              "with both names intact");
        check(writeOcworld(back) == text, "and a second write reproduces the first byte for byte");
    }
    {
        // SPACES, A PERCENT SIGN AND A QUOTE -- exactly what percentEncode escapes, and exactly what
        // an F2 rename can type into the Details panel. Without escaping, the space alone would
        // split the name into extra tokens splitWhitespace hands back as if they were more fields on
        // the PLACE line.
        OcWorldData w;
        w.name = "NameEscaping";
        OcWorldPlacement p;
        p.asset = "b.ocmesh";
        p.name = "Player's 100% \"Secret\" Door";
        w.placements.push_back(p);
        const std::string text = writeOcworld(w);
        // ONE TOKEN: the raw name, spaces and all, must never appear literally in the file -- that
        // is exactly the shape of bug a missing escape would produce.
        check(text.find("Player's 100% \"Secret\" Door") == std::string::npos,
              "the raw name never appears unescaped in the written text");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "the escaped name still parses");
        check(back.placements.size() == 1 && back.placements[0].name == p.name,
              "and decodes back to exactly the name that was written -- spaces, '%' and '\"' intact");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // A FILE WITH NO NAMES IS UNCHANGED -- the exact guarantee task 1 exists for: a level saved
        // before this field existed must keep round-tripping byte-identically, not grow a `name`
        // token on every placement it never had one on.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME NoNames\n"
                           "PLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n"
                           "PLACE b.ocmesh 1 2 3 0 0 0 2 M_B nocollide\n",
                           w, &err), "a world with no PLACE name tokens parses");
        check(w.placements[0].name.empty() && w.placements[1].name.empty(),
              "and every placement's name is absent, not a derived default");

        const std::string text = writeOcworld(w);
        check(text.find("name ") == std::string::npos && text.find("name\n") == std::string::npos,
              "the written text carries no `name` token at all");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "it parses again");
        check(writeOcworld(back) == text,
              "and a second write is byte-identical -- exactly what an old, name-less save keeps doing");
    }
}

// Checks PLACE's `hidden` token: the Details panel's Visible checkbox, saved with the level
// (owner decision, Unreal-style) rather than the editor's own H/Shift+H/Ctrl+H hide, which stays
// session-only and never reaches this file. Modelled on `nocollide` right down to the contract:
// a bare token, absent means visible, and a level written before this field existed round-trips
// byte-identically.
static void checkOcworldPlacementHidden() {
    AVER_INFO("=== .ocworld PLACE hidden token ===");
    using namespace fmt;
    std::string err;

    {
        // A HIDDEN PLACEMENT, beside an ordinary one and beside `nocollide`, `class` and `name` --
        // all five parsed by the same trailing-token loop, keyword before the material catch-all.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "PLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n"
                           "PLACE b.ocmesh 1 0 0 0 0 0 1 M_B nocollide hidden snap class PlayerStart name Spawn\n",
                           w, &err), "PLACE lines with a hidden token parse");
        check(w.placements.size() == 2, "and keep both placements");
        check(w.placements[0].visible, "placement 0 (no `hidden` token) reads visible");
        check(!w.placements[1].visible, "placement 1 (`hidden`) reads NOT visible");
        check(!w.placements[1].collide && w.placements[1].snapToGround &&
              w.placements[1].className == "PlayerStart" && w.placements[1].name == "Spawn",
              "and every OTHER token on the same line still reads correctly beside it");

        const std::string text = writeOcworld(w);
        check(text.find("PLACE b.ocmesh 1 0 0 0 0 0 1 M_B nocollide hidden snap class PlayerStart "
                        "name Spawn") != std::string::npos,
              "the written text places `hidden` right after `nocollide`, before `snap`, matching "
              "the order it was read in");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.placements[0].visible && !back.placements[1].visible,
              "with both placements' visibility intact");
        check(writeOcworld(back) == text, "and a second write reproduces the first byte for byte");
    }
    {
        // A FILE WITH NOTHING HIDDEN IS UNCHANGED -- the exact guarantee `hidden` exists to keep:
        // a level saved before this field existed must keep round-tripping byte-identically, not
        // grow a `hidden` token on every placement it never had one on.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME NoneHidden\n"
                           "PLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n"
                           "PLACE b.ocmesh 1 2 3 0 0 0 2 M_B nocollide\n",
                           w, &err), "a world with no `hidden` tokens parses");
        check(w.placements[0].visible && w.placements[1].visible,
              "and every placement reads visible, the struct default");

        const std::string text = writeOcworld(w);
        check(text.find("hidden") == std::string::npos,
              "the written text carries no `hidden` token at all");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "it parses again");
        check(writeOcworld(back) == text,
              "and a second write is byte-identical -- exactly what an old, nothing-hidden save keeps doing");
    }
}

// Checks the SCATTER record: every field, round-trip byte-identity, an unbounded density band's
// deliberate omission from the written text, and that a line this parser does not understand (a
// stand-in for a future record) does not disturb SCATTER or anything else already parsed.
static void checkOcworldScatter() {
    AVER_INFO("=== .ocworld SCATTER record ===");
    using namespace fmt;
    std::string err;

    {
        // Two species, every field given explicitly, so nothing here is a struct default in disguise.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "SCATTER mesh Meshes/island_tree_03.ocmesh material M_Bark weight 0.6 scale 0.9 1.6 "
            "density 0.8 1.0 collide 170\n"
            "SCATTER mesh Meshes/grass_medium_01.ocmesh material M_Foliage weight 6 scale 0.8 1.4 "
            "collide 0 noyaw\n", w, &err),
            "a world with two SCATTER records parses");
        check(w.scatterSpecies.size() == 2, "and keeps both");

        const OcScatterSpecies& a = w.scatterSpecies[0];
        check(a.meshPath == "Meshes/island_tree_03.ocmesh", "species 0: mesh path");
        check(a.material == "M_Bark", "species 0: material");
        check(std::fabs(a.weight - 0.6) < 1e-12, "species 0: weight");
        check(std::fabs(a.scaleMin - 0.9) < 1e-12 && std::fabs(a.scaleMax - 1.6) < 1e-12,
              "species 0: scale range");
        check(std::fabs(a.densityMin - 0.8) < 1e-12 && std::fabs(a.densityMax - 1.0) < 1e-12,
              "species 0: density band");
        check(std::fabs(a.collisionRadiusCm - 170.0) < 1e-9, "species 0: collision radius");
        check(a.randomizeYaw, "species 0: yaw randomises by default (no `noyaw` token)");

        const OcScatterSpecies& b = w.scatterSpecies[1];
        check(b.meshPath == "Meshes/grass_medium_01.ocmesh", "species 1: mesh path");
        check(std::fabs(b.weight - 6.0) < 1e-12, "species 1: weight");
        check(b.densityMin <= -std::numeric_limits<f64>::max() / 2.0 &&
              b.densityMax >=  std::numeric_limits<f64>::max() / 2.0,
              "species 1: no `density` clause leaves the unbounded default");
        check(!b.randomizeYaw, "species 1: `noyaw` turns off yaw randomisation");

        // ---- round trip --------------------------------------------------------------------------
        const std::string text = writeOcworld(w);
        check(text.find("SCATTER mesh Meshes/island_tree_03.ocmesh") != std::string::npos,
              "the written text carries the first species");
        check(text.find("density 0.8 1") != std::string::npos,
              "...with its explicit density band");
        check(text.find("noyaw") != std::string::npos, "and the second species' `noyaw`");
        // The UNBOUNDED case is deliberately not printed as ~1.79769e+308 -- see writeOcworld's own
        // comment. Checked by absence: nothing in the grass line's output should carry a density
        // clause the input never gave it.
        const usize grassPos = text.find("Meshes/grass_medium_01.ocmesh");
        const usize grassLineEnd = text.find('\n', grassPos);
        check(grassPos != std::string::npos &&
              text.substr(grassPos, grassLineEnd - grassPos).find("density") == std::string::npos,
              "an unbounded species' line has no `density` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.scatterSpecies.size() == 2, "with both species still present");
        check(back.scatterSpecies[0].meshPath == a.meshPath &&
              std::fabs(back.scatterSpecies[0].weight - a.weight) < 1e-9 &&
              std::fabs(back.scatterSpecies[0].collisionRadiusCm - a.collisionRadiusCm) < 1e-6,
              "and the first species' fields round-trip");
        check(!back.scatterSpecies[1].randomizeYaw, "...including the second species' `noyaw`");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // A line no branch of the parser understands must not disturb a SCATTER record next to it --
        // the reader's own contract ("unknown records are skipped, not failed") exercised with SCATTER
        // specifically, since it is the newest record in the chain of if/else-if branches.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "FUTURERECORD something nobody has written yet\n"
            "SCATTER mesh Meshes/rock.ocmesh weight 1\n", w, &err),
            "a world with an unknown record before SCATTER still parses");
        check(w.scatterSpecies.size() == 1 && w.scatterSpecies[0].meshPath == "Meshes/rock.ocmesh",
              "and SCATTER is unaffected by the record it did not understand");
    }
    {
        // A malformed SCATTER line (missing the value a keyword expects) is simply the token that
        // never matches any branch -- the same tolerance PCGVOLUME already has for a short `bounds`.
        // What must NOT happen is the whole record disappearing or the parse failing outright.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nSCATTER mesh Meshes/x.ocmesh weight\n", w, &err),
              "a SCATTER line with a dangling keyword still parses the file");
        check(w.scatterSpecies.size() == 1 && std::fabs(w.scatterSpecies[0].weight - 1.0) < 1e-12,
              "and the species keeps weight's own default rather than reading garbage");
    }
    {
        // GAMEMODE: the level's own override of the project's default, by CLASS NAME -- what a World
        // Settings window edits. By name and not by handle because framework class handles come from
        // aver_fw_class_declare at runtime and are process-local; a number in a file would mean
        // something different next launch.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nGAMEMODE ForestGameMode\n", w, &err),
              "a GAMEMODE record parses");
        check(w.gameMode == "ForestGameMode", "and keeps the class name verbatim");

        // The REST OF THE LINE, so a class name with spaces survives -- nothing forbids one, and
        // silently truncating at the first space would bind the wrong class or none.
        OcWorldData sp;
        check(parseOcworld("OCWORLD 1\nGAMEMODE My Game Mode\n", sp, nullptr), "a spaced name parses");
        check(sp.gameMode == "My Game Mode", "...and is not truncated at the first space");

        OcWorldData none;
        check(parseOcworld("OCWORLD 1\nNAME T\n", none, nullptr), "a level with no GAMEMODE parses");
        check(none.gameMode.empty(), "...and states no override, rather than a class named nothing");

        const std::string text = writeOcworld(w);
        check(text.find("GAMEMODE ForestGameMode") != std::string::npos, "the writer emits it");
        check(writeOcworld(none).find("GAMEMODE") == std::string::npos,
              "a level with no override writes no GAMEMODE line at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.gameMode == w.gameMode, "and it round-trips");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // SEVERAL DENSITY FIELDS, and species bound to them by name. The tokens that make a level
        // able to have a foreground and a background: PCGVOLUME `radius` (how far this field
        // streams, in chunks) and SCATTER `volume` (which field places this species).
        //
        // One field forces one streaming radius for everything, and one radius cannot be right for
        // two things at once -- ground cover wants to be dense and near, a canopy wants to be sparse
        // and far. Both defaults are the unset sentinel, so a level naming neither behaves exactly
        // as every level did before these existed, which is the property checked last here.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "PCGVOLUME name Canopy seed 7 cell 3200 octaves 2 floor 0.7 bias 1 samples 3 radius 10 infinite\n"
            "PCGVOLUME name Floor seed 8 cell 1600 octaves 3 floor 0.3 bias 1 samples 12 infinite\n"
            "SCATTER mesh Meshes/pine.ocmesh material M_pine volume Canopy weight 2.5 scale 0.9 1.4 collide 120\n"
            "SCATTER mesh Meshes/fern.ocmesh material M_fern weight 16 scale 0.8 1.6 collide 0\n", w, &err),
            "a world with two PCGVOLUMEs and a volume-bound species parses");
        check(w.pcgVolumes.size() == 2 && w.scatterSpecies.size() == 2, "both volumes and both species survive");
        check(w.pcgVolumes[0].radiusChunks == 10, "the canopy volume keeps its streaming radius");
        check(w.pcgVolumes[1].radiusChunks == 0,
              "a volume with no `radius` clause keeps the unset-zero sentinel, not a defaulted number");
        check(w.scatterSpecies[0].volume == "Canopy", "the species names its volume verbatim");
        check(w.scatterSpecies[1].volume.empty(),
              "a species with no `volume` clause stays empty -- the first-non-Sky behaviour it always had");
        // `radius` must not disturb the six numbers `bounds` consumes, nor the tokens after `samples`.
        check(w.pcgVolumes[0].infinite && std::fabs(w.pcgVolumes[0].cellSizeCm - 3200.0) < 1e-9 &&
              w.pcgVolumes[0].samplesPerAxis == 3,
              "`radius` does not shift the tokens around it");

        const std::string text = writeOcworld(w);
        check(text.find("radius 10") != std::string::npos, "the written text carries the radius");
        check(text.find("volume Canopy") != std::string::npos, "...and the species' volume binding");
        // Both unset forms must be ABSENT, not written as `radius 0` / `volume `.
        const usize floorPos = text.find("name Floor");
        const usize floorEnd = text.find('\n', floorPos);
        check(floorPos != std::string::npos &&
              text.substr(floorPos, floorEnd - floorPos).find("radius") == std::string::npos,
              "a volume with no declared radius has no `radius` token at all");
        const usize fernPos = text.find("Meshes/fern.ocmesh");
        const usize fernEnd = text.find('\n', fernPos);
        check(fernPos != std::string::npos &&
              text.substr(fernPos, fernEnd - fernPos).find("volume") == std::string::npos,
              "a species with no declared volume has no `volume` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "the written text parses again");
        check(back.pcgVolumes.size() == 2 && back.pcgVolumes[0].radiusChunks == 10 &&
              back.pcgVolumes[1].radiusChunks == 0 &&
              back.scatterSpecies[0].volume == "Canopy" && back.scatterSpecies[1].volume.empty(),
              "radius and volume round-trip, present and absent alike");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
}

// Checks the LANDSCAPE record: every field, round-trip byte-identity, the unset-extent field's
// deliberate omission from the written text, and that a line this parser does not understand does not
// disturb LANDSCAPE or anything else already parsed -- the same three properties checkOcworldScatter
// already proves for SCATTER, exercised against the newest record in the chain of if/else-if branches.
// WATER / WAVE -- the records that let a level author its own surface instead of taking whatever the
// host's --water flag supplied. Two records rather than one, cross-referenced by name, because a
// level may hold both an ocean and a pool and "the water" stops meaning anything once there are two.
static void checkOcworldWater() {
    AVER_INFO("=== .ocworld WATER / WAVE records ===");
    using namespace fmt;
    std::string err;

    {
        // Both shapes of surface in one file, every field given explicitly so nothing below is a
        // struct default in disguise, plus a third WAVE that names no water at all.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "WATER name Pool level 45.5 bounds -300 -200 300 200\n"
            "WATER name Ocean level 0 infinite\n"
            "WAVE water Pool dir 1 0 wavelength 400 amplitude 8 steepness 0.4\n"
            "WAVE water Ocean dir 0.7 0.7 wavelength 1200 amplitude 60 steepness 0.75\n"
            "WAVE dir -1 0.2 wavelength 300 amplitude 5 steepness 0.3\n", w, &err),
            "a world with two WATER records and three WAVEs parses");
        check(w.waters.size() == 2, "and keeps both waters");
        check(w.waves.size() == 3, "and all three waves");

        const OcWaterPlacement& pool = w.waters[0];
        check(pool.name == "Pool", "water 0: name");
        check(std::fabs(pool.levelCm - 45.5) < 1e-12, "water 0: surface height");
        check(!pool.infinite, "water 0: a `bounds` clause makes it finite");
        check(std::fabs(pool.boundsMin[0] + 300.0) < 1e-12 && std::fabs(pool.boundsMin[1] + 200.0) < 1e-12 &&
              std::fabs(pool.boundsMax[0] - 300.0) < 1e-12 && std::fabs(pool.boundsMax[1] - 200.0) < 1e-12,
              "water 0: all four bounds, in order, not transposed");

        const OcWaterPlacement& ocean = w.waters[1];
        check(ocean.name == "Ocean", "water 1: name");
        check(ocean.infinite, "water 1: `infinite` is a bare token and sets the flag");

        const OcGerstnerWave& w0 = w.waves[0];
        check(w0.water == "Pool", "wave 0: names its water");
        check(std::fabs(w0.dirX - 1.0) < 1e-12 && std::fabs(w0.dirZ - 0.0) < 1e-12, "wave 0: direction");
        check(std::fabs(w0.wavelengthCm - 400.0) < 1e-12, "wave 0: wavelength");
        check(std::fabs(w0.amplitudeCm - 8.0) < 1e-12, "wave 0: amplitude");
        check(std::fabs(w0.steepness - 0.4) < 1e-12, "wave 0: steepness");

        // An unnamed wave is the common case -- one water in the level, and its author should not
        // have to name it just to give it a swell. Empty here means "the first declared WATER", a
        // resolution the FORMAT deliberately does not perform; it is the runtime consumer's job,
        // exactly as OcScatterSpecies::volume leaves its own empty case alone.
        check(w.waves[2].water.empty(), "wave 2: no `water` clause leaves the reference empty");

        // `simulate` is absent from both records above, and absence must mean analytic rather than
        // "whatever the last record said" -- the two are parsed by the same branch in sequence.
        check(!pool.simulate && !ocean.simulate, "neither record is simulated without the token");

        // ---- round trip ----------------------------------------------------------------------
        const std::string text = writeOcworld(w);
        check(text.find("WATER name Pool") != std::string::npos, "the written text carries the pool");
        check(text.find("bounds -300 -200 300 200") != std::string::npos, "...with its bounds");
        check(text.find("WATER name Ocean") != std::string::npos, "and the ocean");
        check(text.find("infinite") != std::string::npos, "...written as `infinite`, not as bounds");

        // Checked by ABSENCE, the same way the scatter test checks an unbounded density band: a wave
        // that never named a water must not come back from a save claiming to have named one.
        const usize thirdWave = text.rfind("WAVE");
        const usize thirdEnd = text.find('\n', thirdWave);
        check(thirdWave != std::string::npos &&
              text.substr(thirdWave, thirdEnd - thirdWave).find("water ") == std::string::npos,
              "an unnamed wave's line has no `water` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.waters.size() == 2 && back.waves.size() == 3, "with every record still present");
        check(back.waters[0].name == pool.name && !back.waters[0].infinite &&
              std::fabs(back.waters[0].levelCm - pool.levelCm) < 1e-9 &&
              std::fabs(back.waters[0].boundsMax[1] - pool.boundsMax[1]) < 1e-9,
              "and the pool's fields round-trip");
        check(back.waters[1].infinite, "...including the ocean staying infinite");
        check(std::fabs(back.waves[1].amplitudeCm - 60.0) < 1e-9 &&
              back.waves[1].water == "Ocean",
              "and a wave keeps both its numbers and its cross-reference");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // `simulate`, which turns a bounded surface into a soft body the solver sloshes. A bare
        // token like `infinite`, and it must survive a round trip past the four numbers `bounds`
        // consumes -- written after them for exactly that reason.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate\n", w, &err),
              "a simulated WATER record parses");
        check(w.waters.size() == 1 && w.waters[0].simulate, "and carries the simulate flag");
        check(!w.waters[0].infinite, "...alongside its bounds, which it still needs");

        const std::string text = writeOcworld(w);
        check(text.find("simulate") != std::string::npos, "the written text keeps the token");
        check(text.find("bounds 200 450 800 850 simulate") != std::string::npos,
              "...after the bounds numbers, not between them");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.waters.size() == 1 && back.waters[0].simulate,
              "and it survives the round trip");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // The four solver knobs (compliance/damping/iterations/pressure): real `key value` pairs, not
        // bare tokens like `simulate`, appended after it -- exactly the fluids solver-knobs brief's
        // own grammar example. Default -1 on every one means "not authored"; this record names all
        // four, so none of that sentinel should survive the parse.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate "
                           "compliance 0.00003 damping 0.6 iterations 8 pressure 900\n", w, &err),
              "a WATER record naming all four solver knobs parses");
        check(w.waters.size() == 1, "and it is kept");
        const OcWaterPlacement& knobbed = w.waters[0];
        check(std::fabs(knobbed.compliance - 0.00003) < 1e-9, "compliance carries the authored value");
        check(std::fabs(knobbed.damping - 0.6) < 1e-9, "...as does damping");
        check(knobbed.iterations == 8, "...and iterations, kept as a whole number");
        check(std::fabs(knobbed.pressure - 900.0) < 1e-9, "...and pressure");

        const std::string text = writeOcworld(w);
        check(text.find("compliance 3e-05 damping 0.6 iterations 8 pressure 900") != std::string::npos,
              "the written text keeps all four, after simulate, in parse order");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.waters.size() == 1, "and it survives the round trip");
        check(std::fabs(back.waters[0].compliance - knobbed.compliance) < 1e-9 &&
              std::fabs(back.waters[0].damping - knobbed.damping) < 1e-9 &&
              back.waters[0].iterations == knobbed.iterations &&
              std::fabs(back.waters[0].pressure - knobbed.pressure) < 1e-9,
              "...with all four numbers intact");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // THE MATERIAL LAYER (preset/density/viscosity), same grammar shape and round-trip discipline
        // as the four solver knobs' own block just above -- `preset` alone here, as its own record.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate preset honey\n",
                           w, &err),
              "a WATER record naming a material preset parses");
        check(w.waters.size() == 1 && w.waters[0].preset == "honey",
              "and carries the preset name verbatim");
        check(w.waters[0].density < 0.0 && w.waters[0].viscosity < 0.0,
              "with density/viscosity left at their -1 sentinel -- the preset carries no numbers of "
              "its own at the format level");

        const std::string text = writeOcworld(w);
        check(text.find("preset honey") != std::string::npos,
              "the written text keeps the preset, after the four solver knobs' own (absent) slot");
        check(text.find("density") == std::string::npos && text.find("viscosity") == std::string::npos,
              "and invents neither density nor viscosity for a record that named only a preset");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.waters.size() == 1 &&
              back.waters[0].preset == "honey", "and it survives the round trip");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // THE SURFACE MATERIAL, which is a THIRD thing this record calls "material" and the only one
        // that decides how the water LOOKS. `preset` is the SOLVER's material (density + viscosity);
        // `material` names an .ocmat. They are independent tokens and a record may carry both, which
        // is exactly what this case proves -- a level wanting honey that also LOOKS like honey has
        // to say both, because the preset has never carried an appearance.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate "
                           "preset honey material M_Honey\n", w, &err),
              "a WATER record naming BOTH a solver preset and a surface material parses");
        check(w.waters.size() == 1, "and is kept");
        check(w.waters[0].preset == "honey", "the solver preset is unchanged by the new token");
        check(w.waters[0].material == "M_Honey", "and the surface material is carried verbatim");

        const std::string text = writeOcworld(w);
        check(text.find("material M_Honey") != std::string::npos, "the written text keeps it");
        check(text.find("preset honey") != std::string::npos, "...alongside the preset, not instead");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.waters.size() == 1 &&
              back.waters[0].material == "M_Honey" && back.waters[0].preset == "honey",
              "and both survive the round trip");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // A record naming NO surface material must not GAIN one. Same byte-stability rule every
        // optional token here already follows: every .ocworld written before this token existed has
        // to round-trip unchanged, or adding the field turns every level in the tree into a diff.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate\n", w, &err),
              "a WATER record with no surface material parses");
        check(w.waters.size() == 1 && w.waters[0].material.empty(), "and carries an empty material");
        const std::string text = writeOcworld(w);
        check(text.find("material") == std::string::npos,
              "and the writer invents no material token for it");
    }
    {
        // Hand-typed density/viscosity, WITHOUT a preset -- the other half of the material grammar,
        // and the case that proves `preset` and the two numeric keys are independent tokens rather
        // than one clause that only parses together.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Tank level -20 bounds 0 0 300 200 simulate "
                           "density 1100 viscosity 0.05\n", w, &err),
              "a WATER record naming density and viscosity, with no preset, parses");
        check(w.waters.size() == 1, "and is kept");
        const OcWaterPlacement& tank = w.waters[0];
        check(tank.preset.empty(), "...with no preset name at all");
        check(std::fabs(tank.density - 1100.0) < 1e-9 && std::fabs(tank.viscosity - 0.05) < 1e-9,
              "...and both numbers carried exactly as authored");

        const std::string text = writeOcworld(w);
        check(text.find("density 1100 viscosity 0.05") != std::string::npos,
              "the written text keeps both, in parse order, and writes no `preset` token");
        check(text.find("preset") == std::string::npos, "...since none was authored");

        OcWorldData back;
        check(parseOcworld(text, back, &err) && back.waters.size() == 1 &&
              std::fabs(back.waters[0].density - tank.density) < 1e-9 &&
              std::fabs(back.waters[0].viscosity - tank.viscosity) < 1e-9,
              "and both numbers survive the round trip");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // THE CASE THAT MATTERS MOST FOR A LEVEL WRITTEN BEFORE THIS CHANGE: a plain simulated record
        // that names none of the four solver knobs -- NOR any of the three material-layer keys added
        // alongside them -- must still parse -- with every one of them left at its "not authored"
        // default, not at some other default that would make the format-level struct disagree with
        // what SandboxApp::applyLevelWater actually does with an unset field -- and the writer must
        // not invent any of the seven out of thin air on the way back out.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name Pool level -20 bounds 200 450 800 850 simulate\n", w, &err),
              "a simulated WATER record with no solver knobs still parses");
        check(w.waters.size() == 1, "and is kept");
        const OcWaterPlacement& plain = w.waters[0];
        check(plain.compliance < 0.0 && plain.damping < 0.0 && plain.iterations < 0 && plain.pressure < 0.0,
              "with all four solver knobs left at their -1 sentinel");
        check(plain.preset.empty() && plain.density < 0.0 && plain.viscosity < 0.0,
              "...and the material layer left equally unset: no preset name, density/viscosity at -1");
        const std::string text = writeOcworld(w);
        check(text.find("compliance") == std::string::npos && text.find("damping") == std::string::npos &&
              text.find("iterations") == std::string::npos && text.find("pressure") == std::string::npos,
              "and none of the four solver knobs written back out for a record that never named one");
        check(text.find("preset") == std::string::npos && text.find("density") == std::string::npos &&
              text.find("viscosity") == std::string::npos,
              "...nor any of the three material-layer keys -- a map written before this change loads "
              "and writes back byte-identically to how it always did");
    }
    {
        // THE CASE THAT MATTERS MOST FOR EVERY LEVEL THAT ALREADY EXISTS: a world with no WATER
        // record at all must parse exactly as it did before these records were added, and produce no
        // water rather than a default one.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "a world with no WATER record still parses");
        check(w.waters.empty() && w.waves.empty(), "and declares no water at all");
        check(writeOcworld(w).find("WATER") == std::string::npos,
              "...and writing it back emits no WATER line");
    }
    {
        // A WATER record with nothing but a level is the shortest useful form, and every omitted
        // clause must fall to its documented default rather than to whatever the previous record left
        // behind -- the two records here are parsed by the same branch, one after the other.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name A level 10 bounds -1 -2 3 4\n"
                           "WATER level 20\n", w, &err),
              "a minimal WATER record parses beside a fully-specified one");
        check(w.waters.size() == 2, "and both are kept");
        check(w.waters[1].name.empty(), "the minimal record's name is empty, not the previous one's");
        check(w.waters[1].infinite,
              "and it is INFINITE by default -- a bounded neighbour must not make it a pool");
    }
    {
        // The reader's standing contract, exercised against the newest records in the if/else-if
        // chain: an unknown record between two known ones must be skipped, not fail the parse.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "WATER name P level 5 infinite\n"
                           "FUTURERECORD nobody has written this yet\n"
                           "WAVE dir 1 0 wavelength 100 amplitude 1 steepness 0.1\n", w, &err),
              "an unknown record between WATER and WAVE still parses");
        check(w.waters.size() == 1 && w.waves.size() == 1,
              "and neither record is disturbed by the one the parser did not understand");
    }
}

static void checkOcworldLandscape() {
    AVER_INFO("=== .ocworld LANDSCAPE record ===");
    using namespace fmt;
    std::string err;

    {
        // Two sections, every field given explicitly on the first, so nothing here is a struct
        // default in disguise. The second omits `extent` and `name`, which is the common case: a
        // level with one terrain section rarely bothers naming it or restating its own footprint.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "LANDSCAPE name Valley section Terrain/valley_00.ocland at 1000 -500 120 extent 25600\n"
            "LANDSCAPE section Terrain/valley_01.ocland at 26600 -500 80\n", w, &err),
            "a world with two LANDSCAPE records parses");
        check(w.landscapes.size() == 2, "and keeps both");

        const OcLandscapePlacement& a = w.landscapes[0];
        check(a.name == "Valley", "section 0: name");
        check(a.section == "Terrain/valley_00.ocland", "section 0: asset reference");
        check(std::fabs(a.x - 1000.0) < 1e-9 && std::fabs(a.y + 500.0) < 1e-9 && std::fabs(a.z - 120.0) < 1e-9,
              "section 0: world placement");
        check(std::fabs(a.extentCm - 25600.0) < 1e-6, "section 0: declared extent");

        const OcLandscapePlacement& b = w.landscapes[1];
        check(b.name.empty(), "section 1: no `name` clause leaves the name empty");
        check(b.section == "Terrain/valley_01.ocland", "section 1: asset reference");
        check(std::fabs(b.x - 26600.0) < 1e-9, "section 1: world placement, x");
        check(b.extentCm == 0.0, "section 1: no `extent` clause leaves the unset-zero default");

        // ---- round trip --------------------------------------------------------------------------
        const std::string text = writeOcworld(w);
        check(text.find("LANDSCAPE name Valley section Terrain/valley_00.ocland") != std::string::npos,
              "the written text carries the first section");
        check(text.find("extent 25600") != std::string::npos, "...with its explicit extent");
        // UNSET extent is deliberately not printed as `extent 0` -- see writeOcworld's own comment.
        // Checked by absence: the second section's line must carry no `extent` token at all, and its
        // name must come back as the writer's own "unnamed" placeholder rather than an empty token a
        // reader could not parse a second time.
        check(text.find("LANDSCAPE name unnamed section Terrain/valley_01.ocland") != std::string::npos,
              "an unnamed section is written with the same 'unnamed' placeholder PCGVOLUME uses");
        const usize sec1Pos = text.find("Terrain/valley_01.ocland");
        const usize sec1LineEnd = text.find('\n', sec1Pos);
        check(sec1Pos != std::string::npos &&
              text.substr(sec1Pos, sec1LineEnd - sec1Pos).find("extent") == std::string::npos,
              "a section with no declared extent has no `extent` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again");
        check(back.landscapes.size() == 2, "with both sections still present");
        check(back.landscapes[0].name == a.name && back.landscapes[0].section == a.section &&
              std::fabs(back.landscapes[0].extentCm - a.extentCm) < 1e-6,
              "and the first section's fields round-trip");
        // NOT empty here -- "unnamed" is a WRITTEN placeholder, same as PCGVOLUME's own, and the
        // parser has no way to tell "the file says unnamed" from "the file says the word unnamed" on
        // a second read. That collision is already accepted for PCGVOLUME; b.name.empty() above is
        // the check that actually matters (an un-round-tripped level reads back empty, not "unnamed").
        check(back.landscapes[1].name == "unnamed",
              "...with the second section now carrying the literal placeholder the first write chose "
              "-- idempotent from here on, which the byte-for-byte check just below confirms");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // `material` on a LANDSCAPE line: the token that decides what terrain is SHADED with, and
        // the reason it exists is that there was no way to say it. LandscapeRenderer::setSurface
        // held a hardcoded olive and had no call site in the tree, so every level's terrain rendered
        // the same flat colour whatever it contained.
        //
        // Checked BOTH WAYS on purpose. Stating it must survive a round trip, and NOT stating it
        // must not invent one -- an empty material means "the renderer keeps its own default", so a
        // level that never mentioned terrain shading must not come back from a save claiming a
        // material it does not have. That is the same unset-is-not-a-value rule `extent` follows
        // just above, and the writer omits both for the same reason.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "LANDSCAPE name Floor section Terrain/floor.ocland material M_forest_leaves_02 at 0 0 0\n"
            "LANDSCAPE name Bare section Terrain/bare.ocland at 100 0 0\n", w, &err),
            "a LANDSCAPE record with a `material` clause parses");
        check(w.landscapes.size() == 2, "and both sections survive");
        check(w.landscapes[0].material == "M_forest_leaves_02", "the material name is kept verbatim");
        check(w.landscapes[1].material.empty(),
              "a section with no `material` clause leaves it empty, not defaulted to a name");
        // Placement still parses correctly with `material` sitting between `section` and `at` --
        // the token order is free, and a new key must not shift the ones after it.
        check(std::fabs(w.landscapes[1].x - 100.0) < 1e-9,
              "a later key still parses with `material` present on the sibling record");

        const std::string text = writeOcworld(w);
        check(text.find("material M_forest_leaves_02") != std::string::npos,
              "the written text carries the material");
        const usize barePos = text.find("Terrain/bare.ocland");
        const usize bareEnd = text.find('\n', barePos);
        check(barePos != std::string::npos &&
              text.substr(barePos, bareEnd - barePos).find("material") == std::string::npos,
              "a section with no material has no `material` token at all");

        OcWorldData back;
        check(parseOcworld(text, back, &err), "the written text parses again");
        check(back.landscapes.size() == 2 &&
              back.landscapes[0].material == "M_forest_leaves_02" &&
              back.landscapes[1].material.empty(),
              "and material round-trips, present and absent alike");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // Unknown-record tolerance, both directions: a line no branch understands must not disturb a
        // LANDSCAPE record next to it, whichever side it sits on.
        OcWorldData w;
        check(parseOcworld(
            "OCWORLD 1\nNAME T\n"
            "FUTURERECORD something nobody has written yet\n"
            "LANDSCAPE section Terrain/mesa.ocland at 0 0 0\n"
            "ANOTHERUNKNOWNRECORD 1 2 3\n", w, &err),
            "a world with unknown records around LANDSCAPE still parses");
        check(w.landscapes.size() == 1 && w.landscapes[0].section == "Terrain/mesa.ocland",
              "and LANDSCAPE is unaffected by records it did not understand, before or after it");
    }
    {
        // A malformed LANDSCAPE line (a dangling keyword with no value) is simply the token that never
        // matches any branch -- the same tolerance SCATTER and PCGVOLUME already have.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nLANDSCAPE section Terrain/x.ocland extent\n", w, &err),
              "a LANDSCAPE line with a dangling keyword still parses the file");
        check(w.landscapes.size() == 1 && w.landscapes[0].extentCm == 0.0,
              "and the section keeps extent's own unset default rather than reading garbage");
    }
    {
        // The record is optional and must stay so: every level written before it existed has none.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n", w, &err),
              "a world with no LANDSCAPE still parses");
        check(w.landscapes.empty(), "and reports none, rather than inventing a section from nothing");
    }
}

// THE BUG, REPRODUCED, using the exact function the editor's loadLevel called UNCONDITIONALLY for
// every level before this fix (parseOcworld/writeOcworld, both still here unmodified -- they are
// exactly right for a genuine .ocworld and stay the loader for one). Fed demoworld.ocmap's own
// content byte for byte: parseOcworld's header check accepts the OCMAP spelling without complaint
// (`equalsCI(key, "OCWORLD") || equalsCI(key, "OCMAP")`, OcWorld.cpp), so nothing here reports
// failure -- it just quietly keeps only what OcWorldData has room for, which is the placements
// grammar the two formats share (PLACE) and none of ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM. This
// is the "before": the same input checkOcmapRoundtrip below feeds to parseOcmap/writeOcmap and
// gets back whole.
static void checkOldDispatchDroppedLegacyRecords() {
    AVER_INFO("=== the bug: loadOcworld on a legacy .ocmap silently drops six record kinds ===");
    using namespace fmt;
    std::string err;

    OcWorldData w;
    check(parseOcworld(
        "OCMAP 1\n"
        "ID 0x376B85BC4D1A03BA\n"
        "NAME demoworld\n"
        "BUILD 1\n"
        "ALGO 3\n"
        "ROOT 024529dbb250619b7e8331975f0f06b6029fff86144834dce2ed63171758a0db\n"
        "CLIENT umap /Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack\n"
        "SURFACE 0 tarmac 1.00 0.015 0.30\n"
        "SURFACE 1 kerb   0.92 0.020 0.40\n"
        "SURFACE 2 grass  0.45 0.090 0.35\n"
        "GROUND 0.0 0\n"
        "KILLZ -5000.0\n"
        "DEFORM tyre_barrier.ocbeam 12000.0 800.0 0.0 90.0 0.0 0.0 rubber\n"
        "PLACE kerb_4m 1200.0 400.0 0.0 0.0 0.0 0.0 1.000\n", w, &err),
        "parseOcworld 'succeeds' on an OCMAP-headed file -- ok=true, exactly as the blocker reports");
    check(w.placements.size() == 1,
          "and the DEFORM record is simply gone -- 2 placements in the file, 1 survives (PLACE only; "
          "parseOcworld has no DEFORM branch at all)");

    const std::string lost = writeOcworld(w);
    check(lost.find("ROOT") == std::string::npos && lost.find("CLIENT") == std::string::npos &&
              lost.find("SURFACE") == std::string::npos && lost.find("GROUND") == std::string::npos &&
              lost.find("KILLZ") == std::string::npos && lost.find("DEFORM") == std::string::npos,
          "and a save through this path writes back a file with NONE of ROOT/CLIENT/SURFACE/GROUND/"
          "KILLZ/DEFORM -- OcWorldData has no field for any of them, so there is nothing left to write");
}

// Checks fmt::writeOcmap/saveOcmap -- added alongside the editor's legacy-load dispatch
// (sandbox/src/SandboxApp.cpp's loadLevel/saveLevel), which is what closed the actual blocker: the
// PREVIOUS check reproduced it (fed the SAME demoworld.ocmap content to parseOcworld/writeOcworld,
// the unconditional pre-fix path, and lost six record kinds and a placement). This checks the fix:
// every one of those six round-trips through parseOcmap -> writeOcmap -> parseOcmap, and a second
// write reproduces the first byte for byte, the same guarantee every other writer in this file
// carries. The record set below is demoworld.ocmap's own, values and all (see
// docs/formats/FORMAT_SPECS.md §4.3's own reference to it).
static void checkOcmapRoundtrip() {
    AVER_INFO("=== .ocmap writer / round trip ===");
    using namespace fmt;
    std::string err;

    OcMapData m;
    check(parseOcmap(
        "OCMAP 1\n"
        "ID 0x376B85BC4D1A03BA\n"
        "NAME demoworld\n"
        "BUILD 1\n"
        "ALGO 3\n"
        "ROOT 024529dbb250619b7e8331975f0f06b6029fff86144834dce2ed63171758a0db\n"
        "CLIENT umap /Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack\n"
        "SURFACE 0 tarmac 1.00 0.015 0.30\n"
        "SURFACE 1 kerb   0.92 0.020 0.40\n"
        "SURFACE 2 grass  0.45 0.090 0.35\n"
        "GROUND 0.0 0\n"
        "KILLZ -5000.0\n"
        "DEFORM tyre_barrier.ocbeam 12000.0 800.0 0.0 90.0 0.0 0.0 rubber\n"
        "PLACE kerb_4m 1200.0 400.0 0.0 0.0 0.0 0.0 1.000\n", m, &err),
        "demoworld's own record set parses");
    check(m.surfaces.size() == 3, "all three SURFACE rows kept");
    check(m.hasGround && m.groundZ == 0.0 && m.groundSurface == 0, "GROUND kept");
    check(m.killZ == -5000.0, "KILLZ kept");
    check(m.clientUmap ==
              "/Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack",
          "CLIENT umap path kept");
    check(m.placements.size() == 2 && m.placements[0].deform && !m.placements[1].deform,
          "DEFORM and PLACE both kept, in file order");
    check(m.placements[0].material == "rubber", "DEFORM's material kept");
    check(m.placements[1].surface == -1, "PLACE's unauthored surface reads as -1, not 0");

    const std::string text = writeOcmap(m);
    check(text.find("ROOT 024529dbb250619b7e8331975f0f06b6029fff86144834dce2ed63171758a0db") !=
              std::string::npos,
          "the written text carries ROOT verbatim");
    check(text.find("CLIENT umap /Game/FIA_WEC") != std::string::npos, "and CLIENT");
    check(text.find("SURFACE 2 grass") != std::string::npos, "and the SURFACE table");
    check(text.find("GROUND") != std::string::npos && text.find("KILLZ") != std::string::npos,
          "and GROUND and KILLZ");
    check(text.find("DEFORM") != std::string::npos, "and DEFORM");

    OcMapData back;
    check(parseOcmap(text, back, &err), "what the writer produced parses again");
    check(back.root == m.root, "ROOT survives the round trip byte for byte");
    check(back.clientUmap == m.clientUmap, "and CLIENT");
    check(back.surfaces.size() == 3 && back.surfaces[2].name == "grass" &&
              std::fabs(back.surfaces[2].grip - 0.45) < 1e-9,
          "and every SURFACE row");
    check(back.hasGround && back.groundZ == m.groundZ && back.groundSurface == m.groundSurface,
          "and GROUND");
    check(back.killZ == m.killZ, "and KILLZ");
    check(back.placements.size() == 2 && back.placements[0].deform &&
              back.placements[0].material == "rubber" && !back.placements[1].deform,
          "and both placements, DEFORM's material included");
    check(writeOcmap(back) == text, "and a second write reproduces the first byte for byte");
}

// Checks fmt::levelFileIsLegacyOcmap -- the content scan the editor's loadLevel dispatches on. The
// extension is deliberately the SAME (.ocmap) in every case here, because the extension is exactly
// the signal that function's own comment says cannot be trusted; the header line's spelling is
// ALSO deliberately unhelpful in two of these cases, for the same reason -- see the "modern.ocmap"
// and "electricDreamsShaped" cases below, which reproduce the two real files that ruled the header
// out as a signal at all.
static void checkLevelFileIsLegacyOcmap() {
    AVER_INFO("=== fmt::levelFileIsLegacyOcmap ===");
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "AverFormatTest_levelHeaderSniff";
    std::filesystem::create_directories(dir);

    const std::filesystem::path legacy = dir / "legacy.ocmap";
    {
        std::ofstream f(legacy, std::ios::binary | std::ios::trunc);
        f << "OCMAP 1\nNAME T\nGROUND 0 0\nKILLZ -5000\n";
    }
    check(fmt::levelFileIsLegacyOcmap(legacy.string()),
          "a file using GROUND/KILLZ reads as legacy");

    const std::filesystem::path modern = dir / "modern.ocmap";
    {
        std::ofstream f(modern, std::ios::binary | std::ios::trunc);
        f << "OCWORLD 1\nNAME T\nPLACE m.ocmesh 0 0 0 0 0 0 1\n";
    }
    check(!fmt::levelFileIsLegacyOcmap(modern.string()),
          "a file using none of the six reads as NOT legacy, OCWORLD header and all");

    // THE CASE THAT MATTERS MOST: AverProjects/ElectricDreams/Content/Maps/Default.ocmap and
    // AverProjects/FirstPerson/Content/Maps/Default.ocmap are BOTH real, both currently load and
    // save correctly, and BOTH start with the literal line `OCMAP 1` while using nothing but
    // OCWORLD-only records underneath (SUN/SKY/FOG plus LANDSCAPE/PCGVOLUME/SCATTER for one,
    // SUN/SKY/FOG/PLACEG for the other). A header-only version of this function read both as
    // legacy and would have routed them into fmt::loadOcmap, which has no branch for any of SUN/
    // SKY/FOG/LANDSCAPE/PCGVOLUME/SCATTER/PLACEG -- silently destroying the sky, the fog, the
    // terrain and (for FirstPerson) the ground slab itself on the very first save. This reproduces
    // that shape in miniature and pins the fix: an OCMAP-headed file using only OCWORLD records
    // must NOT read as legacy.
    const std::filesystem::path electricDreamsShaped = dir / "electric_dreams_shaped.ocmap";
    {
        std::ofstream f(electricDreamsShaped, std::ios::binary | std::ios::trunc);
        f << "OCMAP 1\nNAME T\nSUN elev 30 azim 90\nSKY model physical\n"
             "FOG exp density 0.0002\nPLACEG m.ocmesh 0 0 0 0 0 0 1 1 1\n";
    }
    check(!fmt::levelFileIsLegacyOcmap(electricDreamsShaped.string()),
          "an OCMAP-headed file using only SUN/SKY/FOG/PLACEG reads as NOT legacy -- the header "
          "token is not the same question as which records the file actually uses");

    // A comment line and a blank line ahead of the real records must not fool the scan -- the same
    // tolerance parseOcworld/parseOcmap themselves have for a file that opens with either.
    const std::filesystem::path commented = dir / "commented.ocmap";
    {
        std::ofstream f(commented, std::ios::binary | std::ios::trunc);
        f << "# exported by an old tool\n\nOCMAP 1\nNAME T\nGROUND 0 0\n";
    }
    check(fmt::levelFileIsLegacyOcmap(commented.string()),
          "a leading comment/blank line does not hide a legacy record further down the file");

    check(!fmt::levelFileIsLegacyOcmap((dir / "does_not_exist.ocmap").string()),
          "a file that cannot be read is NOT reported as legacy -- the real loader is what reports that failure");

    std::filesystem::remove_all(dir);
}

// Checks PLACE's object-animation tokens: `anim <clip>` (percent-encoded, content-relative, with the
// extension), `animspeed <f>`, `animtime <f>` and the bare `animonce`. Written only when a clip is
// named, each companion only when it differs from its default, so an un-animated level is unchanged.
static void checkOcworldPlacementAnim() {
    AVER_INFO("=== .ocworld PLACE anim tokens ===");
    using namespace fmt;
    std::string err;
    const auto count = [](const std::string& hay, const std::string& needle) {
        int n = 0;
        for (usize at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
        return n;
    };

    {
        // ALL FOUR TOKENS beside a material, and a clip alone beside a name and an ordinary placement.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME T\n"
                           "PLACE car.ocmesh 0 0 0 0 0 0 1 M_Paint anim Anims/Route_Car.ocanim animspeed 2.5 animtime 1.25 animonce\n"
                           "PLACE fan.ocmesh 5 0 0 0 0 0 1 anim Anims/fan.ocanim name Fan\n"
                           "PLACE rock.ocmesh 9 0 0 0 0 0 1 M_Rock\n",
                           w, &err), "PLACE lines with anim tokens parse: " + err);
        check(w.placements.size() == 3, "and keep all three placements");
        if (w.placements.size() == 3) {
            const OcWorldPlacement& car = w.placements[0];
            check(car.animClip == "Anims/Route_Car.ocanim" && car.animSpeed == 2.5f && car.animTime == 1.25f &&
                  car.animOnce, "placement 0: clip, speed, time and once are all read");
            check(car.material == "M_Paint", "without eating the material beside them");
            const OcWorldPlacement& fan = w.placements[1];
            check(fan.animClip == "Anims/fan.ocanim" && fan.animSpeed == 1.0f && fan.animTime == 0.0f && !fan.animOnce,
                  "placement 1: a bare clip keeps speed 1, time 0 and loops");
            check(fan.name == "Fan", "and its name token still reads");
            const OcWorldPlacement& rock = w.placements[2];
            check(rock.animClip.empty() && rock.animSpeed == 1.0f && rock.animTime == 0.0f && !rock.animOnce,
                  "placement 2: no tokens means no animation");
            check(rock.material == "M_Rock", "and the material is still the material");
        }

        const std::string text = writeOcworld(w);
        check(text.find("anim Anims/Route_Car.ocanim animspeed 2.5 animtime 1.25 animonce") != std::string::npos,
              "the written text carries the four tokens together, in that order");
        check(count(text, "animspeed") == 1 && count(text, "animtime") == 1 && count(text, "animonce") == 1,
              "and only the placement that set a companion writes it");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "what the writer produced parses again: " + err);
        check(back.placements.size() == 3 && back.placements[0].animClip == "Anims/Route_Car.ocanim" &&
              back.placements[0].animSpeed == 2.5f && back.placements[0].animTime == 1.25f &&
              back.placements[0].animOnce && back.placements[1].animClip == "Anims/fan.ocanim" &&
              back.placements[2].animClip.empty(), "with every animation field intact");
        check(writeOcworld(back) == text, "and a second write reproduces the first byte for byte");
    }
    {
        // A CLIP PATH WITH A SPACE goes out percent-encoded, like a name, or it would split into tokens.
        OcWorldData w;
        w.name = "AnimEscaping";
        OcWorldPlacement p;
        p.asset = "boat.ocmesh";
        p.animClip = "Anims/My Route.ocanim";
        p.animSpeed = 0.5f;
        w.placements.push_back(p);
        const std::string text = writeOcworld(w);
        check(text.find("My Route") == std::string::npos, "the raw path never appears with its space");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "the escaped path still parses: " + err);
        check(back.placements.size() == 1 && back.placements[0].animClip == p.animClip &&
              back.placements[0].animSpeed == 0.5f, "and decodes back to exactly the path that was written");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // '#' AND ';' IN A NAME OR A CLIP PATH: parseOcworld cuts a line at the first '#' and drops one
        // trailing ';' before it splits tokens, so a raw '#' lost every token after it (the clip, its
        // speed, `animonce`) and a raw trailing ';' lost a character of the last one. Both go out escaped.
        OcWorldData w;
        w.name = "HashEscaping";
        OcWorldPlacement p;
        p.asset = "boat.ocmesh";
        p.name = "Boat #2; fast";
        p.animClip = "Anims/Route #1;.ocanim";
        p.animSpeed = 0.5f;
        p.animOnce = true;
        w.placements.push_back(p);
        OcWorldPlacement q;                 // the name is the LAST token here, so a raw ';' would be stripped
        q.asset = "buoy.ocmesh";
        q.name = "Buoy;";
        w.placements.push_back(q);
        const std::string text = writeOcworld(w);
        check(text.find("Route #1") == std::string::npos && text.find("Boat #2") == std::string::npos,
              "no raw '#' from a name or clip path reaches the written text");
        check(text.find("%23") != std::string::npos, "the '#' is written as %23");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "the escaped text parses: " + err);
        check(back.placements.size() == 2 && back.placements[0].name == p.name &&
              back.placements[0].animClip == p.animClip && back.placements[0].animSpeed == 0.5f &&
              back.placements[0].animOnce, "name, clip path and the tokens after them all come back intact");
        check(back.placements.size() == 2 && back.placements[1].name == q.name,
              "a name ending in ';' keeps it");
        check(writeOcworld(back) == text, "a second write reproduces the first byte for byte");
    }
    {
        // A FILE WITH NO ANIMATION IS UNCHANGED -- no token appears, and an old save keeps round-tripping.
        OcWorldData w;
        check(parseOcworld("OCWORLD 1\nNAME NoAnim\n"
                           "PLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n"
                           "PLACE b.ocmesh 1 2 3 0 0 0 2 M_B nocollide\n",
                           w, &err), "a world with no anim tokens parses: " + err);
        const std::string text = writeOcworld(w);
        check(text.find(" anim") == std::string::npos, "the written text carries no anim token at all");
        OcWorldData back;
        check(parseOcworld(text, back, &err), "it parses again: " + err);
        check(writeOcworld(back) == text, "and a second write is byte-identical");
    }
    {
        // COMPANIONS WITHOUT A CLIP WRITE NOTHING: with no clip there is nothing to time or hold.
        OcWorldData w;
        w.name = "Orphans";
        OcWorldPlacement p;
        p.asset = "a.ocmesh";
        p.animSpeed = 3.0f;
        p.animTime = 2.0f;
        p.animOnce = true;
        w.placements.push_back(p);
        check(writeOcworld(w).find(" anim") == std::string::npos,
              "speed, time and once are not written for a placement with no clip");
    }
}

// Checks PLACE's `vehicle <preset>` token: the physics preset world::VehicleSystem builds a placement
// into. Written only when set, so a level with no vehicles is byte-for-byte what it was.
static void checkOcworldPlacementVehicle() {
    AVER_INFO("=== .ocworld PLACE vehicle token ===");
    using namespace fmt;
    std::string err;

    OcWorldData w;
    check(parseOcworld("OCWORLD 1\nNAME T\n"
                       "PLACE car.ocmesh 0 0 0 0 0 0 1 M_Paint vehicle sports name Taxi\n"
                       "PLACE bus.ocmesh 5 0 0 0 0 0 1 vehicle bus\n"
                       "PLACE rock.ocmesh 9 0 0 0 0 0 1 M_Rock\n",
                       w, &err), "PLACE lines with a vehicle token parse: " + err);
    check(w.placements.size() == 3, "and keep all three placements");
    if (w.placements.size() == 3) {
        check(w.placements[0].vehiclePreset == "sports" && w.placements[1].vehiclePreset == "bus" &&
              w.placements[2].vehiclePreset.empty(), "the preset is read, and absent means none");
        check(w.placements[0].material == "M_Paint" && w.placements[0].name == "Taxi",
              "without eating the material or the name beside it");
        check(w.placements[0].animClip.empty(), "and a vehicle is not an animation");
    }

    const std::string text = writeOcworld(w);
    check(text.find(" vehicle sports") != std::string::npos && text.find(" vehicle bus") != std::string::npos,
          "the written text carries both presets");
    OcWorldData back;
    check(parseOcworld(text, back, &err), "what the writer produced parses again: " + err);
    check(back.placements.size() == 3 && back.placements[0].vehiclePreset == "sports" &&
          back.placements[1].vehiclePreset == "bus" && back.placements[2].vehiclePreset.empty(),
          "with every preset intact");
    check(writeOcworld(back) == text, "and a second write reproduces the first byte for byte");

    OcWorldData none;
    check(parseOcworld("OCWORLD 1\nNAME NoCars\nPLACE a.ocmesh 0 0 0 0 0 0 1 M_A\n", none, &err),
          "a world with no vehicle token parses: " + err);
    check(writeOcworld(none).find(" vehicle") == std::string::npos, "and writes no vehicle token at all");
}

// Runs the self-checks, then every file named on the command line. Returns the failure count.
int main(int argc, char** argv) {
    checkFnv();
    checkOcproject();
    checkOcworld();
    checkOcworldNesting();
    checkOcworldPlacementName();
    checkOcworldPlacementHidden();
    checkOcworldPlacementAnim();
    checkOcworldPlacementVehicle();
    checkOcworldScatter();
    checkOcworldLandscape();
    checkOcworldWater();
    checkOldDispatchDroppedLegacyRecords();
    checkOcmapRoundtrip();
    checkLevelFileIsLegacyOcmap();
    if (argc < 2) {
        AVER_INFO("usage: FormatTest <file.ocbeam|file.ocmap> [more...]");
        return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        switch (assetTypeFromPath(path)) {
            case AssetType::Beam: testBeam(path); break;
            case AssetType::Map:  testMap(path); break;
            default: AVER_WARN("skipping (unknown type): {}", path); break;
        }
    }
    AVER_INFO("==================================================");
    AVER_INFO("Format tests done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
