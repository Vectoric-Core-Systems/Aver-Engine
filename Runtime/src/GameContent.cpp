#include "aver/game/GameContent.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

// UNCONDITIONAL, deliberately. AssetType/assetTypeFromPath live in modules/assets -- a leaf with no
// module switch at all, always linked through Aver.Formats -- and they have TWO callers here under
// DIFFERENT guards: loadProjectMeshes(), which is scene-guarded, and loadProjectParticleEffects(),
// which is particles-guarded. Scoping the include to either guard leaves the other branch without
// the type. (A PBR-guarded loadProjectMaterials() was a third caller until it was removed as dead
// code; that removal changes nothing here, because the two remaining guards still differ.)
#include "aver/assets/AssetId.hpp"

#if AVER_MODULE_PBR
#  include "aver/assets/TextureUpload.hpp"
#  include "aver/formats/OcGraph.hpp"
#  include "aver/formats/OcMat.hpp"
#  include "aver/pbr/MaterialGraphRegistry.hpp"
#endif

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/formats/OcMesh.hpp"
#  include "aver/scene/scene_abi.h"
#  include "GameMath.hpp"
#  if AVER_MODULE_TRIFACTOR
#    include "aver/trifactor/ClusterAdapt.hpp"
#  endif
#endif

#if AVER_MODULE_PARTICLES
#  include "aver/formats/OcParticle.hpp"
#  include "aver/particles/ParticleEffectLibrary.hpp"
#endif

namespace aver::game {

void GameContent::adopt(const fmt::ProjectDesc& project) {
    project_ = project;
    contentIndex_.clear();

    const std::string content = project_.contentDir();
    if (content.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) {
        AVER_WARN("[Content] the project's content root does not exist: {}", content);
        return;
    }

    // The error_code overload of increment, so an unreadable subdirectory ends the walk instead of
    // throwing out of it.
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // FROZEN: the id hashes the forward-slash spelling, matching C# Assets.ObjectIdOf. This is
        // the property that makes packaging nearly free -- stage-game.ps1 copies Content/ under a
        // new root and every id in every .ocworld and .ocmat is unchanged, because the ids were
        // never a function of where the project lives.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);

#if AVER_MODULE_SCENE
    // The anim system does its own file discovery through this and caches by id, so a re-index has
    // to drop what it cached or a moved asset keeps resolving to its old path. Safe because adopt()
    // is a project-adoption call and never a per-frame one.
    //
    // Guarded on SCENE rather than PBR, which is where SandboxApp has it: the animation system has
    // nothing to do with physically based rendering, and the original guard is the cross-
    // contamination this lift exists to stop copying forward.
    anim::animSystem().clear();
    anim::animSystem().setResolver(&GameContent::resolveAnimAsset, this);
#endif
}

std::string GameContent::pathFor(u64 id) const {
    const auto it = contentIndex_.find(id);
    return it == contentIndex_.end() ? std::string() : it->second;
}

std::vector<std::string> GameContent::pathsWithExtension(std::string_view ext) const {
    std::vector<std::string> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        // Case-insensitive suffix compare, ASCII only -- matches isOcproject's own reasoning in
        // GameApp.cpp (a project's asset extensions are all plain ASCII, and Windows paths are
        // case-insensitive on disk but not in a plain string compare).
        bool match = true;
        for (usize i = 0; i < ext.size(); ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match) out.push_back(path);
    }
    // contentIndex_ is an unordered_map: iteration order is not the walk order, and is not even
    // stable between two runs of the SAME binary over the SAME content. A caller that assigns
    // anything by position (GameApp's synthetic entity ids, notably) would otherwise get a
    // reproducibility gap that looks like a bug in whatever the ids are used for.
    std::sort(out.begin(), out.end());
    return out;
}

std::string GameContent::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->pathFor(id) : std::string();
}

#if AVER_MODULE_SCENE

namespace {

// Appends an axis-aligned box of INDEPENDENT per-axis half-extents (hx,hy,hz), yawed by yawDeg around
// Z and placed at (cx,cy,cz). GameMath.hpp's appendBox is fixed to a symmetric cube (one h for all
// three axes, no rotation) because that is all a placed-in-a-level box ever needed; the drone's arms
// are long and thin and pointed at the four diagonals, and its body/skids/struts are axis-aligned
// boxes of yet other aspect ratios, so one generalised generator replaces four bespoke ones. The face
// table below is copied verbatim from appendBox -- same corners, same winding, same per-face normals
// -- with the per-axis extents and the yaw rotation as the only additions. yawDeg=0 makes this an
// axis-aligned anisotropic box; hx=hy=hz with yawDeg=0 reproduces appendBox exactly (true by
// construction, same face table and corner order -- not separately asserted here).
//
// A SECOND COPY of sandbox/src/SandboxApp.cpp's own appendBoxYaw (read, not shared -- that file
// belongs to another agent), the same trade GameMath.hpp already makes for appendBox/appendSphere:
// one generator, needed by both the editor and the runtime, copied instead of promoted to a shared
// header because that header is outside this change's file ownership.
void appendBoxYaw(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                   f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, f32 yawDeg) {
    const f32 rad = yawDeg * kDegToRad;
    const f32 cs = std::cos(rad), sn = std::sin(rad);
    // Rotates a LOCAL (lx,ly,lz) around Z. A pure rotation has determinant +1, so it changes nothing
    // about winding or handedness -- every face below stays CCW-outward exactly as appendBox left it.
    auto rotZ = [cs, sn](f32 lx, f32 ly, f32 lz, f32& ox, f32& oy, f32& oz) {
        ox = lx * cs - ly * sn; oy = lx * sn + ly * cs; oz = lz;
    };
    const f32 p[8][3] = {{-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
                         {-hx,-hy,hz},{hx,-hy,hz},{hx,hy,hz},{-hx,hy,hz}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        f32 nx = 0.0f, ny = 0.0f, nz = 0.0f; rotZ(f.n[0], f.n[1], f.n[2], nx, ny, nz);
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) {
            const f32* c = p[f.c[k]];
            f32 wx = 0.0f, wy = 0.0f, wz = 0.0f; rotZ(c[0], c[1], c[2], wx, wy, wz);
            v.push_back({cx+wx, cy+wy, cz+wz, nx, ny, nz, quadUV[k][0], quadUV[k][1]});
        }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}

// Appends a capped cylinder standing along +Z, centred at (cx,cy,cz) -- the drone's motor pods and
// rotor discs. Flat-shaded per face like appendBox/appendBoxYaw, not smooth-shaded like appendSphere:
// at the segment counts a rotor pod uses (8-10) a smoothed normal would look indistinguishable from a
// faceted one, so this reuses the "duplicate vertices, exact face normal" convention every other
// hand-built primitive already follows rather than adding a second shading convention. A second copy
// of SandboxApp.cpp's own appendCylinderZ, for the same file-ownership reason as appendBoxYaw above.
void appendCylinderZ(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                      f32 cx, f32 cy, f32 cz, f32 radius, f32 halfHeight, u32 segments) {
    for (u32 s = 0; s < segments; ++s) {
        const f32 a0 = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 x0 = std::cos(a0), y0 = std::sin(a0);
        const f32 x1 = std::cos(a1), y1 = std::sin(a1);
        // Side quad: both edges get the SAME flat normal -- the averaged (renormalised) radial
        // direction of the two -- the same "one normal per face" rule appendBox uses, just computed
        // rather than hand-written because the direction depends on which segment this is.
        f32 nx = x0 + x1, ny = y0 + y1;
        const f32 nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; }
        const u32 b = static_cast<u32>(v.size());
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, nx, ny, 0, 0, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, nx, ny, 0, 1, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, nx, ny, 0, 1, 1});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, nx, ny, 0, 0, 1});
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
        // Top (+Z) and bottom (-Z) caps, each a single fan triangle for this segment's wedge -- cheap
        // at these segment counts (an 8-10 sided cap still reads as round) and it keeps the caps flat-
        // shaded too, instead of introducing yet another normal convention for just two faces.
        const u32 ct = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz + halfHeight, 0, 0, 1, 0.5f, 0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, 0, 0, 1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, 0, 0, 1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        idx.push_back(ct); idx.push_back(ct+1); idx.push_back(ct+2);
        const u32 cb = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz - halfHeight, 0, 0, -1, 0.5f, 0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, 0, 0, -1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, 0, 0, -1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        idx.push_back(cb); idx.push_back(cb+1); idx.push_back(cb+2);
    }
}

