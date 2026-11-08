// Scene2dAbiTest -- scene2d_abi.h: the version, the struct layouts (against the C++ components, the managed mirrors in
// Aver.Framework/Scene2D.cs read as source, and the sizes the DLL reports), set/get round trips, rejections with their reasons,
// and the world affine and sprite bounds. No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene2d_abi.h"

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;
using namespace aver::scene2d;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string stripLineComments(const std::string& s) {
    std::string out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (const size_t p = line.find("//"); p != std::string::npos) line.erase(p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out += line + "\n";
    }
    return out;
}

static std::string stripBlockComments(std::string s) {
    for (size_t a = s.find("/*"); a != std::string::npos; a = s.find("/*")) {
        const size_t b = s.find("*/", a + 2);
        s.erase(a, (b == std::string::npos ? s.size() : b + 2) - a);
    }
    return s;
}

static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The scalar kinds both languages use, normalised to the C# spelling.
static std::string scalar(const std::string& t) {
    if (t == "int32_t") return "int";
    if (t == "uint32_t") return "uint";
    if (t == "int64_t") return "long";
    return t;
}
static int sizeOfScalar(const std::string& t) { return t == "long" ? 8 : 4; }

// The flat scalar sequence of the body between the braces: C declares `float position[2];`, C# `float position0, position1;`.
static std::vector<std::string> flatC(const std::string& body) {
    std::vector<std::string> out;
    std::istringstream in(body);
    std::string stmt;
    while (std::getline(in, stmt, ';')) {
        stmt = trim(stmt);
        if (stmt.empty()) continue;
        const size_t sp = stmt.find_first_of(" \t");
        const std::string type = scalar(stmt.substr(0, sp));
        const std::string name = trim(stmt.substr(sp));
        int n = 1;
        if (const size_t lb = name.find('['); lb != std::string::npos) n = std::stoi(name.substr(lb + 1));
        for (int i = 0; i < n; ++i) out.push_back(type);
    }
    return out;
}
static std::vector<std::string> flatCs(const std::string& body) {
    std::vector<std::string> out;
    std::istringstream in(body);
    std::string stmt;
    while (std::getline(in, stmt, ';')) {
        stmt = trim(stmt);
        if (stmt.rfind("public ", 0) != 0) continue;
        stmt = trim(stmt.substr(7));
        const size_t sp = stmt.find_first_of(" \t");
        const std::string type = stmt.substr(0, sp);
        const std::string names = stmt.substr(sp);
        int n = 1;
        for (char c : names) n += c == ',';
        for (int i = 0; i < n; ++i) out.push_back(type);
    }
    return out;
}
// Sequential layout with natural alignment, as StructLayout.Sequential does for blittable scalars.
static int sizeOfSeq(const std::vector<std::string>& v) {
    int off = 0, maxAlign = 1;
    for (const auto& t : v) {
        const int s = sizeOfScalar(t);
        off = (off + s - 1) / s * s + s;
        maxAlign = std::max(maxAlign, s);
    }
    return (off + maxAlign - 1) / maxAlign * maxAlign;
}

static std::string bodyOf(const std::string& text, const std::string& marker) {
    const size_t p = text.find(marker);
    if (p == std::string::npos) return {};
    const size_t a = text.find('{', p);
    const size_t b = text.find('}', a);
    return a == std::string::npos || b == std::string::npos ? std::string{} : text.substr(a + 1, b - a - 1);
}

