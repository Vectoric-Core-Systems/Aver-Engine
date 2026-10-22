// HairSystem.cpp -- strand hair on scene entities. See HairSystem.hpp for the model.
#include "aver/hair/HairSystem.hpp"

#include "HairProxies.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/hair/HairProxy.hpp"
#include "aver/hair/HairSettingsBuild.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

namespace aver::hair {

namespace {

u32 g_hairType = 0;
u32 g_drawType = 0;

// The engine cube: the placeholder mesh of the draw child, whose draw the ribbon mesh replaces.
u64 placeholderMeshId() {
    static const u64 id = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    return id;
}

std::string normalisedRel(const std::string& p) {
    std::string r = p;
    for (char& c : r)
        if (c == '\\') c = '/';
    return r;
}

f32 scaleOf(const Mat4& m) {
    const f32 sx = Vec3(m.m[0][0], m.m[0][1], m.m[0][2]).size();
    return sx > 1.0e-6f ? sx : 1.0f;
}

// A sample of the strands for the editor overlay: at most 192 of them, each as up to 10 segments, 2 points (x y z) per segment, hair space.
void sampleGuides(const fmt::OcHairStrands& strands, std::vector<f32>& out) {
    out.clear();
    const usize n = strands.strandCount();
    if (n == 0 || strands.start.size() < n + 1) return;
    const usize step = std::max<usize>(1, n / 192);
    for (usize s = 0; s < n; s += step) {
        const u32 first = strands.start[s];
        const u32 count = strands.start[s + 1] - first;
        if (count < 2 || (static_cast<usize>(first) + count) * 3 > strands.points.size()) continue;
        const u32 segs = std::min<u32>(count - 1, 10u);
        u32 prev = 0;
        for (u32 k = 1; k <= segs; ++k) {
            const u32 cur = (k * (count - 1) + segs / 2) / segs;
            const f32* a = &strands.points[(static_cast<usize>(first) + prev) * 3];
            const f32* b = &strands.points[(static_cast<usize>(first) + cur) * 3];
            out.insert(out.end(), {a[0], a[1], a[2], b[0], b[1], b[2]});
            prev = cur;
        }
    }
}

// True when the physics world exists: aver_phys_hair_create answers NotInitialised (-3) before it looks at anything else.
bool physicsReady() {
    const f32 id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    aver_phys_hair_create(0, id, 0);
    return aver_phys_last_error() != -3;
}

} // namespace

// ================================================================================ components

u32 HairSystem::registerComponents(scene::World& world) {
    using scene::FieldKind;
    {
        auto b = world.registerComponent<CHair>("CHair");
        b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CHair, asset)))
            .field("material", FieldKind::I64, static_cast<u16>(offsetof(CHair, material)))
            .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CHair, flags)))
            .field("widthScale", FieldKind::F32, static_cast<u16>(offsetof(CHair, widthScale)))
            .field("densityScale", FieldKind::F32, static_cast<u16>(offsetof(CHair, densityScale)))
            .field("lodBias", FieldKind::F32, static_cast<u16>(offsetof(CHair, lodBias)))
            .field("windScale", FieldKind::F32, static_cast<u16>(offsetof(CHair, windScale)))
            .field("stiffnessScale", FieldKind::F32, static_cast<u16>(offsetof(CHair, stiffnessScale)))
            .field("teleportCm", FieldKind::F32, static_cast<u16>(offsetof(CHair, teleportCm)))
            .field("reserved", FieldKind::I32, static_cast<u16>(offsetof(CHair, reserved)))
            .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CHair, outState)), 0, true)
            .field("outRenderStrands", FieldKind::I32, static_cast<u16>(offsetof(CHair, outRenderStrands)), 0, true)
            .field("outSimStrands", FieldKind::I32, static_cast<u16>(offsetof(CHair, outSimStrands)), 0, true)
            .field("outReserved", FieldKind::I32, static_cast<u16>(offsetof(CHair, outReserved)), 0, true);
        AVER_ASSERTM(b.verify(sizeof(CHair)), "CHair");
        g_hairType = b.typeId();
    }
    {
        auto b = world.registerComponent<CHairDraw>("CHairDraw");
        b.field("owner", FieldKind::Entity, static_cast<u16>(offsetof(CHairDraw, owner)))
            .field("reserved", FieldKind::I32, static_cast<u16>(offsetof(CHairDraw, reserved)));
        AVER_ASSERTM(b.verify(sizeof(CHairDraw)), "CHairDraw");
        g_drawType = b.typeId();
    }
    return g_hairType;
}

u32 HairSystem::hairType() { return g_hairType; }
u32 HairSystem::drawType() { return g_drawType; }

