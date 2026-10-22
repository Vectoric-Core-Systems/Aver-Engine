// HairProxy.cpp -- hull points for proxy shapes and the default humanoid proxy set. See HairProxy.hpp.
#include "aver/hair/HairProxy.hpp"

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aver::hair {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

constexpr int kSpherePoints = 32;

// A Fibonacci spiral: evenly spread points on a unit sphere, +Z first.
void unitSphere(Vec3* out) {
    const f32 golden = kPi * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < kSpherePoints; ++i) {
        const f32 z = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kSpherePoints);
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const f32 a = golden * static_cast<f32>(i);
        out[i] = Vec3(r * std::cos(a), r * std::sin(a), z);
    }
}

} // namespace

bool jointMatches(std::string_view joint, std::string_view kind) {
    const std::string j = lower(joint);
    const std::string k = lower(kind);
    if (k == "head") {
        // The head bone itself, not the helper bones that hang off it (HeadTop_End, head_end, headfront).
        return contains(j, "head") && !contains(j, "top") && !contains(j, "end") && !contains(j, "front") && !contains(j, "nub");
    }
    if (k == "neck") return contains(j, "neck");
    if (k == "spine") return contains(j, "spine");
    if (k == "shoulder") return contains(j, "shoulder");
    if (k == "clavicle") return contains(j, "clavicle");
    return contains(j, lower(kind).c_str());
}

std::vector<f32> proxyHullPoints(fmt::OcHairProxyShape shape, const f32 sizeCm[3]) {
    std::vector<f32> out;
    switch (shape) {
    case fmt::OcHairProxyShape::Sphere:
    case fmt::OcHairProxyShape::Capsule: {
        const f32 radius = sizeCm[0];
        const f32 half = shape == fmt::OcHairProxyShape::Capsule ? sizeCm[1] : 0.0f;
        if (!(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(half) || (shape == fmt::OcHairProxyShape::Capsule && !(half > 0.0f))) return out;
        Vec3 unit[kSpherePoints];
        unitSphere(unit);
        out.reserve(kSpherePoints * 3);
        for (const Vec3& u : unit) {
            const f32 lift = shape == fmt::OcHairProxyShape::Capsule ? (u.z >= 0.0f ? half : -half) : 0.0f;
            out.push_back(u.x * radius);
            out.push_back(u.y * radius);
            out.push_back(u.z * radius + lift);
        }
        break;
    }
    case fmt::OcHairProxyShape::Box: {
        if (!(sizeCm[0] > 0.0f) || !(sizeCm[1] > 0.0f) || !(sizeCm[2] > 0.0f)) return out;
        out.reserve(24);
        for (int c = 0; c < 8; ++c) {
            out.push_back((c & 1) ? sizeCm[0] : -sizeCm[0]);
            out.push_back((c & 2) ? sizeCm[1] : -sizeCm[1]);
            out.push_back((c & 4) ? sizeCm[2] : -sizeCm[2]);
        }
        break;
    }
    }
    return out;
}

std::vector<fmt::OcHairProxy> defaultHumanoidProxies(const std::vector<std::string>& jointNames, std::vector<std::string>* missing) {
    std::vector<fmt::OcHairProxy> out;
    auto matches = [&](const char* kind) {
        std::vector<const std::string*> found;
        for (const std::string& j : jointNames)
            if (jointMatches(j, kind)) found.push_back(&j);
        return found;
    };
    auto make = [&](const std::string& name, const std::string& bone, fmt::OcHairProxyShape shape, f32 a, f32 b, f32 c, f32 oz) {
        fmt::OcHairProxy p;
        p.name = name;
        p.bone = bone;
        p.shape = shape;
        p.sizeCm[0] = a; p.sizeCm[1] = b; p.sizeCm[2] = c;
        p.offsetCm[2] = oz;
        out.push_back(std::move(p));
    };
    // Sizes for a 175 cm character, offsets along the bone towards where the mass is (UNVERIFIED: tuned in the headed pass against the HairLab rig).
    if (const auto head = matches("head"); !head.empty()) make("head", *head.front(), fmt::OcHairProxyShape::Sphere, 10.5f, 0, 0, 8.0f);
    else if (missing) missing->push_back("head");
    if (const auto neck = matches("neck"); !neck.empty()) make("neck", *neck.front(), fmt::OcHairProxyShape::Capsule, 5.0f, 3.0f, 0, 3.0f);
    else if (missing) missing->push_back("neck");
    // Every clavicle; a rig that names shoulders instead gets those.
    auto shoulders = matches("clavicle");
    if (shoulders.empty()) shoulders = matches("shoulder");
    if (shoulders.empty() && missing) missing->push_back("clavicle or shoulder");
    for (usize i = 0; i < shoulders.size(); ++i)
        make("shoulder" + std::to_string(i), *shoulders[i], fmt::OcHairProxyShape::Capsule, 5.5f, 6.0f, 0, 0.0f);
    // The highest spine bone (spine_03 over spine_01): the list is usually parents first.
    if (const auto spine = matches("spine"); !spine.empty()) make("spine", *spine.back(), fmt::OcHairProxyShape::Box, 14.0f, 8.0f, 12.0f, 8.0f);
    else if (missing) missing->push_back("spine");
    return out;
}

} // namespace aver::hair
