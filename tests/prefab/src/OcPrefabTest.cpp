// .ocprefab and the prefab-instance records of .ocworld: text round trips, validation, unknown
// records, and -- the one that matters for existing content -- that a level with no prefabs parses
// and writes exactly as before.
#include "aver/core/Log.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/formats/OcWorld.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static fmt::OcSaveField fI32(const char* n, i64 v) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindI32; f.i = v; return f; }
static fmt::OcSaveField fI64(const char* n, i64 v) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindI64; f.i = v; return f; }
static fmt::OcSaveField fStr(const char* n, const char* s) { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindString; f.s = s; return f; }
static fmt::OcSaveField fEnt(const char* n, i64 i, const char* path = "") { fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindEntity; f.i = i; f.s = path; return f; }
static fmt::OcSaveField fVec3(const char* n, f32 x, f32 y, f32 z) {
    fmt::OcSaveField f; f.name = n; f.kind = fmt::kOcPrefabKindVec3; f.f = {x, y, z}; return f;
}
static fmt::OcSaveComponent comp(const char* type, std::vector<fmt::OcSaveField> fields) {
    fmt::OcSaveComponent c; c.type = type; c.fields = std::move(fields); return c;
}
static fmt::OcPrefabOverride setOv(const char* path, const char* c, fmt::OcSaveField v) {
    fmt::OcPrefabOverride o; o.op = fmt::OcOverrideOp::Set; o.path = path; o.component = c; o.value = std::move(v); return o;
}

static fmt::OcPrefabData sample() {
    fmt::OcPrefabData p;
    p.name = "Crate # with spaces";
    {
        fmt::OcPrefabNode r; r.uid = p.allocUid(); r.parent = 0; r.name = "Crate";
        r.components.push_back(comp("CLocal", {fVec3("position", 0.1f, -0.0f, 1e-7f)}));
        r.components.push_back(comp("CMeshRenderer", {fI64("mesh", 0x7FFFFFFFFFFFFFF1ll), fStr("material", "M_Wood Dark")}));
        p.nodes.push_back(std::move(r));
    }
    {
        fmt::OcPrefabNode c; c.uid = p.allocUid(); c.parent = 1; c.name = "Lid";
        c.className = "AN_Lid";
        c.components.push_back(comp("CJoint", {fEnt("otherEntity", 1), fI32("jointType", -3)}));
        p.nodes.push_back(std::move(c));
    }
    {
        fmt::OcPrefabNode n; n.uid = p.allocUid(); n.parent = 1; n.name = "Handle"; n.prefab = "Prefabs/Handle.ocprefab";
        n.overrides.push_back(setOv("", "CLocal", fVec3("position", 0, 0, 50)));
        n.overrides.push_back(setOv("4", "CTags", fI32("bits", 9)));
        n.overrides.push_back(setOv("4", "CJoint", fEnt("otherEntity", 0, "")));
        n.overrides.push_back(setOv("4", "CJoint", fEnt("otherEntity", -1)));
        fmt::OcPrefabOverride add; add.op = fmt::OcOverrideOp::AddComponent; add.path = "4"; add.component = "CTags";
        fmt::OcPrefabOverride rm; rm.op = fmt::OcOverrideOp::RemoveComponent; rm.path = "4/7"; rm.component = "CLight";
        n.overrides.push_back(add);
        n.overrides.push_back(rm);
        n.overrides.push_back(setOv("", fmt::kOcPrefabNodeComponent, fStr("name", "Handle #1")));
        p.nodes.push_back(std::move(n));
    }
    return p;
}