// Appends a placeholder quadcopter, built from appendBoxYaw/appendCylinderZ, for the "the drone [has]
// a box, the drone should be like UE's default drone" complaint against the old drone spawn (a bare
// unit cube -- see SandboxApp.cpp's setDroneEnabled). 420 triangles: a central body, four arms out to
// the corners each ending in a motor-pod hub and a rotor disc, and a pair of landing skids on struts.
//
// NORMALISED THE SAME WAY THE UNIT CUBE AND UNIT SPHERE ARE: nothing in this mesh goes past 1.0 from
// the origin, so a PLACEG scale on "Meshes/drone.ocmesh" means the same half-extent-in-centimetres
// thing it means on the cube. UNLIKE the isotropic cube and sphere, though, this shape is NOT the same
// size along every axis -- it is a flat quadcopter, not a cube -- so "1.0" is reached only at the four
// rotor-tip diagonals (kArmROuter + kDiscRadius = 0.80 + 0.20 = 1.00 exactly); the straight per-axis
// reach is smaller (about 0.7657 along X or Y alone, since a disc centred on a 45-degree line does not
// project its full radius onto either axis), and the vertical reach is smaller again (about 0.16 up,
// 0.22 down). Recorded precisely at the registration site (search "droneId" in registerBuiltins,
// below) rather than assumed to be the cube/sphere's -1..1 box.
//
// MATCHED, vertex-for-vertex convention (winding, normal style, units), by SandboxApp.cpp's own
// appendDrone. See appendBoxYaw's header comment, above, for why this is a copy and not a shared call.
void appendDrone(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    // Central body: a squarish box, flatter than it is wide -- real quadcopter chassis proportions.
    constexpr f32 kBodyHX = 0.26f, kBodyHY = 0.26f, kBodyHZ = 0.15f;
    appendBoxYaw(v, idx, 0, 0, 0, kBodyHX, kBodyHY, kBodyHZ, 0.0f);

    // Four arms, out to the corners (45/135/225/315 degrees), each ending in a motor pod and a rotor
    // disc -- see this function's own header comment for why kArmROuter + kDiscRadius is exactly 1.0.
    constexpr f32 kArmAngleDeg[4] = {45.0f, 135.0f, 225.0f, 315.0f};
    constexpr f32 kArmRInner = 0.34f;  // just inside the body's own corner (0.26*sqrt2 = 0.368) -- no seam
    constexpr f32 kArmROuter = 0.80f;  // hub distance from the drone's centre
    constexpr f32 kArmHalfLen = (kArmROuter - kArmRInner) * 0.5f;
    constexpr f32 kArmCenterR = (kArmROuter + kArmRInner) * 0.5f;
    constexpr f32 kArmHalfWidth = 0.045f, kArmHalfThick = 0.032f;
    constexpr f32 kHubRadius = 0.11f, kHubHalfHeight = 0.05f, kHubCenterZ = 0.08f;
    constexpr f32 kDiscRadius = 0.20f, kDiscHalfHeight = 0.014f, kDiscCenterZ = 0.14f;
    for (f32 deg : kArmAngleDeg) {
        const f32 rad = deg * kDegToRad;
        const f32 armX = kArmCenterR * std::cos(rad), armY = kArmCenterR * std::sin(rad);
        appendBoxYaw(v, idx, armX, armY, 0.0f, kArmHalfLen, kArmHalfWidth, kArmHalfThick, deg);

        const f32 hubX = kArmROuter * std::cos(rad), hubY = kArmROuter * std::sin(rad);
        appendCylinderZ(v, idx, hubX, hubY, kHubCenterZ, kHubRadius, kHubHalfHeight, 8);
        // The rotor disc stands in for the swept area of a spinning prop that a static placeholder
        // mesh cannot animate -- a flat approximation, stated here rather than left for someone to
        // wonder why a "propeller" never turns.
        appendCylinderZ(v, idx, hubX, hubY, kDiscCenterZ, kDiscRadius, kDiscHalfHeight, 10);
    }

    // A pair of landing skids plus the four short struts that stand them off the body's underside.
    constexpr f32 kSkidHalfLen = 0.30f, kSkidHalfWidth = 0.02f, kSkidHalfThick = 0.018f;
    constexpr f32 kSkidY = 0.20f, kSkidZ = -0.20f;
    appendBoxYaw(v, idx, 0.0f,  kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);
    appendBoxYaw(v, idx, 0.0f, -kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);

    constexpr f32 kStrutHalfX = 0.02f, kStrutHalfY = 0.02f, kStrutHalfZ = 0.016f;
    // Midpoint between the body's underside (-kBodyHZ = -0.15) and the skid's top (kSkidZ +
    // kSkidHalfThick = -0.182).
    constexpr f32 kStrutX = 0.16f, kStrutZ = -0.166f;
    for (f32 sx : {-kStrutX, kStrutX})
        for (f32 sy : {-kSkidY, kSkidY})
            appendBoxYaw(v, idx, sx, sy, kStrutZ, kStrutHalfX, kStrutHalfY, kStrutHalfZ, 0.0f);
}

} // namespace

