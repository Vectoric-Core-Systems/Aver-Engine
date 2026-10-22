// The pure authoring helpers of Aver.Hair besides the generator: glTF curves into strands (HairCurves.hpp) and the collision proxy shapes and default
// humanoid set (HairProxy.hpp). No physics, no scene, no device. Exit code = failure count.
//
// NOTHING IN HERE HAS BEEN RUN: thresholds are starting values for the first pass.
#include "aver/hair/HairCurves.hpp"
#include "aver/hair/HairProxy.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static void addPoint(fmt::ImportedHairCurves& c, float x, float y, float z, float widthCm = -1.0f) {
    c.points.insert(c.points.end(), {x, y, z});
    if (widthCm >= 0.0f) c.pointWidthCm.push_back(widthCm);
}

static void closeStrand(fmt::ImportedHairCurves& c, u32 material) {
    if (c.strandStart.empty()) c.strandStart.push_back(0);
    c.strandStart.push_back(static_cast<u32>(c.points.size() / 3));
    c.material.push_back(material);
}

int main() {
    AVER_INFO("=== curves to strands ===");
    {
        fmt::ImportedHairCurves in;
        // 0: two points -> dropped (too short)
        addPoint(in, 0, 0, 0); addPoint(in, 1, 0, 0);
        closeStrand(in, 0);
        // 1: five points with a duplicate in the middle -> four usable points
        for (float x : {0.0f, 1.0f, 1.0f, 2.0f, 3.0f}) addPoint(in, x, 5, 0);
        closeStrand(in, 4);
        // 2: three hundred points along +Z -> resampled to 255
        for (int i = 0; i < 300; ++i) addPoint(in, 10, 10, static_cast<float>(i) * 0.1f);
        closeStrand(in, 300);
        // 3: a plain four-point strand
        for (int i = 0; i < 4; ++i) addPoint(in, 20, static_cast<float>(i), 0);
        closeStrand(in, 1);

        fmt::OcHairStrands out;
        hair::CurveImportReport rep;
        std::string why;
        check(hair::curvesToStrands(in, out, {}, &rep, &why), "curves convert (" + why + ")");
        check(rep.strandsIn == 4 && rep.strandsOut == 3 && rep.droppedShort == 1, "one strand of two points is dropped");
        check(rep.droppedPoints == 1, "the coincident neighbour is removed");
        check(rep.resampled == 1 && rep.materialsClamped == 1, "the long strand is resampled and the material index 300 is clamped");
        check(out.strandCount() == 3 && out.start[0] == 0 && out.start[1] == 4 && out.start[2] == 4 + 255 && out.start[3] == 4 + 255 + 4, "strand tables: 4, 255 and 4 points");
        check(out.material.size() == 3 && out.material[0] == 4 && out.material[1] == 255 && out.material[2] == 1, "materials carried, 300 clamped to 255");
        check(out.width.empty(), "no widths in the file, no width table");
        // The resampled strand keeps its ends and is evenly spaced.
        const size_t base = 4 * 3;
        const float z0 = out.points[base + 2], zEnd = out.points[base + 254 * 3 + 2];
        check(std::fabs(z0) < 1e-4f && std::fabs(zEnd - 29.9f) < 1e-2f, "the resampled strand keeps both ends");
        const float step = out.points[base + 3 + 2] - out.points[base + 2];
        check(std::fabs(step - 29.9f / 254.0f) < 1e-3f, "and is evenly spaced by arc length");

        // Widths: the mean of the points' widths per strand.
        fmt::ImportedHairCurves w;
        for (int i = 0; i < 4; ++i) addPoint(w, static_cast<float>(i), 0, 0, 0.1f * static_cast<float>(i + 1));
        closeStrand(w, 0);
        fmt::OcHairStrands wo;
        check(hair::curvesToStrands(w, wo, {}, nullptr, &why) && wo.width.size() == 1 && std::fabs(wo.width[0] - 0.25f) < 1e-5f, "a strand's width is the mean of its points'");

        fmt::ImportedHairCurves none;
        check(!hair::curvesToStrands(none, out, {}, nullptr, &why), "no curves at all is refused");
        fmt::ImportedHairCurves shortOnly;
        addPoint(shortOnly, 0, 0, 0); addPoint(shortOnly, 1, 0, 0);
        closeStrand(shortOnly, 0);
        check(!hair::curvesToStrands(shortOnly, out, {}, nullptr, &why), "only too-short strands is refused");
        fmt::ImportedHairCurves broken = in;
        broken.material.pop_back();
        check(!hair::curvesToStrands(broken, out, {}, nullptr, &why), "inconsistent tables are refused");
    }

    AVER_INFO("=== proxy hulls ===");
    {
        const float sphere[3] = {10.0f, 0, 0};
        const std::vector<f32> sp = hair::proxyHullPoints(fmt::OcHairProxyShape::Sphere, sphere);
        check(sp.size() == 32 * 3, "a sphere hull has 32 points");
        bool onSphere = true;
        float minZ = 1e9f, maxZ = -1e9f;
        for (size_t i = 0; i < sp.size(); i += 3) {
            if (std::fabs(Vec3(sp[i], sp[i + 1], sp[i + 2]).size() - 10.0f) > 1e-3f) onSphere = false;
            minZ = std::min(minZ, sp[i + 2]);
            maxZ = std::max(maxZ, sp[i + 2]);
        }
        check(onSphere, "every point is on the sphere");
        check(maxZ > 9.0f && minZ < -9.0f, "and the spiral reaches both poles");

        const float capsule[3] = {5.0f, 3.0f, 0};
        const std::vector<f32> cp = hair::proxyHullPoints(fmt::OcHairProxyShape::Capsule, capsule);
        float cMin = 1e9f, cMax = -1e9f;
        for (size_t i = 2; i < cp.size(); i += 3) { cMin = std::min(cMin, cp[i]); cMax = std::max(cMax, cp[i]); }
        check(cp.size() == 32 * 3 && cMax > 3.0f + 4.0f && cMin < -(3.0f + 4.0f), "a capsule hull reaches half height + radius at each end");

        const float box[3] = {1.0f, 2.0f, 3.0f};
        const std::vector<f32> bp = hair::proxyHullPoints(fmt::OcHairProxyShape::Box, box);
        check(bp.size() == 24, "a box hull has 8 points");
        bool corners = true;
        for (size_t i = 0; i < bp.size(); i += 3)
            if (std::fabs(bp[i]) != 1.0f || std::fabs(bp[i + 1]) != 2.0f || std::fabs(bp[i + 2]) != 3.0f) corners = false;
        check(corners, "at the corners");

        const float zero[3] = {0, 0, 0};
        check(hair::proxyHullPoints(fmt::OcHairProxyShape::Sphere, zero).empty() && hair::proxyHullPoints(fmt::OcHairProxyShape::Capsule, sphere).empty() &&
                  hair::proxyHullPoints(fmt::OcHairProxyShape::Box, sphere).empty(),
              "a size that is not positive gives no hull");
    }

    AVER_INFO("=== default humanoid proxies ===");
    {
        check(hair::jointMatches("Head", "head") && hair::jointMatches("mixamorig:Head", "head") && !hair::jointMatches("HeadTop_End", "head") &&
                  !hair::jointMatches("head_end", "head"),
              "'head' matches the head bone and not its end helper");
        check(hair::jointMatches("Spine_02", "spine") && hair::jointMatches("LeftClavicle", "clavicle") && hair::jointMatches("RightShoulder", "shoulder") &&
                  hair::jointMatches("neck_01", "neck") && !hair::jointMatches("Hips", "neck"),
              "spine, clavicle, shoulder and neck match anywhere in the name");

        const std::vector<std::string> rig = {"Hips", "Spine_01", "Spine_02", "Spine_03", "Neck", "Head", "HeadTop_End", "LeftShoulder", "RightShoulder"};
        std::vector<std::string> missing;
        const std::vector<fmt::OcHairProxy> p = hair::defaultHumanoidProxies(rig, &missing);
        check(missing.empty(), "a full rig misses nothing");
        check(p.size() == 5, "head, neck, two shoulders and the spine: " + std::to_string(p.size()));
        bool headOk = false, spineOk = false;
        int shoulders = 0;
        for (const fmt::OcHairProxy& x : p) {
            if (x.name == "head") headOk = x.bone == "Head" && x.shape == fmt::OcHairProxyShape::Sphere;
            if (x.name == "spine") spineOk = x.bone == "Spine_03" && x.shape == fmt::OcHairProxyShape::Box;
            if (x.name.rfind("shoulder", 0) == 0) ++shoulders;
        }
        check(headOk && spineOk && shoulders == 2, "the head sphere is on Head, the spine box on the highest spine bone, one capsule per shoulder");
        for (const fmt::OcHairProxy& x : p) {
            std::string why;
            fmt::OcHairData d;
            d.proxies = {x};
            d.generate.present = true;
            (void)why;
            check(!hair::proxyHullPoints(x.shape, x.sizeCm).empty(), "proxy '" + x.name + "' has a usable hull");
        }

        missing.clear();
        const std::vector<fmt::OcHairProxy> bare = hair::defaultHumanoidProxies({"Root", "Pelvis"}, &missing);
        check(bare.empty() && missing.size() == 4, "a rig with none of the bones gets no proxies and four misses");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