CHair* HairSystem::attachHair(scene::World& world, scene::Entity e, u64 asset) {
    if (!g_hairType || !world.valid(e) || world.hasComponent(e, g_hairType)) return nullptr;
    auto* c = static_cast<CHair*>(world.addComponent(e, g_hairType));
    if (!c) return nullptr;
    *c = CHair{};
    c->asset = asset;
    return c;
}

std::string HairSystem::materialNameOf(u64 key) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "Hair/%016llX", static_cast<unsigned long long>(key));
    return buf;
}

// ================================================================================ state

struct HairSystem::Impl {
    // One built groom, shared by every instance of the same asset.
    struct Groom {
        u64 key = 0;
        i32 settings = 0;
        fmt::OcHairData data;                      // strands released after the build
        std::vector<i32> strandTable;              // {first render vertex, count} per render strand
        std::vector<f32> widthRoot, widthTip;      // per render strand, cm
        u32 renderStrands = 0, renderVertices = 0, simStrands = 0;
        f32 bounds[6] = {};                        // hair space, cm, engine axes
        i32 scalpJoints = 1;
        HairLookView look;
        f32 lodDistances[3] = {300, 700, 1500};
        f32 lodFractions[4] = {1.0f, 0.5f, 0.25f, 0.12f};
        u64 estimatedBytes = 0;
        f32 appliedStiffness = 1.0f;
        i32 refs = 0;
        std::vector<f32> guides;                   // sampleGuides of the strands, for Show > Hair
        i32 materialToken = 0;                     // aver_scene_material of the groom's generated surface name, interned on first use
    };

    struct Inst {
        std::shared_ptr<Groom> groom;
        i32 hair = 0;
        u64 asset = 0;
        scene::Entity drawEntity = scene::kInvalidEntity;
        std::vector<u32> jointBone;                // the skeleton bone of each scalp joint
        std::vector<f32> palette;                  // 16 floats per scalp joint
        std::vector<detail::ProxyBody> proxies;
        HairCadenceState cadence;
        bool teleportNext = true;
        bool settled = false;
        u32 lodLevel = 0;
        Vec3 lastPos{0, 0, 0};
        bool havePos = false;
        const fmt::OcSkeleton* skel = nullptr;
        u64 renderBuffer = 0;
        bool simulatedThisTick = false;
        u32 cadenceLevel = 0;
        usize groomProxies = 0;                    // the first this many entries of `proxies` came from the groom; the rest are a script's
        u32 colliderApplied = 0;                   // how many of Impl::colliders[entity] already have a body
    };

    std::map<scene::Entity, Inst> insts;
    std::unordered_map<u64, std::weak_ptr<Groom>> grooms;
    std::map<scene::Entity, u64> failed;           // entity -> the asset it failed to build (latched until the asset changes)
    std::map<scene::Entity, std::vector<fmt::OcHairProxy>> colliders;   // what scripts asked for, kept across a rebuild
    std::unordered_map<u64, i32> lookTokens;       // .ocmat object id -> the interned surface name it draws with
    std::set<u64> lookWarned;
    scene::World* world = nullptr;
    bool layersApplied = false;
    std::vector<Mat4> model;                       // scratch: the skeleton's model matrices
    anim::Pose restScratch;

    void destroyInstance(scene::World& w, scene::Entity e, bool destroyDraw);
    bool build(HairSystem& sys, scene::World& w, scene::Entity e, const CHair& ch, Inst& out, std::string& why, bool& retry);
    scene::Entity ensureDraw(HairSystem& sys, scene::World& w, scene::Entity owner, const Groom& g);
    void releaseGroom(const std::shared_ptr<Groom>& g);
};

HairSystem::HairSystem() : impl_(std::make_unique<Impl>()) {}

HairSystem::~HairSystem() = default;

HairSystem& hairSystem() {
    static HairSystem s;
    return s;
}

bool HairSystem::installComputeHost(const AverPhysComputeHostV1* table) {
    return aver_phys_hair_set_compute_host(table) != 0;
}

// ================================================================================ lifetime

void HairSystem::Impl::releaseGroom(const std::shared_ptr<Groom>& g) {
    if (!g) return;
    if (--g->refs > 0) return;
    if (g->settings) aver_phys_hair_settings_destroy(g->settings);
    g->settings = 0;
    grooms.erase(g->key);
}

void HairSystem::Impl::destroyInstance(scene::World& w, scene::Entity e, bool destroyDraw) {
    const auto it = insts.find(e);
    if (it == insts.end()) return;
    Inst& in = it->second;
    if (in.hair) aver_phys_hair_destroy(in.hair);
    in.hair = 0;
    detail::destroyProxies(in.proxies);
    if (destroyDraw && in.drawEntity != scene::kInvalidEntity && w.valid(in.drawEntity) && !w.destroyPending(in.drawEntity))
        w.destroy(in.drawEntity);
    releaseGroom(in.groom);
    insts.erase(it);
}