static bool sameNodes(const fmt::OcPrefabData& a, const fmt::OcPrefabData& b) {
    if (a.name != b.name || a.nextUid != b.nextUid || a.nodes.size() != b.nodes.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (x.uid != y.uid || x.parent != y.parent || x.name != y.name || x.className != y.className ||
            x.prefab != y.prefab || x.components.size() != y.components.size() || x.overrides.size() != y.overrides.size())
            return false;
        for (size_t c = 0; c < x.components.size(); ++c) {
            if (x.components[c].type != y.components[c].type || x.components[c].fields.size() != y.components[c].fields.size()) return false;
            for (size_t f = 0; f < x.components[c].fields.size(); ++f)
                if (x.components[c].fields[f].name != y.components[c].fields[f].name ||
                    !fmt::ocPrefabFieldEqual(x.components[c].fields[f], y.components[c].fields[f])) return false;
        }
        for (size_t o = 0; o < x.overrides.size(); ++o) {
            const auto& u = x.overrides[o]; const auto& v = y.overrides[o];
            if (u.op != v.op || u.path != v.path || u.component != v.component) return false;
            if (u.op == fmt::OcOverrideOp::Set && (u.value.name != v.value.name || !fmt::ocPrefabFieldEqual(u.value, v.value))) return false;
        }
    }
    return true;
}