void GameContent::registerBuiltins(rhi::IDevice& device) {
    // FROZEN: the unit cube stays half-extent 1. A .ocworld PLACEG scale is a half-extent in
    // centimetres applied to this mesh, so changing it silently resizes every placed box in every
    // level ever authored. The same constant is frozen in SandboxApp.cpp with the same note.
    // BOUNDS ARE RECORDED FOR THE BUILT-INS, which SandboxApp does not do. Both are generated at
    // radius/half-extent 1, so the box is exactly known and costs nothing to write down.
    //
    // This is not tidiness. A CMeshRenderer whose bounds were never filled in presents a DEGENERATE
    // box, and the draw walk deliberately draws a degenerate box rather than culling it -- an entity
    // whose bounds are unknown must not vanish. The consequence in the editor is that every entity
    // using a built-in primitive is exempt from frustum culling entirely, including ones directly
    // behind the camera. Measured here: a five-placement level reported "5 drawn, 0 culled" from
    // every camera angle until these two lines existed.
    const std::pair<Vec3, Vec3> unitBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    const auto add = [&](const std::string& path, const std::vector<rhi::MeshVertex>& v,
                         const std::vector<u32>& i, const std::pair<Vec3, Vec3>& bounds) {
        const u64 id = fnv1a64(std::string_view(path));
        const rhi::MeshHandle h = device.createMesh(v.data(), (u32)v.size(), i.data(), (u32)i.size());
        sceneMeshes_[id] = h;
        meshBounds_[id]  = bounds;
        if (meshLoaded_) meshLoaded_(LoadedMesh{id, path, nullptr, v, i, h}, meshLoadedUser_);
    };
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendSphere(v, i, 1.0f, 24, 48);
        add("Meshes/sphere.ocmesh", v, i, unitBounds);
    }
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendBox(v, i, 0, 0, 0, 1.0f);
        add("Meshes/cube.ocmesh", v, i, unitBounds);
    }
    {
        // Third built-in: the quadcopter appendDrone (above) builds, for the graph-driven drone actor
        // (SandboxApp.cpp's setDroneEnabled) that used to spawn as a bare unit cube.
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendDrone(v, i);
        // NOT unitBounds: appendDrone is not isotropic (see its own comment for the exact per-axis
        // reach), so recording the cube/sphere's -1..1 box here would be roughly six times too tall
        // and would silently defeat the frustum cull the comment over this function already exists to
        // fix -- for a different reason (loose rather than missing) than the one that comment
        // describes. Padded a few thousandths beyond the generator's own exact numbers (X/Y tip reach
        // 0.765685..., top 0.154, skid bottom -0.218) rather than trimmed to them -- a bound must
        // never be tighter than the geometry it describes.
        add("Meshes/drone.ocmesh", v, i, {Vec3{-0.78f, -0.78f, -0.22f}, Vec3{0.78f, 0.78f, 0.16f}});
    }

    // The named surfaces gameplay can ask for, with the editor's exact values.
    //
    // THIS TABLE MUST STAY IN STEP WITH sandbox/src/SandboxApp.cpp's OWN look()/surfaceLooks_ BLOCK
    // (search "The named surfaces gameplay can ask for" there), and there is nothing that enforces
    // that beyond this comment and the one over there. A VERIFIED PARITY BUG lived here until this
    // edit: the editor's table names TEN surfaces, this one named only SEVEN -- M_Foliage, M_Bark
    // and M_Rock were missing entirely. A level authored in the editor using one of those three,
    // with no backing .ocmat, rendered its intended colour in the editor and fell through to the
    // flat 0.80/0.80/0.85 gray fallback (see GameRender.cpp's drawWorld) the moment the packaged
    // game ran the SAME level -- with nothing in the log to say why, until GameRender.cpp's
    // one-shot "no authored .ocmat and no built-in look" warning was added alongside this fix.
    // Whoever adds an eleventh name to the editor's table and forgets this one reproduces exactly
    // that bug, silently, again.
    auto look = [this](const char* name, f32 r, f32 g, f32 b, f32 metal, f32 rough) {
        surfaceLooks_[aver_scene_material(0, name)] = SurfaceLook{{r, g, b}, metal, rough};
    };
    look("M_Floor",  0.22f, 0.23f, 0.26f, 0.02f, 0.85f);
    look("M_Wall",   0.48f, 0.50f, 0.55f, 0.03f, 0.72f);
    // A GENERIC SURFACE IN THE ENGINE'S OWN DEFAULT PALETTE, alongside M_Floor/M_Wall/M_Metal above
    // -- not an entry that exists because one project asked for it. Every name in this table is a
    // common architectural surface the engine is willing to give a sensible look to when a level
    // names it and no .ocmat defines it.
    //
    // It was added after a scene naming it fell through to the flat {0.80,0.80,0.85} fallback and
    // rendered as undifferentiated near-white -- which got reported as a lighting bug when it was
    // content resolving to nothing, identically in BOTH render paths. That is the failure mode this
    // whole table exists to prevent, and concrete was simply a hole in it.
    //
    // KEEP THIS TABLE AND GameContent.cpp/SandboxApp.cpp IN STEP -- they have diverged before.
    look("M_Concrete", 0.55f, 0.54f, 0.51f, 0.00f, 0.88f);
    look("M_Trim",   0.30f, 0.33f, 0.38f, 0.35f, 0.45f);
    look("M_Crate",  0.62f, 0.44f, 0.22f, 0.02f, 0.78f);
    look("M_Target", 0.86f, 0.20f, 0.16f, 0.05f, 0.40f);
    look("M_Metal",  0.55f, 0.57f, 0.60f, 0.85f, 0.28f);
    look("M_Accent", 0.95f, 0.66f, 0.15f, 0.30f, 0.35f);
    // Copied verbatim from sandbox/src/SandboxApp.cpp's table. Ordinary outdoor vocabulary, not tied to any one demo project;
    // see that file's own comment on why a former "M_Foliage" scatter default was removed and
    // these three names were kept anyway.
    look("M_Foliage", 0.16f, 0.42f, 0.14f, 0.00f, 0.85f);
    look("M_Bark",    0.35f, 0.24f, 0.15f, 0.00f, 0.85f);
    look("M_Rock",    0.42f, 0.40f, 0.37f, 0.05f, 0.80f);
    // M_Glass: the editor's values (SandboxApp.cpp's table).
    //
    // NOT TRANSLUCENT: SurfaceLook (GameContent.hpp) has no alphaMode field
    // at all -- it is a colour and a metal/rough pair, nothing else. GameRender.cpp's
    // drawWorld only ever sets device.setDrawBlended(true) for an AUTHORED .ocmat whose alphaMode
    // reads AlphaMode::Blend (pbr::MaterialLibrary::desc()); a built-in look, this one included, can
    // never trigger the blended path. A level using "M_Glass" with no backing .ocmat therefore
    // renders an OPAQUE near-white cube, not glass -- an improvement over the flat gray default (and
    // over the one-shot "no built-in look" warning this would otherwise trip), but still opaque.
    // Actual translucency needs an authored M_Glass.ocmat with BLEND set; this entry is a fallback
    // for the case where one was never authored, not a substitute for authoring one.
    look("M_Glass", 0.92f, 0.94f, 0.95f, 0.00f, 0.05f);

    AVER_INFO("[Mesh] {} built-in primitive(s), {} named surface(s)", sceneMeshes_.size(), surfaceLooks_.size());
}