void HairSystem::clear() {
    Impl& m = *impl_;
    // Instances before settings: a settings object is refused while an instance uses it.
    while (!m.insts.empty()) {
        const auto it = m.insts.begin();
        Impl::Inst& in = it->second;
        if (in.hair) aver_phys_hair_destroy(in.hair);
        in.hair = 0;
        detail::destroyProxies(in.proxies);
        m.releaseGroom(in.groom);
        m.insts.erase(it);
    }
    m.grooms.clear();
    m.failed.clear();
    m.colliders.clear();
    m.lookTokens.clear();
    m.lookWarned.clear();
    m.layersApplied = false;
    views_.clear();
    stats_ = HairStats{};
}

bool HairSystem::teleport(scene::Entity e) {
    const auto it = impl_->insts.find(e);
    if (it == impl_->insts.end()) return false;
    it->second.teleportNext = true;
    return true;
}

i32 HairSystem::hairOf(scene::Entity e) const {
    const auto it = impl_->insts.find(e);
    return it == impl_->insts.end() ? 0 : it->second.hair;
}

bool HairSystem::isBuilt(scene::Entity e) const { return impl_->insts.count(e) != 0; }

bool HairSystem::addCollider(scene::World& world, scene::Entity e, const fmt::OcHairProxy& spec) {
    if (!g_hairType || !world.valid(e) || !world.hasComponent(e, g_hairType)) return false;
    if (!(spec.sizeCm[0] > 0.0f) || !std::isfinite(spec.sizeCm[0]) || !std::isfinite(spec.sizeCm[1]) || !std::isfinite(spec.sizeCm[2])) return false;
    if (spec.shape != fmt::OcHairProxyShape::Sphere && !(spec.sizeCm[1] > 0.0f)) return false;
    if (spec.shape == fmt::OcHairProxyShape::Box && !(spec.sizeCm[2] > 0.0f)) return false;
    auto& list = impl_->colliders[e];
    if (list.size() >= 16) return false;    // a handful of hulls is the design; more is a mistake
    list.push_back(spec);
    return true;
}

bool HairSystem::clearColliders(scene::Entity e) {
    Impl& m = *impl_;
    const auto cit = m.colliders.find(e);
    if (cit == m.colliders.end() || cit->second.empty()) return false;
    cit->second.clear();
    if (const auto it = m.insts.find(e); it != m.insts.end()) {
        detail::destroyProxiesFrom(it->second.proxies, it->second.groomProxies);
        it->second.colliderApplied = 0;
    }
    return true;
}

u32 HairSystem::colliderCount(scene::Entity e) const {
    const auto cit = impl_->colliders.find(e);
    return cit == impl_->colliders.end() ? 0u : static_cast<u32>(cit->second.size());
}

bool HairSystem::stateOf(scene::World& world, scene::Entity e, StateOut& out) const {
    out = StateOut{};
    if (!g_hairType || !world.valid(e)) return false;
    const auto* ch = world.component<CHair>(e, g_hairType);
    if (!ch) return false;
    out.built = impl_->insts.count(e) != 0;
    out.outState = ch->outState;
    out.renderStrands = ch->outRenderStrands;
    out.simStrands = ch->outSimStrands;
    out.colliders = colliderCount(e);
    return true;
}

const std::vector<f32>* HairSystem::guideSegments(scene::Entity e) const {
    const auto it = impl_->insts.find(e);
    return it == impl_->insts.end() || !it->second.groom ? nullptr : &it->second.groom->guides;
}

u32 HairSystem::debugProxies(scene::Entity e, std::vector<DebugProxy>& out) const {
    const auto it = impl_->insts.find(e);
    if (it == impl_->insts.end()) return 0;
    u32 added = 0;
    for (const detail::ProxyBody& b : it->second.proxies) {
        if (b.body == 0) continue;
        f32 p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
        if (!aver_phys_body_position(b.body, p) || !aver_phys_body_rotation(b.body, q)) continue;
        DebugProxy d;
        d.shape = b.shape;
        for (int i = 0; i < 3; ++i) d.sizeCm[i] = b.sizeCm[i];
        Transform t;
        t.position = Vec3(p[0], p[1], p[2]);
        t.rotation = Quat(q[0], q[1], q[2], q[3]);
        d.world = t.toMatrix();
        d.script = b.script;
        out.push_back(d);
        ++added;
    }
    return added;
}

// ================================================================================ the draw child