int main() {
    // ---- version and layout ----
    check(aver_scene2d_abi_version() == AVER_SCENE2D_ABI_VERSION && AVER_SCENE2D_ABI_VERSION == 3, "the ABI version is 3 and is what the header says");
    check(sizeof(AverScene2dTransform) == 32 && sizeof(AverScene2dSprite) == 56 && sizeof(AverScene2dLayer) == 32, "struct sizes: 32, 56, 32");
    check(aver_scene2d_struct_size(0) == 32 && aver_scene2d_struct_size(1) == 56 && aver_scene2d_struct_size(2) == 32 && aver_scene2d_struct_size(8) == 0,
          "the DLL reports the same sizes (the managed handshake) and 0 for anything else");
    check(sizeof(AverScene2dAnimFrame) == 32 && sizeof(AverScene2dAnimState) == 32 && sizeof(AverScene2dAnimEvent) == 16 && sizeof(AverScene2dCamera) == 56 &&
              sizeof(AverScene2dParallax) == 56,
          "the animation, camera and parallax struct sizes: 32, 32, 16, 56, 56");
    check(offsetof(AverScene2dAnimFrame, region) == 8 && offsetof(AverScene2dAnimFrame, weight) == 24 && offsetof(AverScene2dAnimFrame, cell) == 28 &&
              offsetof(AverScene2dAnimState, clip) == 8 && offsetof(AverScene2dAnimState, frameCount) == 28 && offsetof(AverScene2dCamera, limit) == 16 &&
              offsetof(AverScene2dCamera, drag) == 36 && offsetof(AverScene2dCamera, flags) == 52 && offsetof(AverScene2dParallax, repeatTimes) == 24 &&
              offsetof(AverScene2dParallax, flags) == 52,
          "and their field offsets are pinned");
    check(offsetof(AverScene2dTransform, position) == 0 && offsetof(AverScene2dTransform, rotation) == 8 && offsetof(AverScene2dTransform, scale) == 12 &&
              offsetof(AverScene2dTransform, zIndex) == 20 && offsetof(AverScene2dTransform, flags) == 24 && offsetof(AverScene2dTransform, treeOrder) == 28,
          "AverScene2dTransform field offsets are pinned");
    check(offsetof(AverScene2dSprite, texture) == 0 && offsetof(AverScene2dSprite, region) == 8 && offsetof(AverScene2dSprite, offset) == 24 &&
              offsetof(AverScene2dSprite, modulate) == 32 && offsetof(AverScene2dSprite, flags) == 36 && offsetof(AverScene2dSprite, hframes) == 40 &&
              offsetof(AverScene2dSprite, vframes) == 44 && offsetof(AverScene2dSprite, frame) == 48 && offsetof(AverScene2dSprite, blend) == 52,
          "AverScene2dSprite field offsets are pinned");
    check(offsetof(AverScene2dLayer, index) == 0 && offsetof(AverScene2dLayer, flags) == 4 && offsetof(AverScene2dLayer, offset) == 8 &&
              offsetof(AverScene2dLayer, rotation) == 16 && offsetof(AverScene2dLayer, scale) == 20 && offsetof(AverScene2dLayer, reserved) == 28,
          "AverScene2dLayer field offsets are pinned");

#ifdef AVER_REPO_ROOT
    {
        const std::string root = std::string(AVER_REPO_ROOT) + "/";
        const std::string hdr = stripBlockComments(stripLineComments(readAll(root + "modules/scene/include/aver/scene/scene2d_abi.h")));
        const std::string cs = stripBlockComments(stripLineComments(readAll(root + "scripting/csharp/Aver.Framework/Scene2D.cs")));
        check(!hdr.empty() && !cs.empty(), "the header and the managed mirror are readable");
        struct Row { const char* c; const char* cs; size_t size; };
        const Row rows[] = {{"AverScene2dTransform", "struct AverScene2dTransform", sizeof(AverScene2dTransform)},
                            {"AverScene2dSprite", "struct AverScene2dSprite", sizeof(AverScene2dSprite)},
                            {"AverScene2dLayer", "struct AverScene2dLayer", sizeof(AverScene2dLayer)},
                            {"AverScene2dAnimFrame", "struct AverScene2dAnimFrame", sizeof(AverScene2dAnimFrame)},
                            {"AverScene2dAnimState", "struct AverScene2dAnimState", sizeof(AverScene2dAnimState)},
                            {"AverScene2dAnimEvent", "struct AverScene2dAnimEvent", sizeof(AverScene2dAnimEvent)},
                            {"AverScene2dCamera", "struct AverScene2dCamera", sizeof(AverScene2dCamera)},
                            {"AverScene2dParallax", "struct AverScene2dParallax", sizeof(AverScene2dParallax)}};
        for (const Row& r : rows) {
            const auto c = flatC(bodyOf(hdr, std::string("typedef struct ") + r.c));
            const auto m = flatCs(bodyOf(cs, r.cs));
            check(!c.empty() && c == m, std::string(r.c) + ": the managed mirror has the same scalars in the same order");
            check(sizeOfSeq(m) == static_cast<int>(r.size), std::string(r.c) + ": and lays out to the same size under Sequential");
        }
        check(cs.find("AbiVersion = 3") != std::string::npos, "the managed binding names ABI version 3");
        check(cs.find("aver_scene2d_struct_size") != std::string::npos, "and checks the struct sizes before it calls anything");
    }
#endif

    World& w = World::instance();
    check(aver_scene2d_component_id(0) == 0 || ids().transform == static_cast<u32>(aver_scene2d_component_id(0)), "component ids agree with ids()");
    check(aver_scene2d_register() == 1, "register succeeds");
    const Ids id = ids();
    check(aver_scene2d_component_id(0) == static_cast<int>(id.transform) && aver_scene2d_component_id(1) == static_cast<int>(id.sprite) &&
              aver_scene2d_component_id(2) == static_cast<int>(id.layer) && aver_scene2d_component_id(3) == static_cast<int>(id.anim) &&
              aver_scene2d_component_id(4) == static_cast<int>(id.camera) && aver_scene2d_component_id(5) == static_cast<int>(id.parallax) && aver_scene2d_component_id(6) == 0,
          "component_id returns the six dynamic ids, 0 for anything else");
    check(w.componentId("C2DTransform") == id.transform && w.componentId("CSprite2D") == id.sprite && w.componentId("C2DLayer") == id.layer &&
              w.componentId("CSpriteAnim2D") == id.anim && w.componentId("CCamera2D") == id.camera && w.componentId("CParallax2D") == id.parallax,
          "and they are the names the generic scene ABI resolves");
    check(aver_scene2d_register() == 1 && aver_scene2d_component_id(0) == static_cast<int>(id.transform), "registering again changes nothing");

    // ---- round trips ----
    const Entity e = w.create("e");
    const int h = static_cast<int>(e);
    {
        AverScene2dTransform t{};
        check(aver_scene2d_get_transform(h, &t) == 0 && aver_scene_last_error() < 0, "an entity without the component reads 0 with a reason");
        t.position[0] = 12; t.position[1] = -3; t.rotation = 0.5f; t.scale[0] = 2; t.scale[1] = 3; t.zIndex = 7;
        t.flags = AVER_SCENE2D_T_Z_ABSOLUTE | AVER_SCENE2D_T_HIDDEN | 0x100; t.treeOrder = 99;
        check(aver_scene2d_set_transform(h, &t) == 1 && aver_scene_last_error() == 0, "set_transform adds the component and writes it");
        AverScene2dTransform r{};
        check(aver_scene2d_get_transform(h, &r) == 1, "and reads it back");
        check(r.position[0] == 12 && r.position[1] == -3 && r.rotation == 0.5f && r.scale[0] == 2 && r.scale[1] == 3 && r.zIndex == 7 && r.treeOrder == 99,
              "every field round-trips");
        check(r.flags == (AVER_SCENE2D_T_Z_ABSOLUTE | AVER_SCENE2D_T_HIDDEN), "unknown flag bits are dropped");
        const auto* c = w.component<C2DTransform>(e, id.transform);
        check(c && c->posX == 12 && c->scaleY == 3 && c->zIndex == 7 && c->treeOrder == 99, "the component in the world holds it");
    }
    {
        AverScene2dSprite s{};
        s.texture = 0x1234567890LL; s.region[0] = 1; s.region[1] = 2; s.region[2] = 3; s.region[3] = 4; s.offset[0] = 5; s.offset[1] = 6;
        s.modulate = 0x80FF8040u; s.flags = AVER_SCENE2D_S_FLIP_H | AVER_SCENE2D_S_NEAREST | 0x1000; s.hframes = 4; s.vframes = 2; s.frame = 5;
        s.blend = AVER_SCENE2D_BLEND_ADD;
        check(aver_scene2d_set_sprite(h, &s) == 1, "set_sprite writes");
        AverScene2dSprite r{};
        check(aver_scene2d_get_sprite(h, &r) == 1 && r.texture == s.texture && r.region[2] == 3 && r.offset[1] == 6 && r.modulate == 0x80FF8040u &&
                  r.hframes == 4 && r.vframes == 2 && r.frame == 5 && r.blend == AVER_SCENE2D_BLEND_ADD,
              "every sprite field round-trips");
        check(r.flags == (AVER_SCENE2D_S_FLIP_H | AVER_SCENE2D_S_NEAREST), "unknown sprite flag bits are dropped");
    }
    {
        AverScene2dLayer l{};
        l.index = -2; l.flags = AVER_SCENE2D_L_HIDDEN; l.offset[0] = 9; l.offset[1] = 8; l.rotation = 0.25f; l.scale[0] = 1.5f; l.scale[1] = 2.5f; l.reserved = 77;
        check(aver_scene2d_set_layer(h, &l) == 1, "set_layer writes");
        AverScene2dLayer r{};
        check(aver_scene2d_get_layer(h, &r) == 1 && r.index == -2 && r.flags == AVER_SCENE2D_L_HIDDEN && r.offset[0] == 9 && r.rotation == 0.25f &&
                  r.scale[1] == 2.5f && r.reserved == 0,
              "every layer field round-trips and reserved reads zero");
    }

    // ---- rejections ----
    {
        AverScene2dTransform t{};
        t.position[0] = std::nanf("");
        check(aver_scene2d_set_transform(h, &t) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "a NaN position is InvalidArgument");
        t = {};
        t.zIndex = 5000;
        check(aver_scene2d_set_transform(h, &t) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "a z-index past 4096 is InvalidArgument");
        AverScene2dTransform keep{};
        aver_scene2d_get_transform(h, &keep);
        check(keep.position[0] == 12 && keep.zIndex == 7, "and a rejected set writes nothing");
        AverScene2dSprite s{};
        s.blend = 9;
        check(aver_scene2d_set_sprite(h, &s) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "a blend outside 0..3 is InvalidArgument");
        s = {};
        s.region[3] = INFINITY;
        check(aver_scene2d_set_sprite(h, &s) == 0, "an infinite region is refused");
        AverScene2dLayer l{};
        l.scale[1] = std::nanf("");
        check(aver_scene2d_set_layer(h, &l) == 0, "a NaN layer scale is refused");
        check(aver_scene2d_set_transform(h, nullptr) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::NullPointer), "a NULL descriptor is NullPointer");
        check(aver_scene2d_get_transform(h, nullptr) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::NullPointer), "a NULL out is NullPointer");
        t = {};
        check(aver_scene2d_set_transform(0, &t) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::BadHandle), "entity 0 is BadHandle");
        check(aver_scene2d_set_transform(0x7FFFFF, &t) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::BadHandle), "a stale entity is BadHandle");
        check(aver_scene2d_remove(h, 7) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::InvalidArgument), "removing component 7 is InvalidArgument");
    }

    // ---- world affine, bounds, remove ----
    {
        const Entity p = w.create("parent");
        const Entity c = w.create("child", p, Transform{});
        AverScene2dTransform tp{};
        tp.position[0] = 100; tp.position[1] = 50; tp.scale[0] = 2; tp.scale[1] = 2; tp.zIndex = 3;
        AverScene2dTransform tc{};
        tc.position[0] = 10; tc.zIndex = 4;
        aver_scene2d_set_transform(static_cast<int>(p), &tp);
        aver_scene2d_set_transform(static_cast<int>(c), &tc);
        check(aver_scene2d_update_transforms() >= 2, "update_transforms recomposes the new entities");
        float m[6] = {};
        int z = -1;
        check(aver_scene2d_world_affine(static_cast<int>(c), m, &z) == 1, "world_affine reads a composed entity");
        check(approx(m[0], 2) && approx(m[3], 2) && approx(m[4], 120) && approx(m[5], 50) && z == 7, "a child under a doubled parent: scale 2, origin (120, 50), z 3 + 4");
        check(aver_scene2d_world_affine(static_cast<int>(c), m, nullptr) == 1, "outZ may be NULL");
        const Entity bare = w.create("bare");
        check(aver_scene2d_world_affine(static_cast<int>(bare), m, &z) == 0 && m[0] == 1 && m[4] == 0 && z == 0, "no C2DTransform: 0, the identity written");

        AverScene2dSprite sp{};
        sp.texture = 1; sp.region[2] = 32; sp.region[3] = 16;
        aver_scene2d_set_sprite(static_cast<int>(c), &sp);
        float b[4] = {};
        check(aver_scene2d_sprite_bounds(static_cast<int>(c), b) == 1, "sprite_bounds works from a region without any resolver");
        // 32x16 centred on (120,50), scaled by 2 -> 64x32.
        check(approx(b[0], 88) && approx(b[1], 34) && approx(b[2], 64) && approx(b[3], 32), "and returns the transformed box (x, y, w, h)");
        sp.region[2] = 0; sp.region[3] = 0;
        aver_scene2d_set_sprite(static_cast<int>(c), &sp);
        check(aver_scene2d_sprite_bounds(static_cast<int>(c), b) == 0 && aver_scene_last_error() == static_cast<int>(AbiError::Unsupported),
              "with no region and no resolver the size is unknown");
        setTextureResolver([](i64, TextureInfo* out, void*) { *out = {5, 16, 8}; return true; }, nullptr);
        check(aver_scene2d_sprite_bounds(static_cast<int>(c), b) == 1 && approx(b[2], 32) && approx(b[3], 16), "the resolver supplies the image size");
        reset();

        check(aver_scene2d_remove(static_cast<int>(c), 1) == 1 && aver_scene2d_get_sprite(static_cast<int>(c), &sp) == 0, "remove takes a component off");
        check(aver_scene2d_remove(static_cast<int>(c), 1) == 0, "and a second remove reports it was not there");
    }

    reset();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