void GameContent::loadProjectMeshes(rhi::IDevice& device) {
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Mesh) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); ++failed; continue; }

        // Position, normal and uv ONLY. rhi::MeshVertex is 32 bytes and has nowhere to put joints
        // or weights, so a skinned asset arrives here as static geometry -- correct, because the
        // skinning path uploads its own target mesh and resolves through resolveSceneMesh.
        std::vector<rhi::MeshVertex> verts(md.vertexCount());
        for (u32 i = 0; i < md.vertexCount(); ++i) {
            rhi::MeshVertex& v = verts[i];
            v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
            v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
            v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
        }
        const rhi::MeshHandle h = device.createMesh(verts.data(), (u32)verts.size(),
                                                   md.indices.data(), (u32)md.indices.size());
        if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); ++failed; continue; }

        const u64 id = fnv1a64(std::string_view(rel));
        sceneMeshes_[id] = h;
        meshBounds_[id] = {md.boundsMin, md.boundsMax};
        // THE MESH'S OWN MATERIAL. .ocmesh has always carried a materialSlots table and nothing here
        // read it, so an entity that named no material drew flat grey even though the mesh said what
        // it was. See GameContent.hpp's meshDefaultMaterial for why 0 means "ask the mesh".
        if (!md.materialSlots.empty() && !md.materialSlots[0].empty()) {
            meshSlot0Material_[id] = aver_scene_material(0, md.materialSlots[0].c_str());
            meshSlot0Name_[id] = md.materialSlots[0];
        }
        // THE PER-SUBMESH SPLIT. .ocmesh has always carried a submeshes table alongside
        // materialSlots, and until now nothing here read it either: a mesh naming several materials
        // (bark and leaves, say) drew as one mesh in slot 0's material end to end. See MeshPart's own
        // comment (GameContent.hpp) and buildMeshParts' (below) for the shape this mirrors.
        buildMeshParts(device, id, md, verts, rel);
        projectMeshIds_.push_back(id);
        if (meshLoaded_) meshLoaded_(LoadedMesh{id, rel, &md, verts, md.indices, h}, meshLoadedUser_);

        // THE COARSE STAND-IN THE SHADOW, GI-SHADOW AND VOXELISE PASSES DRAW INSTEAD OF THIS MESH.
        // Those passes are depth-only -- they resolve a silhouette, never a surface -- so detail a
        // coarser level drops is detail they were never going to show. Same rule, same threshold and
        // same reasoning as the editor's ladder (sandbox/src/SandboxApp.cpp's kShadowErrorCm block):
        // chosen on Trifactor's measured world error in centimetres, not on a triangle ratio, because
        // how coarse a level is says nothing about how wrong it looks. 20cm is about one shadow-map
        // texel, below which the silhouette cannot change the shadow.
        //
        // WHERE THIS DELIBERATELY DIVERGES FROM THE EDITOR: the editor uploads the WHOLE ladder and
        // keys the map on every level's handle, because with --lod-select its lit pass submits
        // whichever level it chose and each of those handles has to resolve. The game has no runtime
        // LOD selection at all -- GameRender.cpp's draw walk only ever submits meshFor(mr->mesh)
        // itself (this handle, `h`, LOD 0 and nothing else) or, for a mesh buildMeshParts split below,
        // one of ITS handles -- neither route ever looks up a coarser level -- so uploading the rest
        // of the ladder would be VRAM that nothing can ever look up. The pick needs no upload to
        // compute (levelWorldErrorCm reads the OcMeshData), so it is computed first and exactly ONE
        // extra level is uploaded. The day the game learns to select, this becomes the editor's loop
        // again.
        //
        // A SPLIT MESH'S PARTS HAVE NO PROXY: this map is keyed on `h`, the whole mesh's handle, and
        // buildMeshParts' parts are separate handles it never mentions, so the depth passes draw
        // each part at full detail.