scene::Entity HairSystem::Impl::ensureDraw(HairSystem&, scene::World& w, scene::Entity owner, const Groom& g) {
    // A saved level carries the child: find it again through its marker before making another.
    if (scene::ComponentPool* pool = g_drawType ? w.pool(g_drawType) : nullptr) {
        for (usize i = 0; i < pool->size(); ++i) {
            const scene::Entity c = pool->entityAt(i);
            if (w.destroyPending(c)) continue;
            const auto* d = w.component<CHairDraw>(c, g_drawType);
            if (d && d->owner == owner) return c;
        }
    }
    const scene::Entity c = w.create("Hair", owner, Transform{});
    if (c == scene::kInvalidEntity) return c;
    auto* d = static_cast<CHairDraw*>(w.addComponent(c, g_drawType));
    if (d) { d->owner = owner; d->reserved = 0; }
    auto* mr = w.component<scene::CMeshRenderer>(c, scene::kComponentMeshRenderer);
    if (!mr) mr = static_cast<scene::CMeshRenderer*>(w.addComponent(c, scene::kComponentMeshRenderer));
    if (mr) {
        *mr = scene::CMeshRenderer{};
        mr->mesh = placeholderMeshId();
        mr->flags = 0;                      // invisible until the render side has a ribbon mesh to show (HairScene reveals it)
        mr->dirty = 1;
        mr->material = aver_scene_material(0, materialNameOf(g.key).c_str());
        mr->aabbMin[0] = g.bounds[0] - 2.0f; mr->aabbMin[1] = g.bounds[1] - 2.0f; mr->aabbMin[2] = g.bounds[2] - 2.0f;
        mr->aabbMax[0] = g.bounds[3] + 2.0f; mr->aabbMax[1] = g.bounds[4] + 2.0f; mr->aabbMax[2] = g.bounds[5] + 2.0f;
    }
    return c;
}

// ================================================================================ the build