int main() {
    AVER_INFO("OcPrefabTest");

    AVER_INFO("an asset round-trips exactly");
    {
        const fmt::OcPrefabData p = sample();
        std::string why;
        check(p.valid(&why), "the sample is valid: " + why);
        const std::string text = fmt::writeOcPrefab(p);
        fmt::OcPrefabData q;
        check(fmt::parseOcPrefab(text, q, &why), "it parses: " + why);
        check(sameNodes(p, q), "every node, component, field and override survives");
        check(fmt::writeOcPrefab(q) == text, "writing the parsed copy gives the same bytes");
        const auto* mr = q.find(1) ? &q.find(1)->components[1] : nullptr;
        check(mr && mr->fields[0].i == 0x7FFFFFFFFFFFFFF1ll, "a 64-bit id keeps its top bits");
        check(mr && mr->fields[1].s == "M_Wood Dark", "a string with a space survives");
        check(q.name == "Crate # with spaces", "so does a name with a '#'");
        const auto& pos = q.find(1)->components[0].fields[0].f;
        check(pos.size() == 3 && pos[0] == 0.1f && pos[2] == 1e-7f && std::signbit(pos[1]), "floats come back bit for bit, -0 included");
    }

    AVER_INFO("validation");
    {
        fmt::OcPrefabData p = sample();
        p.nodes[1].uid = 1;
        check(!p.valid(), "a duplicate uid is refused");
        p = sample();
        std::swap(p.nodes[0], p.nodes[1]);
        check(!p.valid(), "a child before its parent is refused");
        p = sample();
        p.nodes[1].parent = 0;
        check(!p.valid(), "two roots are refused");
        p = sample();
        p.nextUid = 2;
        check(!p.valid(), "a nextUid that would reuse a uid is refused");
        fmt::OcPrefabData q;
        std::string why;
        check(!fmt::parseOcPrefab("NAME x\n", q, &why), "text with no header is refused");
        check(!fmt::parseOcPrefab("OCPREFAB 1\nNAME x\n", q, &why), "a file with no nodes is refused");
    }

    AVER_INFO("unknown records and a stale nextUid are tolerated");
    {
        fmt::OcPrefabData q;
        std::string why;
        const std::string text =
            "OCPREFAB 1\nFUTURE a b c\nNAME ~T\nNEXTUID 1\nNODE 5 parent 0 name ~Root\n  COMP CTags\n    F bits i32 3\n    F wat nope 1\n";
        check(fmt::parseOcPrefab(text, q, &why), "parses: " + why);
        check(q.nextUid == 6, "nextUid is pushed past the highest uid");
        check(q.nodes.size() == 1 && q.nodes[0].components.size() == 1 && q.nodes[0].components[0].fields.size() == 1,
              "the unknown record and the field of an unknown kind are skipped");
    }

    AVER_INFO("path helpers");
    {
        check(fmt::ocPrefabJoinPath("", "") == "" && fmt::ocPrefabJoinPath("3", "") == "3" &&
              fmt::ocPrefabJoinPath("", "5") == "5" && fmt::ocPrefabJoinPath("3", "5") == "3/5", "join");
        std::string h, r;
        fmt::ocPrefabSplitPath("3/5/2", h, r);
        check(h == "3" && r == "5/2", "split keeps the tail");
        fmt::ocPrefabSplitPath("7", h, r);
        check(h == "7" && r.empty(), "a one-segment path has no tail");
    }

    AVER_INFO("prefab instances ride in an .ocworld");
    {
        fmt::OcWorldData w;
        w.name = "T";
        fmt::OcWorldPlacement pl; pl.asset = "Meshes/a.ocmesh"; pl.x = 1; pl.y = 2; pl.z = 3;
        w.placements.push_back(pl);
        fmt::OcPrefabInstance in;
        in.prefab = "Prefabs/Crate.ocprefab"; in.name = "Crate 1";
        in.x = 100.5; in.y = -20; in.z = 0.25; in.yaw = 90; in.sx = in.sy = in.sz = 2;
        in.overrides.push_back(setOv("2", "CTags", fI32("bits", 7)));
        in.overrides.push_back(setOv("3/4", "CLocal", fVec3("position", 1, 2, 3)));
        w.prefabInstances.push_back(in);
        w.prefabInstances.push_back(fmt::OcPrefabInstance{"Prefabs/B.ocprefab", "", 0, 0, 0, 0, 0, 0, 1, 1, 1, {}});

        const std::string text = fmt::writeOcworld(w);
        fmt::OcWorldData r;
        std::string why;
        check(fmt::parseOcworld(text, r, &why), "it parses: " + why);
        check(r.placements.size() == 1 && r.placements[0].asset == "Meshes/a.ocmesh", "the placement is untouched");
        check(r.prefabInstances.size() == 2, "both instances come back");
        if (r.prefabInstances.size() == 2) {
            const auto& a = r.prefabInstances[0];
            check(a.prefab == "Prefabs/Crate.ocprefab" && a.name == "Crate 1", "asset and name");
            check(a.x == 100.5 && a.y == -20 && a.z == 0.25 && a.yaw == 90 && a.sx == 2, "transform");
            check(a.overrides.size() == 2 && a.overrides[0].path == "2" && a.overrides[0].value.i == 7 &&
                  a.overrides[1].path == "3/4", "overrides, in order");
            check(r.prefabInstances[1].overrides.empty() && r.prefabInstances[1].name.empty(), "an instance with none");
        }
        check(fmt::writeOcworld(r) == text, "write(parse(write(x))) is stable");
    }

    AVER_INFO("a level with no prefabs is unchanged");
    {
        const std::string old =
            "OCWORLD 1\nID 0x0000000000000001\nNAME Old\nBUILD 0\nALGO 3\n\nPLACE Meshes/a.ocmesh 1 2 3 0 0 0 1 M_X\n"
            "BEGIN\n  CHILD Meshes/b.ocmesh 0 0 5 0 0 0 1\nEND\n";
        fmt::OcWorldData w;
        std::string why;
        check(fmt::parseOcworld(old, w, &why), "parses: " + why);
        check(w.prefabInstances.empty() && w.placements.size() == 2, "no prefab instances, both placements");
        const std::string out = fmt::writeOcworld(w);
        check(out.find("PREFAB") == std::string::npos && out.find("POVERRIDE") == std::string::npos,
              "nothing prefab-shaped is written");
        fmt::OcWorldData again;
        check(fmt::parseOcworld(out, again, &why) && fmt::writeOcworld(again) == out, "and it round-trips");
    }

    AVER_INFO("an unterminated or stray prefab record does not break the level");
    {
        const std::string text =
            "OCWORLD 1\nNAME X\nPOVERRIDE set 2 CTags bits i32 1\nPREFABINST P.ocprefab pos 1 2 3\n"
            "  POVERRIDE set 2 CTags bits i32 4\nPLACE Meshes/a.ocmesh 0 0 0 0 0 0 1\n";
        fmt::OcWorldData w;
        std::string why;
        check(fmt::parseOcworld(text, w, &why), "parses: " + why);
        check(w.prefabInstances.size() == 1 && w.prefabInstances[0].overrides.size() == 1, "a stray override is dropped, the open one kept");
        check(w.placements.size() == 1, "the placement after it is still read");
    }

    if (g_failures) { AVER_ERROR("OcPrefabTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("OcPrefabTest: all passed");
    return 0;
}