#if AVER_MODULE_TRIFACTOR
        if (buildDepthProxies_ && md.lodCount() > 1) {
            constexpr f32 kShadowErrorCm = 20.0f;
            u32 pick = 0;
            for (u32 lvl = 1; lvl < md.lodCount(); ++lvl)
                if (trifactor::levelWorldErrorCm(md, lvl) <= kShadowErrorCm) pick = lvl;

            // A pick that is not actually cheaper than LOD 0 buys an upload and saves nothing.
            const u32 tris0 = trifactor::levelTriangleCount(md, 0);
            if (pick > 0 && trifactor::levelTriangleCount(md, pick) < tris0) {
                const fmt::OcMeshLod& lod = md.coarserLods[pick - 1];
                // LOD 0's OWN vertex array: a coarser level owns its index buffer but shares the one
                // VTXS block (see OcMeshData::coarserLods), which is why `verts` is correct here.
                const rhi::MeshHandle ph = device.createMesh(verts.data(), (u32)verts.size(),
                                                             lod.indices.data(), (u32)lod.indices.size());
                if (ph) {
                    depthProxyMap_[h] = ph;
                    AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, {:.1f}cm error)",
                              rel, pick, trifactor::levelTriangleCount(md, pick),
                              static_cast<f64>(tris0) /
                                  static_cast<f64>(trifactor::levelTriangleCount(md, pick)),
                              trifactor::levelWorldErrorCm(md, pick));
                } else {
                    AVER_WARN("[Mesh] '{}' depth proxy LOD {} refused by the device; it draws at full detail",
                              rel, pick);
                }
            }
        }
#endif

        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}

// Splits a mesh that names more than one material into one MeshHandle per slot. Ported from
// SandboxApp::buildMeshParts (sandbox/src/SandboxAssets.cpp) -- see that function's own comment for
// why splitting at load time, rather than drawing per-submesh RANGES, is what makes this tractable at
// all (the ray path's BLAS carries one materialIndex per instance, so a range draw would still shade
// flat in the renderer that is actually on screen).
//
// COMPACTED PER PART, not sharing the parent's vertex array. createMesh COPIES what it is given, so
// handing every part of a multi-material mesh the WHOLE vertex buffer would upload that buffer once
// per part. The remap also gives each part honest bounds, which a future per-part culler would want
// anyway.
void GameContent::buildMeshParts(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md,
                                  const std::vector<rhi::MeshVertex>& verts, const std::string& rel) {
    if (md.submeshes.size() <= 1) return;   // the common case: nothing to split

    std::vector<MeshPart> parts;
    parts.reserve(md.submeshes.size());
    std::unordered_map<u32, u32> remap;
    std::vector<rhi::MeshVertex> pv;
    std::vector<u32> pi;
    // THE UNREMAPPED SLICES, kept for a SKINNED mesh only. Each part below is compacted and
    // renumbered, which is right for a static mesh and useless over a posed buffer: a skin target
    // keeps the BASE mesh's vertex numbering (it shares the base index buffer verbatim), so only a
    // slice of md.indices in that numbering can be re-cut over the pose. See posedPartsFor.
    const bool keepBaseIndices = md.hasSkin();
    std::vector<std::vector<u32>> baseIndices;

    for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
        if (sm.indexCount == 0) continue;
        const usize end = usize(sm.indexStart) + sm.indexCount;
        if (end > md.indices.size()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' runs past the index buffer; skipped", rel, sm.name);
            continue;
        }
        remap.clear(); pv.clear(); pi.clear();
        pi.reserve(sm.indexCount);
        bool bad = false;
        for (usize k = sm.indexStart; k < end; ++k) {
            const u32 vi = md.indices[k];
            if (vi >= verts.size()) { bad = true; break; }
            const auto [it2, inserted] = remap.try_emplace(vi, static_cast<u32>(pv.size()));
            if (inserted) pv.push_back(verts[vi]);
            pi.push_back(it2->second);
        }
        if (bad || pv.empty()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' indexes a vertex it does not have; skipped", rel, sm.name);
            continue;
        }

        MeshPart part;
        part.mesh = device.createMesh(pv.data(), static_cast<u32>(pv.size()),
                                       pi.data(), static_cast<u32>(pi.size()));
        if (!part.mesh) {
            AVER_WARN("[Mesh] the device refused submesh '{}' of '{}'", sm.name, rel);
            continue;
        }
        // THE SLOT NAMES THE MATERIAL, which is the whole point of the format's slot table -- and
        // the cook writes those names as the .ocmat stems it produced, so a name resolves through
        // exactly the path an authored material does (GameContent::materialForSurface).
        if (sm.materialSlot < md.materialSlots.size()) {
            const std::string& slot = md.materialSlots[sm.materialSlot];
            if (!slot.empty()) part.material = aver_scene_material(0, slot.c_str());
        }
        parts.push_back(part);
        // IN LOCKSTEP with `parts`: pushed only for a part that survived every check above, so
        // baseIndices[i] is always the slice parts[i] was cut from.
        if (keepBaseIndices)
            baseIndices.emplace_back(md.indices.data() + sm.indexStart, md.indices.data() + end);
    }

    // ONE SURVIVING PART IS NOT A SPLIT. Falling through to the ordinary single-mesh path costs a
    // draw call less and keeps the entity's own material override meaningful.
    if (parts.size() <= 1) {
        for (const MeshPart& p : parts) if (p.mesh) device.destroyMesh(p.mesh);
        return;
    }
    AVER_INFO("[Mesh] '{}' names {} materials; split into {} part(s) so each draws its own",
              rel, md.materialSlots.size(), parts.size());
    meshParts_[id] = std::move(parts);
    if (keepBaseIndices) meshPartBaseIndices_[id] = std::move(baseIndices);
}

const std::vector<GameContent::MeshPart>* GameContent::partsFor(u64 id) const {
    const auto it = meshParts_.find(id);
    return it == meshParts_.end() ? nullptr : &it->second;
}