bool HairSystem::Impl::build(HairSystem& sys, scene::World& w, scene::Entity e, const CHair& ch, Inst& out, std::string& why, bool& retry) {
    retry = false;
    if (!sys.pathFn_) { why = "no content path resolver is installed"; return false; }
    if (ch.asset == 0) { why = "the component names no asset"; return false; }
    if (!physicsReady()) { retry = true; why = "the physics world is not ready"; return false; }

    // ---- the skeleton the scalp is skinned to (optional: a groom on a rigid head has none) ----
    anim::AnimSystem& animSys = anim::animSystem();
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    const fmt::OcSkeleton* skel = sm ? animSys.skeleton(sm->skeleton) : nullptr;

    // ---- the groom: shared by every instance of the asset ----
    std::shared_ptr<Groom> groom;
    if (const auto it = grooms.find(ch.asset); it != grooms.end()) groom = it->second.lock();
    if (!groom) {
        const std::string path = sys.pathFn_(ch.asset, sys.pathUser_);
        if (path.empty()) { why = "the asset is not in the content index"; return false; }
        auto g = std::make_shared<Groom>();
        g->key = ch.asset;
        if (!fmt::loadOcHair(path, g->data, &why)) return false;
        fmt::OcHairData& data = g->data;

        BuildScalp scalp;
        if (data.hasScalp) {
            if (!scalpFromGroom(data, scalp, &why)) return false;
        } else {
            if (data.scalpMesh.empty()) { why = "the groom names no scalp mesh and embeds none"; return false; }
            const std::string meshPath = sys.pathFn_(fnv1a64(std::string_view(normalisedRel(data.scalpMesh))), sys.pathUser_);
            if (meshPath.empty()) { why = "the scalp mesh '" + data.scalpMesh + "' is not in the content index"; return false; }
            fmt::OcMeshData md;
            if (!fmt::loadOcMesh(meshPath, md, &why)) return false;
            std::vector<std::string> meshJoints;
            if (skel)
                for (const fmt::OcBone& b : skel->bones) meshJoints.push_back(b.name);
            if (!scalpFromMesh(md, data.scalpSubmesh, meshJoints, data.scalpJoints, scalp, &why)) return false;
        }

        BuildOptions bo;
        bo.maxRootToScalpCm = sys.opt_.maxRootToScalpCm;
        BuildResult result;
        if (!buildHairSettings(data, scalp, result, bo, &why)) {
            if (aver_phys_last_error() == -3) retry = true;
            return false;
        }
        g->settings = result.settings;

        i32 ints[AVER_PHYS_HAIRI_COUNT] = {};
        f32 floats[AVER_PHYS_HAIRF_COUNT] = {};
        aver_phys_hair_settings_info(g->settings, ints, AVER_PHYS_HAIRI_COUNT, floats, AVER_PHYS_HAIRF_COUNT);
        g->simStrands = static_cast<u32>(ints[AVER_PHYS_HAIRI_SIM_STRANDS]);
        g->renderStrands = static_cast<u32>(ints[AVER_PHYS_HAIRI_RENDER_STRANDS]);
        g->renderVertices = static_cast<u32>(ints[AVER_PHYS_HAIRI_RENDER_VERTICES]);
        g->scalpJoints = std::max(ints[AVER_PHYS_HAIRI_SCALP_JOINTS], 1);
        for (int i = 0; i < 6; ++i) g->bounds[i] = floats[AVER_PHYS_HAIRF_BOUNDS_MIN_X + i];

        auto refuse = [&](const std::string& msg) {
            aver_phys_hair_settings_destroy(g->settings);
            g->settings = 0;
            why = msg;
            return false;
        };
        if (aver_phys_hair_host_kind() == 0 && g->simStrands > sys.opt_.cpuMaxSimStrands)
            return refuse("the groom has " + std::to_string(g->simStrands) + " simulated strands and no GPU compute host is installed (the CPU backend is for tests and takes at most " +
                          std::to_string(sys.opt_.cpuMaxSimStrands) + ")");

        // The render table, in the order the render positions buffer holds the strands.
        const i32 table = aver_phys_hair_settings_render_strands(g->settings, nullptr, 0);
        if (table <= 0 || static_cast<usize>(table) != result.strands.strandCount()) return refuse("the physics layer's render strand table disagrees with the groom");
        g->strandTable.resize(static_cast<usize>(table) * 2);
        aver_phys_hair_settings_render_strands(g->settings, g->strandTable.data(), table);

        // Widths per strand: the explicit one if the groom has it, else its look's, tapering to the look's tip.
        g->widthRoot.resize(static_cast<usize>(table));
        g->widthTip.resize(static_cast<usize>(table));
        for (i32 s = 0; s < table; ++s) {
            const u32 mat = result.strands.material.empty() ? 0u : result.strands.material[static_cast<usize>(s)];
            const fmt::OcHairLook* lk = data.findLook(mat);
            const fmt::OcHairLook look = lk ? *lk : fmt::OcHairLook{};
            f32 root = look.widthRootCm;
            f32 tip = look.widthTipCm;
            if (!result.strands.width.empty()) {
                const f32 explicitRoot = result.strands.width[static_cast<usize>(s)];
                if (look.widthRootCm > 1.0e-6f) tip = look.widthTipCm * (explicitRoot / look.widthRootCm);
                root = explicitRoot;
            }
            g->widthRoot[static_cast<usize>(s)] = std::max(root, 0.001f);
            g->widthTip[static_cast<usize>(s)] = std::max(tip, 0.001f);
        }

        // The draw buffers: ribbon vertices (2 per render vertex, 32 bytes), the points (28 bytes), the indices (6 per segment) of the whole groom
        // and of its three thinner levels (about 0.9 more), and the render positions the physics layer keeps (12 bytes).
        const u64 verts = g->renderVertices;
        const u64 segs = verts > g->renderStrands ? verts - g->renderStrands : 0;
        g->estimatedBytes = verts * 2 * 32 + verts * 28 + segs * 6 * 4 * 2 + verts * 12;
        if (g->estimatedBytes > static_cast<u64>(sys.opt_.vramBudgetMB) * 1024ull * 1024ull)
            return refuse("its draw buffers would take " + std::to_string(g->estimatedBytes / (1024 * 1024)) + " MB, over the " +
                          std::to_string(sys.opt_.vramBudgetMB) + " MB hair budget");

        sampleGuides(result.strands, g->guides);

        // The look of the groom (its first LOOK record), the LOD record, then the strands are no longer needed.
        if (!data.looks.empty()) {
            const fmt::OcHairLook& l = data.looks.front();
            HairLookView& v = g->look;
            for (int i = 0; i < 3; ++i) { v.baseColor[i] = l.baseColor[i]; v.tipColor[i] = l.tipColor[i]; v.tint[i] = l.tint[i]; }
            v.colorVariation = l.colorVariation;
            v.roughness = l.roughness;
            v.shift = l.shift;
            v.tt = l.tt;
            v.trt = l.trt;
            v.azimuthRough = l.azimuthRough;
            v.scatter = l.scatter;
        }
        for (int i = 0; i < 3; ++i) g->lodDistances[i] = data.lod.distances[i];
        for (int i = 0; i < 4; ++i) g->lodFractions[i] = data.lod.strandFractions[i];
        std::vector<f32>().swap(data.strands.points);
        std::vector<u32>().swap(data.strands.start);
        std::vector<u8>().swap(data.strands.material);
        std::vector<f32>().swap(data.strands.width);
        grooms[ch.asset] = g;
        groom = g;
    }

    // ---- this instance ----
    Inst in;
    in.groom = groom;
    in.asset = ch.asset;
    in.skel = skel;
    const fmt::OcHairData& data = groom->data;
    if (skel && !data.scalpJoints.empty()) {
        for (const std::string& name : data.scalpJoints) {
            i32 found = -1;
            for (usize i = 0; i < skel->bones.size() && found < 0; ++i)
                if (skel->bones[i].name == name) found = static_cast<i32>(i);
            if (found < 0) {
                why = "scalp joint '" + name + "' is not in the skeleton";
                if (groom->refs == 0) { aver_phys_hair_settings_destroy(groom->settings); grooms.erase(groom->key); groom->settings = 0; }
                return false;
            }
            in.jointBone.push_back(static_cast<u32>(found));
        }
    } else {
        // A rigid head, or a groom that lists no joints: the scalp follows one bone (the root).
        const u32 root = skel && skel->rootBone < skel->bones.size() ? skel->rootBone : 0u;
        in.jointBone.assign(static_cast<usize>(groom->scalpJoints), root);
    }
    if (static_cast<i32>(in.jointBone.size()) != groom->scalpJoints) {
        why = "the groom's scalp skins to " + std::to_string(groom->scalpJoints) + " joints but lists " + std::to_string(in.jointBone.size());
        if (groom->refs == 0) { aver_phys_hair_settings_destroy(groom->settings); grooms.erase(groom->key); groom->settings = 0; }
        return false;
    }
    in.palette.assign(in.jointBone.size() * 16, 0.0f);
    for (usize j = 0; j < in.jointBone.size(); ++j)
        for (int k = 0; k < 4; ++k) in.palette[j * 16 + static_cast<usize>(k) * 5] = 1.0f;

    const Mat4& W = w.worldMatrix(e);
    const i32 layer = (ch.flags & kHairNoCollision) ? sys.opt_.freeLayer : sys.opt_.hairLayer;
    in.hair = aver_phys_hair_create(groom->settings, &W.m[0][0], layer);
    if (in.hair == 0) {
        why = "the physics layer refused the instance";
        if (aver_phys_last_error() == -3) retry = true;
        if (groom->refs == 0) { aver_phys_hair_settings_destroy(groom->settings); grooms.erase(groom->key); groom->settings = 0; }
        return false;
    }
    in.renderBuffer = aver_phys_hair_render_buffer(in.hair);
    ++groom->refs;

    if (!layersApplied) {
        detail::setupHairLayers(sys.opt_.hairLayer, sys.opt_.proxyLayer, sys.opt_.freeLayer);
        layersApplied = true;
    }
    if (skel && !(ch.flags & kHairNoCollision)) {
        std::vector<std::string> missing;
        const u32 made = detail::createProxies(data, *skel, scaleOf(W), sys.opt_.proxyLayer, in.proxies, &missing);
        if (made == 0)
            AVER_WARN("[Hair] entity {}: no collision proxies (the rig has no head / neck / shoulder / spine bones and the groom lists none); the hair "
                      "will pass through the body", e);
    }
    in.groomProxies = in.proxies.size();
    in.drawEntity = ensureDraw(sys, w, e, *groom);
    out = std::move(in);
    return true;
}