const std::vector<GameContent::MeshPart>* GameContent::posedPartsFor(rhi::IDevice& device, u64 id,
        rhi::MeshHandle baseMesh, rhi::MeshHandle posedMesh) {
    if (!posedMesh || !baseMesh || posedMesh == baseMesh) return nullptr;
    if (const auto it = posedParts_.find(posedMesh); it != posedParts_.end())
        return (it->second.meshId == id && !it->second.parts.empty()) ? &it->second.parts : nullptr;
    PosedParts& entry = posedParts_[posedMesh];   // recorded now, so a refusal is not retried per frame
    entry.meshId = id;
    const auto pit = meshParts_.find(id);
    const auto bit = meshPartBaseIndices_.find(id);
    // Every refusal below WARNS, once per posed handle (the entry above caches it): each one leaves
    // a multi-material character drawing whole under one material, and a log that says nothing
    // makes that look like the cut-out bug this path exists to fix.
    if (pit == meshParts_.end() || bit == meshPartBaseIndices_.end() ||
        bit->second.size() != pit->second.size()) {
        if (pit != meshParts_.end())   // no parts at all is a single-material mesh: nothing to say
            AVER_WARN("[Mesh] posed split skipped for mesh {} (posed handle {}): its per-slot index "
                      "slices were not kept; it draws as one mesh", id, posedMesh);
        return nullptr;
    }
    // PROOF THE POSED COPY IS NUMBERED LIKE THIS UPLOAD, not an assumption: a skin target shares its
    // source's index buffer verbatim (createSkinTargetMesh), so equal index buffers and equal vertex
    // counts mean it was cut from `baseMesh` itself. After an editor mesh reload they differ, and the
    // entity keeps its single whole-mesh draw rather than drawing indices against the wrong vertices.
    rhi::BufferHandle baseIb = 0, posedIb = 0;
    u32 baseVc = 0, posedVc = 0;
    if (!device.meshGeometry(baseMesh, nullptr, &baseIb, &baseVc, nullptr) ||
        !device.meshGeometry(posedMesh, nullptr, &posedIb, &posedVc, nullptr) ||
        baseIb == 0 || baseIb != posedIb || baseVc != posedVc) {
        AVER_WARN("[Mesh] posed split skipped for mesh {}: posed handle {} was not cut from this "
                  "upload (base {}; reloaded since it was skinned?); it draws as one mesh",
                  id, posedMesh, baseMesh);
        return nullptr;
    }
    std::vector<MeshPart> out;
    out.reserve(pit->second.size());
    for (usize i = 0; i < pit->second.size(); ++i) {
        MeshPart pp;
        pp.material = pit->second[i].material;
        const std::vector<u32>& idx = bit->second[i];
        if (pit->second[i].mesh && !idx.empty()) {
            pp.mesh = device.createPosedPartMesh(posedMesh, idx.data(), static_cast<u32>(idx.size()));
            // ALL OR NOTHING: a half-split character would silently lose the geometry of every part
            // that failed, where the whole-mesh fallback at least draws all of it.
            if (!pp.mesh) {
                for (const MeshPart& q : out) if (q.mesh) device.destroyMesh(q.mesh);
                AVER_WARN("[Mesh] posed split refused for mesh {} (posed handle {}); it draws as one mesh",
                          id, posedMesh);
                return nullptr;
            }
        }
        out.push_back(pp);
    }
    entry.parts = std::move(out);
    AVER_INFO("[Mesh] mesh {}: {} posed part(s) over posed handle {}", id, entry.parts.size(), posedMesh);
    return &entry.parts;
}

void GameContent::registerMesh(u64 id, rhi::MeshHandle handle, const std::pair<Vec3, Vec3>& bounds) {
    sceneMeshes_[id] = handle;
    meshBounds_[id] = bounds;
}

void GameContent::releaseProjectMeshes(rhi::IDevice& device, bool destroyBaseHandles) {
    // POSED PARTS FIRST: each holds a vertex share on a skin target (which would otherwise refuse its
    // own destruction while they live), and each was cut from a meshPartBaseIndices_ entry about to go.
    for (auto& kv : posedParts_)
        for (const MeshPart& p : kv.second.parts)
            if (p.mesh) device.destroyMesh(p.mesh);
    posedParts_.clear();
    for (const u64 id : projectMeshIds_) {
        meshPartBaseIndices_.erase(id);
        // The split parts are this class's own uploads, and nothing keys anything else on them.
        if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
            for (const MeshPart& p : pit->second)
                if (p.mesh) device.destroyMesh(p.mesh);
            meshParts_.erase(pit);
        }
        if (const auto sit = sceneMeshes_.find(id); sit != sceneMeshes_.end()) {
            // The depth proxy is keyed on the base handle, so it goes before that handle does.
            if (const auto dit = depthProxyMap_.find(sit->second); dit != depthProxyMap_.end()) {
                if (dit->second) device.destroyMesh(dit->second);
                depthProxyMap_.erase(dit);
            }
            if (destroyBaseHandles && sit->second) device.destroyMesh(sit->second);
            sceneMeshes_.erase(sit);
        }
        meshBounds_.erase(id);
        meshSlot0Material_.erase(id);
        meshSlot0Name_.erase(id);
        collisionMeshCache_.erase(id);
    }
    projectMeshIds_.clear();
}

const std::pair<Vec3, Vec3>* GameContent::boundsFor(u64 id) const {
    const auto it = meshBounds_.find(id);
    return it == meshBounds_.end() ? nullptr : &it->second;
}

// Concave architecture needs its triangles: NewSponza's per-material merged meshes (walls, arches,
// ...) each span the whole building, so world::addStaticBoxBody's one box per mesh fills the
// courtyard and buries anyone standing in it. This is the lazily-built source those triangles come
// from -- see GameContent.hpp's own comment on the shape of the answer and what null means.
const GameContent::CollisionMesh* GameContent::collisionMeshFor(u64 id) {
    if (const auto it = collisionMeshCache_.find(id); it != collisionMeshCache_.end())
        return it->second.get();

    // INSERTED NOW, EVEN ON FAILURE: every `return nullptr` below leaves this null entry behind, so
    // the next ask for the same id is a hash lookup, not a re-read of a file that was never going to
    // parse (or a re-stat of a path that was never going to exist, e.g. every built-in id).
    std::unique_ptr<CollisionMesh>& slot = collisionMeshCache_[id];

    const std::string path = pathFor(id);
    if (path.empty()) return nullptr;   // a built-in (never indexed) or an id nothing recognises

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) {
        AVER_WARN("[Collision] {}", why);
        return nullptr;
    }

    // THE COARSEST LOD WITHIN kCollisionMaxErrorCm, same search shape as loadProjectMeshes' depth-
    // proxy pick just above (monotonic non-decreasing error, level 0 always qualifies at 0.0f) --
    // but a different threshold and a different reason: a depth pass only needs a correct
    // silhouette, while collision needs a shape a player cannot obviously clip through, so the
    // budget here is centimetres a human can feel, not a shadow-map texel.
    //
    // GATED ON TRIFACTOR, like the depth-proxy pick: coarserLods is data the FILE carries regardless
    // of which module baked it, but reading its error back out in world units goes through
    // aver::trifactor::levelWorldErrorCm (OcMeshLod::screenErrorThreshold is worldErrorCm *
    // kReferenceProjScale -- that function is the one divide back to centimetres). Without Trifactor
    // linked, `pick` stays 0 -- LOD 0, always exact, just heavier -- which is the same fallback a
    // mesh with no coarser level at all already gets.
    u32 pick = 0;
    f32 pickErrorCm = 0.0f;
#if AVER_MODULE_TRIFACTOR
    constexpr f32 kCollisionMaxErrorCm = 2.0f;
    for (u32 lvl = 1; lvl < md.lodCount(); ++lvl) {
        const f32 err = trifactor::levelWorldErrorCm(md, lvl);
        if (err <= kCollisionMaxErrorCm) { pick = lvl; pickErrorCm = err; }
    }
#endif

    const std::vector<u32>& srcIndices = pick == 0 ? md.indices : md.coarserLods[pick - 1].indices;
    const u32 vertexCount = md.vertexCount();
    if (srcIndices.size() < 3 || vertexCount == 0) return nullptr;

    // COMPACTED to only the vertices this LOD's triangles reference: a coarser level shares LOD 0's
    // whole `positions` array (OcMeshData::coarserLods' own comment) rather than owning a smaller
    // one, and NewSponza's coarsest levels touch a small fraction of it -- physics has no use for
    // carrying the rest of a 3.75M-vertex building along for a 182k-triangle collision proxy.
    auto mesh = std::make_unique<CollisionMesh>();
    mesh->lod = pick;
    mesh->errorCm = pickErrorCm;
    std::unordered_map<u32, u32> remap;
    remap.reserve(srcIndices.size());
    mesh->indices.reserve(srcIndices.size());
    for (usize k = 0; k + 2 < srcIndices.size(); k += 3) {
        const u32 ia = srcIndices[k], ib = srcIndices[k + 1], ic = srcIndices[k + 2];
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) continue;   // out of range
        if (ia == ib || ib == ic || ia == ic) continue;   // degenerate: no area, nothing to collide with
        for (const u32 orig : {ia, ib, ic}) {
            const auto [it2, inserted] =
                remap.try_emplace(orig, static_cast<u32>(mesh->positions.size() / 3));
            if (inserted) {
                mesh->positions.push_back(md.positions[usize(orig) * 3 + 0]);
                mesh->positions.push_back(md.positions[usize(orig) * 3 + 1]);
                mesh->positions.push_back(md.positions[usize(orig) * 3 + 2]);
            }
            mesh->indices.push_back(it2->second);
        }
    }
    if (mesh->indices.size() < 3) return nullptr;   // every triangle was degenerate or out of range

    const usize tris0 = md.indices.size() / 3;
    AVER_INFO("[Collision] {}: LOD {}, {} tris (from {} at LOD 0), {:.1f} cm error", path, pick,
              mesh->indices.size() / 3, tris0, pickErrorCm);
    slot = std::move(mesh);
    return slot.get();
}

rhi::MeshHandle GameContent::resolveSceneMesh(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->meshFor(id) : 0;
}

const GameContent::SurfaceLook* GameContent::lookFor(i32 material) const {
    const auto it = surfaceLooks_.find(material);
    return it == surfaceLooks_.end() ? nullptr : &it->second;
}

#endif // AVER_MODULE_SCENE

#if AVER_MODULE_PBR

std::string GameContent::resolveAssetPath(const pbr::TextureRef& ref) const {
    if (!ref.path.empty()) {
        const std::string& p = ref.path;
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute) return p;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            const std::string full = content + "\\" + p;
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) return full;
        }
        return p;
    }
    if (ref.id) return pathFor(ref.id);
    return {};
}

pbr::MaterialSystem::ResolvedTexture GameContent::resolveMaterialTexture(const pbr::TextureRef& ref,
                                                                         pbr::TextureSlot slot, void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_) return {};

    const std::string path = self->resolveAssetPath(ref);
    if (path.empty()) {
        AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps its fallback",
                  ref.id, pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }

    // THE SLOT DECIDES THE COLOUR SPACE, NEVER THE FILENAME. A normal map read as sRGB is a subtly
    // wrong lighting response that looks like a shading bug rather than a decode bug.
    assets::TextureUsage usage = assets::TextureUsage::Data;
    // THE LAYER-1 SLOTS BELONG HERE TOO, and their absence was a decode bug rather than an omission
    // of principle. MaterialSystem::colourClass classifies Layer1BaseColor as 'c' (sRGB) and
    // Layer1Normal as 'n', and its own comment says "KEEP THIS IN STEP WITH THOSE TWO SWITCHES" --
    // this being one of them. It drifted: everything not named fell through to Data, so a
    // slope-blended material's SECOND base-colour layer was uploaded LINEAR when its pixels are sRGB.
    //
    // WHAT THAT LOOKS LIKE is why it went unnoticed: decoding sRGB texels as linear does not corrupt
    // them, it LIFTS the midtones and flattens the contrast -- the layer reads pale and washed out
    // beside the layer 0 it blends against, which reads as a lighting or blending problem rather than
    // as a colour-space one. Layer1Normal had the matching fault the other way: routed to Data it lost
    // the normal-map-aware mip generation that NormalMap selects.
    switch (slot) {
        case pbr::TextureSlot::BaseColor:
        case pbr::TextureSlot::Layer1BaseColor:
        case pbr::TextureSlot::Emissive:      usage = assets::TextureUsage::Colour;    break;
        case pbr::TextureSlot::Normal:
        case pbr::TextureSlot::Layer1Normal:  usage = assets::TextureUsage::NormalMap; break;
        default:                              usage = assets::TextureUsage::Data;      break;
    }

    std::string err;
    assets::TextureUploadInfo info;
    const rhi::TextureHandle h = assets::uploadTexture(*self->textureFactory_, path, usage, &err, &info);
    if (!h) {
        AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err,
                  pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }
    AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
              info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
    // The mean travels with the handle -- see MaterialSystem::ResolvedTexture for why anything
    // that cannot sample a texture needs it.
    pbr::MaterialSystem::ResolvedTexture out;
    out.handle = h;
    for (int c = 0; c < 3; ++c) out.averageLinear[c] = info.averageLinear[c];
    return out;
}