// ================================================================================ the tick

void HairSystem::tick(scene::World& world, f32 dt, const Vec3& view) {
    Impl& m = *impl_;
    m.world = &world;
    stats_ = HairStats{};
    views_.clear();
    if (!g_hairType) registerComponents(world);

    scene::ComponentPool* pool = world.pool(g_hairType);
    anim::AnimSystem& animSys = anim::animSystem();

    // ---- instances whose entity or component is gone ----
    {
        std::vector<scene::Entity> dead;
        for (const auto& kv : m.insts) {
            const scene::Entity e = kv.first;
            if (!world.valid(e) || world.destroyPending(e) || !world.hasComponent(e, g_hairType)) dead.push_back(e);
        }
        for (scene::Entity e : dead) {
            m.destroyInstance(world, e, world.valid(e) && !world.destroyPending(e));
            m.colliders.erase(e);
        }
        for (auto it = m.failed.begin(); it != m.failed.end();)
            it = (!world.valid(it->first) || !world.hasComponent(it->first, g_hairType)) ? m.failed.erase(it) : std::next(it);
    }
    if (!pool || dt <= 0.0f || !std::isfinite(dt)) return;

    bool anyUpdate = false;
    u32 builds = 0;
    for (usize pi = 0; pi < pool->size(); ++pi) {
        const scene::Entity e = pool->entityAt(pi);
        if (world.destroyPending(e)) continue;
        auto* ch = world.component<CHair>(e, g_hairType);
        if (!ch) continue;

        // ---- build, rebuild on a new asset ----
        auto it = m.insts.find(e);
        if (it != m.insts.end() && it->second.asset != ch->asset) {
            m.destroyInstance(world, e, true);
            it = m.insts.end();
        }
        if (it == m.insts.end()) {
            const auto f = m.failed.find(e);
            if (f != m.failed.end() && f->second == ch->asset) { ch->outState = kHairInert; ++stats_.inert; continue; }
            if (m.insts.size() >= opt_.maxInstances) { ch->outState = kHairInert; ++stats_.inert; continue; }
            if (builds >= opt_.maxBuildsPerFrame) { ch->outState = kHairBuilding; continue; }
            ++builds;
            Impl::Inst in;
            std::string why;
            bool retry = false;
            if (!m.build(*this, world, e, *ch, in, why, retry)) {
                if (retry) {
                    ch->outState = kHairBuilding;
                } else {
                    AVER_WARN("[Hair] entity {}: the groom was not built: {}", e, why);
                    m.failed[e] = ch->asset;
                    ch->outState = kHairInert;
                    ++stats_.inert;
                }
                continue;
            }
            it = m.insts.emplace(e, std::move(in)).first;
        }
        Impl::Inst& in = it->second;
        Impl::Groom& g = *in.groom;
        const Mat4& W = world.worldMatrix(e);

        // The animation system owns the skeleton and may reload it: look it up again rather than keep a pointer across frames.
        {
            const auto* sm = world.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
            in.skel = sm ? animSys.skeleton(sm->skeleton) : nullptr;
        }

        // Colliders a script asked for: one hull each, made once the skeleton can name the bone. A record whose bone is missing is dropped from the
        // wait (createProxy logs it) so it is not retried every frame.
        if (const auto cit = m.colliders.find(e); cit != m.colliders.end() && in.skel && in.colliderApplied < cit->second.size()) {
            for (; in.colliderApplied < cit->second.size(); ++in.colliderApplied) {
                const usize before = in.proxies.size();
                if (detail::createProxy(cit->second[in.colliderApplied], *in.skel, scaleOf(W), opt_.proxyLayer, in.proxies) && in.proxies.size() > before)
                    in.proxies.back().script = true;
            }
        }

        // ---- the distance rules ----
        const Vec3 pos(W.m[3][0], W.m[3][1], W.m[3][2]);
        const f32 distance = (pos - view).size();
        const f32 teleportCm = ch->teleportCm > 0.0f ? ch->teleportCm : opt_.teleportCm;
        if (in.havePos && (pos - in.lastPos).size() > teleportCm) in.teleportNext = true;
        in.lastPos = pos;
        in.havePos = true;
        const HairCadenceResult cad = stepHairCadence(opt_.cadence, in.cadence, distance, true, dt);

        // ---- feed the physics ----
        aver_phys_hair_set_transform(in.hair, &W.m[0][0]);

        u32 count = 0;
        const Mat4* skin = animSys.skinning(e, count);
        if (skin && count > 0) {
            for (usize j = 0; j < in.jointBone.size(); ++j) {
                const u32 b = in.jointBone[j];
                if (b >= count) continue;
                std::memcpy(&in.palette[j * 16], skin[b].m, 16 * sizeof(f32));
            }
        }
        aver_phys_hair_set_palette(in.hair, in.palette.data(), static_cast<i32>(in.jointBone.size()));

        // The world wind, scaled by the component; a hair that opts out gets none.
        const f32 windScale = ch->windScale > 0.0f ? ch->windScale : 1.0f;
        const bool windOn = !(ch->flags & kHairNoWind);
        const f32 wind[3] = {windOn ? wind_.x * windScale : 0.0f, windOn ? wind_.y * windScale : 0.0f, windOn ? wind_.z * windScale : 0.0f};
        aver_phys_hair_set_wind(in.hair, wind, windOn ? opt_.windDragPerS : 0.0f, windOn ? gust_ * windScale : 0.0f);

        // Stiffness is a property of the SHARED settings: the last component to ask wins for every instance of the groom.
        const f32 stiff = ch->stiffnessScale > 0.0f ? ch->stiffnessScale : 1.0f;
        if (std::fabs(stiff - g.appliedStiffness) > 1.0e-4f) {
            f32 block[kMaterialBlockSize];
            for (const fmt::OcHairMaterial& mat : g.data.materials) {
                materialBlock(mat, g.data.sim.simFraction, block);
                block[AVER_PHYS_HAIRM_BEND_COMPLIANCE] /= stiff;
                block[AVER_PHYS_HAIRM_STRETCH_COMPLIANCE] /= stiff;
                aver_phys_hair_settings_set_material(g.settings, static_cast<i32>(mat.index), block, kMaterialBlockSize);
            }
            g.appliedStiffness = stiff;
        }

        // The collision proxies follow the head's bones; a frozen groom leaves them where they are.
        if (!in.proxies.empty() && cad.level != HairCadence::Frozen && in.skel) {
            const anim::Pose* pose = animSys.pose(e);
            if (!pose) {
                anim::restPose(*in.skel, m.restScratch);
                pose = &m.restScratch;
            }
            anim::poseToModel(*in.skel, *pose, m.model);
            detail::moveProxies(in.proxies, m.model, W, dt);
        }

        in.simulatedThisTick = false;
        if (cad.update) {
            if (in.teleportNext || cad.teleport) {
                aver_phys_hair_teleport(in.hair);
                in.teleportNext = false;
            }
            if (aver_phys_hair_update(in.hair, cad.dt)) {
                in.simulatedThisTick = true;
                in.settled = true;
                anyUpdate = true;
                ++stats_.updatesLastTick;
            }
        }
        in.cadenceLevel = static_cast<u32>(cad.level);

        // ---- the draw level, the outputs, the view ----
        in.lodLevel = chooseHairLod(distance, g.lodDistances, 0.0f, in.lodLevel, 0.15f);
        const u32 drawLevel = biasedHairLod(in.lodLevel, ch->lodBias);
        const f32 densityScale = ch->densityScale > 0.0f ? ch->densityScale : 1.0f;
        const f32 fraction = hairDrawFraction(g.lodFractions, drawLevel, densityScale);

        u32 state = 0;
        if (in.simulatedThisTick || cad.level != HairCadence::Frozen) state |= kHairSimulating;
        if (cad.level == HairCadence::Frozen) { state &= ~kHairSimulating; state |= kHairFrozen; }
        state |= drawLevel << kHairLodShift;
        ch->outState = state;
        ch->outRenderStrands = static_cast<u32>(static_cast<f32>(g.renderStrands) * fraction + 0.5f);
        ch->outSimStrands = g.simStrands;
        if (state & kHairFrozen) ++stats_.frozen; else ++stats_.simulating;
        ++stats_.instances;
        stats_.proxies += static_cast<u32>(in.proxies.size());

        // The look override: CHair::material names an .ocmat, and the draw child then draws with that surface instead of the groom's generated one
        // (the host binds a surface name to the .ocmat of that stem). Zero, or an asset the content index does not know, keeps the groom's.
        if (in.drawEntity != scene::kInvalidEntity && world.valid(in.drawEntity)) {
            if (!g.materialToken) g.materialToken = aver_scene_material(0, materialNameOf(g.key).c_str());
            i32 want = g.materialToken;
            if (ch->material != 0) {
                const auto lt = m.lookTokens.find(ch->material);
                if (lt != m.lookTokens.end()) {
                    want = lt->second;
                } else {
                    const std::string path = pathFn_ ? pathFn_(ch->material, pathUser_) : std::string();
                    const std::string stem = std::filesystem::path(path).stem().string();
                    if (!stem.empty()) {
                        const i32 token = aver_scene_material(0, stem.c_str());
                        m.lookTokens[ch->material] = token;
                        want = token;
                    } else if (m.lookWarned.insert(ch->material).second) {
                        AVER_WARN("[Hair] entity {}: the look material 0x{:016X} is not in the content index; the groom's own look is used", e, ch->material);
                    }
                }
            }
            if (auto* mr = world.component<scene::CMeshRenderer>(in.drawEntity, scene::kComponentMeshRenderer); mr && mr->material != want) {
                mr->material = want;
                mr->dirty = 1;
            }
        }

        const bool hiddenByFlag = (ch->flags & kHairHidden) != 0;
        HairView v;
        v.entity = e;
        v.drawEntity = in.drawEntity;
        v.key = g.key;
        v.settings = g.settings;
        v.hair = in.hair;
        v.renderBuffer = in.renderBuffer;
        v.renderStrands = g.renderStrands;
        v.renderVertices = g.renderVertices;
        v.strandTable = g.strandTable.data();
        v.widthRootCm = g.widthRoot.data();
        v.widthTipCm = g.widthTip.data();
        v.seed = g.key ? g.key : 1ull;
        v.world = W;
        v.widthScale = ch->widthScale > 0.0f ? ch->widthScale : 1.0f;
        v.densityScale = densityScale;
        v.lodBias = ch->lodBias;
        v.flags = ch->flags;
        for (int i = 0; i < 3; ++i) v.lodDistances[i] = g.lodDistances[i];
        for (int i = 0; i < 4; ++i) v.lodFractions[i] = g.lodFractions[i];
        v.distanceCm = distance;
        v.lodLevel = drawLevel;
        v.look = g.look;
        v.materialName = materialNameOf(g.key);
        if (!g.materialToken) g.materialToken = aver_scene_material(0, v.materialName.c_str());
        v.materialToken = g.materialToken;
        v.visible = !hiddenByFlag && distance < opt_.hideBeyondCm;
        v.settled = in.settled;
        v.estimatedBytes = g.estimatedBytes;
        views_.push_back(std::move(v));
    }

    stats_.buildsLastTick = builds;
    stats_.groomsCached = static_cast<u32>(m.grooms.size());
    // One seal for everything this tick recorded.
    if (anyUpdate) aver_phys_hair_flush();
}

} // namespace aver::hair