// Turns an .ocmat's GRAPHREF path into the gMaterialGraphId its constants carry. 0 for a material
// with no GRAPHREF, and 0 for one whose graph will not load or compile. Ported from
// SandboxApp::resolveMaterialGraph (sandbox/src/SandboxAssets.cpp).
//
// A broken graph does not take the material down with it: returning 0 falls back to the stock
// .ocmat factors/maps instead of vanishing the object entirely. Logged either way.
u32 GameContent::resolveMaterialGraph(const std::string& graphRef) const {
    if (graphRef.empty()) return 0;
    const std::string content = project_.contentDir();
    if (content.empty()) return 0;

    // CONTENT-RELATIVE, the same convention COMP mesh= uses in .ocgraph and TEX uses in
    // materialForSurface's candidate paths: a path with the content directory on the front resolves
    // to nothing, silently, which is a mistake worth not repeating here.
    std::string path = content + "\\" + graphRef;
    for (char& c : path) if (c == '/') c = '\\';

    // ALREADY COMPILED? Two materials naming one graph is ordinary -- a stone and a wet stone
    // sharing a pattern -- and asking the registry first means the graph is read and compiled once,
    // and both materials get the same id rather than two arms doing the same arithmetic.
    if (const u32 known = pbr::materialGraphs().idOf(path)) return known;

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::loadOcgraph(path, g, &err)) {
        AVER_ERROR("[MaterialGraph] '{}' could not be read, so the material shades as a stock "
                   "one: {}", path, err);
        return 0;
    }
    return pbr::materialGraphs().add(path, g.name, g);
}

pbr::MaterialHandle GameContent::materialForSurface(const std::string& name) {
    if (name.empty()) return 0;
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // ORDER MATTERS: a BUILT .ocmat under Binaries wins over a hand-authored one under Content,
        // because the built one is what avermatc produced from the C# source and is therefore the
        // one the ids in the level refer to.
        const std::string candidates[3] = {
            project_.binariesDir() + "\\Materials\\" + name + ".ocmat",
            content + "\\Materials\\" + name + ".ocmat",
            content + "\\" + name,
        };
        for (const std::string& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) continue;
            pbr::MaterialDesc d;
            fmt::OcMatExtras extras;
            std::string err;
            // A parse failure BREAKS rather than falling through to the next candidate: a corrupt
            // built material must not be silently replaced by a stale hand-authored one.
            if (!fmt::loadOcmat(path, d, &extras, &err)) { AVER_WARN("[Material] {}", err); break; }
            d.graphId = resolveMaterialGraph(extras.graphRef);
            h = pbr::MaterialLibrary::get().create(d);
            if (h) AVER_INFO("[Material] '{}' loaded from {}{}", d.name, path,
                              d.graphId ? " (graph " + std::to_string(d.graphId) + ")" : "");
            break;
        }
    }
    // Caches 0 as a negative result and never retries. Deliberate: a project with fifty unauthored
    // surfaces would otherwise stat three paths per surface per level load.
    materialAssets_.emplace(name, h);
    return h;
}

void GameContent::releaseProjectMaterials(bool clearGraphRegistry) {
    // Destroyed, not just forgotten: MaterialLibrary owns the material, this map only names it.
    for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
    materialAssets_.clear();
    // resolveMaterialGraph() caches into this process-wide registry by compiled path (its own
    // idOf()), so a project close has to forget the graphs too -- otherwise a differently-authored
    // project reusing the same content-relative GRAPHREF path would inherit stale ids (or a reload
    // of the SAME project would just leak entries forever, since idOf() never expires them itself).
    if (clearGraphRegistry) pbr::materialGraphs().clear();
#if AVER_MODULE_SCENE
    surfaceMaterials_.clear();
#endif
}

#endif // AVER_MODULE_PBR

#if AVER_MODULE_SCENE
i32 GameContent::meshDefaultMaterial(u64 meshId) const {
    const auto it = meshSlot0Material_.find(meshId);
    return it == meshSlot0Material_.end() ? 0 : it->second;
}

const std::string& GameContent::meshSlot0Name(u64 meshId) const {
    static const std::string kNone;
    const auto it = meshSlot0Name_.find(meshId);
    return it == meshSlot0Name_.end() ? kNone : it->second;
}
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
pbr::MaterialHandle GameContent::authoredFor(i32 token) const {
    const auto it = surfaceMaterials_.find(token);
    return it == surfaceMaterials_.end() ? 0 : it->second;
}
#endif

#if AVER_MODULE_PARTICLES
void GameContent::loadProjectParticleEffects() {
    // The table is process-global: without this a reload keeps effects whose files are gone.
    particles::particleEffects().clear();
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Particle) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        particles::ParticleEffect fx;
        std::string err;
        if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
            AVER_WARN("[Particles] {}", err);
            ++failed;
            continue;
        }

        // The SAME id a CParticleEmitter::effect placed by a level or set by a script names --
        // see this method's own header comment on why that is fnv1a64(relative path) and not
        // something GameContent invents.
        particles::particleEffects().set(fnv1a64(std::string_view(rel)), fx);
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}
#endif // AVER_MODULE_PARTICLES

// OUTSIDE EVERY GUARD, matching the declaration -- see GameContent.hpp for why a lookup in
// the content index's mesh table is not scene state, and what an empty table means.
rhi::MeshHandle GameContent::meshFor(u64 id) const {
    const auto it = sceneMeshes_.find(id);
    return it == sceneMeshes_.end() ? 0 : it->second;
}

} // namespace aver::game
