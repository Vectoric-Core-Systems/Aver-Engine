#pragma once
// The world.* console commands (LARGE_WORLD 2.7): world.origin, world.rebase, world.margin, world.enable, world.policy, world.tp.
//
// The logic is here, inline and free of SandboxApp, so EditorOriginTest runs it against a real game::OriginParticipants. The
// console registration is WorldConsole.cpp (EditorConsole.hpp only declares registerWorldCommands, the way it does for phys.*).
//
// A command that MOVES the world (rebase, tp) is queued and runs at slot S0 of the next frame (applyWorldRequest, called from
// SandboxApp::tickOrigin): the console runs in the UI pass, after this frame's update has gathered its draws, and a shift there
// would render one frame in two frames of reference. Settings (margin, enable, policy) change nothing in the scene and apply at once.
#include "WorldOriginReadout.hpp"

#if AVER_MODULE_SCENE

#include <cerrno>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

struct WorldRequest {
    enum class Kind : u8 { None, Rebase, Teleport };
    Kind kind = Kind::None;
    world::OriginCm deltaCm{};     // Rebase: a whole number of regions on each axis
    world::Vec3d absCm{};          // Teleport: the camera's destination, absolute centimetres
};

struct WorldConsoleHost {
    game::OriginParticipants* origin = nullptr;
    Vec3* camPos = nullptr;                    // the camera, origin-relative (a teleport sets it)
    std::function<void()> onTeleport;          // reset anything that tracks the camera's velocity
    WorldRequest pending;
};

// The host the registered commands act on; null until the editor sets it.
inline WorldConsoleHost*& worldConsoleHostSlot() { static WorldConsoleHost* h = nullptr; return h; }

namespace detail {

inline bool parseWholeNumber(std::string_view tok, i64& out) {
    const std::string s(tok);
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || end != s.c_str() + s.size() || errno == ERANGE) return false;
    out = static_cast<i64>(v);
    return true;
}
inline bool parseReal(std::string_view tok, f64& out) {
    const std::string s(tok);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v)) return false;
    out = v;
    return true;
}
inline bool worldFail(std::vector<std::string>& lines, std::string msg) { lines.push_back(std::move(msg)); return false; }

} // namespace detail

// Runs one command. False = it failed (or refused) and `lines` holds why; true = done or queued. `name` is matched case-insensitively.
inline bool runWorldCommand(WorldConsoleHost& h, std::string_view name, const std::vector<std::string>& args,
                            std::vector<std::string>& lines) {
    using detail::worldFail;
    auto eq = [&](std::string_view a) {
        if (a.size() != name.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            char x = a[i], y = name[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    };
    if (!h.origin) return worldFail(lines, "the floating origin is not available in this build");
    game::OriginParticipants& op = *h.origin;
    const Vec3 cam = h.camPos ? *h.camPos : Vec3{};

    if (eq("world.origin")) {
        for (std::string& l : worldOriginReadoutLines(makeWorldOriginReadout(op, cam))) lines.push_back(std::move(l));
        const WorldRequest& p = h.pending;
        if (p.kind != WorldRequest::Kind::None) lines.push_back("a world move is queued for the next frame");
        return true;
    }

    if (eq("world.rebase")) {
        i64 dx = 0, dy = 0;
        if (args.size() != 2 || !detail::parseWholeNumber(args[0], dx) || !detail::parseWholeNumber(args[1], dy))
            return worldFail(lines, "usage: world.rebase <dxRegions> <dyRegions>   (whole numbers; one region is 16.384 km)");
        if (dx == 0 && dy == 0) return worldFail(lines, "world.rebase 0 0 moves nothing");
        constexpr i64 kLimit = 1'000'000;   // regions: far past anything a planet holds, and clear of i64 overflow in cm
        if (dx > kLimit || dx < -kLimit || dy > kLimit || dy < -kLimit) return worldFail(lines, "that is more than a million regions");
        if (!op.service().enabled()) return worldFail(lines, "the floating origin is off (world.enable 1)");
        if (op.service().settings().policy == world::OriginPolicy::Fixed)
            return worldFail(lines, "the policy is fixed: the origin never moves (world.policy manual)");
        h.pending = {};
        h.pending.kind = WorldRequest::Kind::Rebase;
        h.pending.deltaCm = world::OriginCm{dx * world::kOriginStepCm, dy * world::kOriginStepCm};
        lines.push_back("queued: the origin moves by (" + std::to_string(dx) + ", " + std::to_string(dy) + ") regions next frame");
        return true;
    }

    if (eq("world.margin")) {
        if (args.empty()) {
            lines.push_back("margin " + std::to_string(op.service().settings().marginCm) + " cm (0 to " +
                            std::to_string(world::kRebaseMarginMaxCm) + ")");
            return true;
        }
        i64 v = 0;
        if (args.size() != 1 || !detail::parseWholeNumber(args[0], v)) return worldFail(lines, "usage: world.margin <cm>");
        if (v < 0 || v > world::kRebaseMarginMaxCm)
            return worldFail(lines, "the margin is 0 to " + std::to_string(world::kRebaseMarginMaxCm) + " cm: past that, far objects lose sub-millimetre precision");
        op.configure(-1, static_cast<int>(v), op.service().enabled());
        lines.push_back("margin " + std::to_string(op.service().settings().marginCm) + " cm: the shift happens " +
                        std::to_string(op.service().settings().marginCm + world::kRegionHalfCm) + " cm from the origin");
        return true;
    }

    if (eq("world.enable")) {
        if (args.empty()) { lines.push_back(op.service().enabled() ? "enabled" : "disabled"); return true; }
        if (args.size() != 1 || (args[0] != "0" && args[0] != "1")) return worldFail(lines, "usage: world.enable 0|1");
        op.configure(args[0] == "1" ? 1 : 0, -1, args[0] == "1");
        lines.push_back(op.service().enabled() ? "the floating origin is on" : "the floating origin is off: the origin stays where it is");
        return true;
    }

    if (eq("world.policy")) {
        world::WorldOrigin::Settings s = op.service().settings();
        if (args.empty()) { lines.push_back(originPolicyName(s.policy)); return true; }
        if (args.size() != 1) return worldFail(lines, "usage: world.policy follow|fixed|manual");
        std::string a = args[0];
        for (char& c : a) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (a == "follow") s.policy = world::OriginPolicy::FollowCamera;
        else if (a == "fixed") s.policy = world::OriginPolicy::Fixed;
        else if (a == "manual") s.policy = world::OriginPolicy::Manual;
        else return worldFail(lines, "usage: world.policy follow|fixed|manual");
        op.service().configure(s);
        lines.push_back(std::string("policy ") + originPolicyName(op.service().settings().policy));
        return true;
    }

    if (eq("world.tp")) {
        f64 km[3] = {0, 0, 0};
        if (args.size() != 3 || !detail::parseReal(args[0], km[0]) || !detail::parseReal(args[1], km[1]) || !detail::parseReal(args[2], km[2]))
            return worldFail(lines, "usage: world.tp <xKm> <yKm> <zKm>   (absolute position of the camera)");
        for (f64 v : km) if (v > 1.0e7 || v < -1.0e7) return worldFail(lines, "that is past ten million km");
        if (!op.service().enabled()) return worldFail(lines, "the floating origin is off (world.enable 1): the camera would be too far from the origin to render");
        if (op.service().settings().policy == world::OriginPolicy::Fixed)
            return worldFail(lines, "the policy is fixed: the origin never moves (world.policy manual)");
        h.pending = {};
        h.pending.kind = WorldRequest::Kind::Teleport;
        h.pending.absCm = world::Vec3d{km[0] * 100000.0, km[1] * 100000.0, km[2] * 100000.0};
        lines.push_back("queued: the camera goes to (" + std::to_string(km[0]) + ", " + std::to_string(km[1]) + ", " +
                        std::to_string(km[2]) + ") km next frame, with the origin moved to it");
        return true;
    }

    return worldFail(lines, "unknown world command");
}

// Slot S0, BEFORE the origin's own update: runs a queued rebase or teleport. Returns a one-line report ("" when nothing was queued);
// `ok` is false when the move was refused (a gesture, a level load, a physics step...), in which case it is dropped, not retried.
inline std::string applyWorldRequest(WorldConsoleHost& h, bool& ok) {
    ok = true;
    const WorldRequest req = h.pending;
    if (req.kind == WorldRequest::Kind::None) return {};
    h.pending = {};
    if (!h.origin) { ok = false; return "the floating origin is not available"; }
    game::OriginParticipants& op = *h.origin;
    world::WorldOrigin& svc = op.service();

    if (req.kind == WorldRequest::Kind::Rebase) {
        if (!svc.shiftBy(req.deltaCm, /*urgent=*/false)) {
            ok = false;
            return std::string("world.rebase refused: ") + (svc.stats().lastDeferReason ? svc.stats().lastDeferReason : "the service would not shift");
        }
        return "world.rebase: origin now (" + std::to_string(svc.origin().x / world::kOriginStepCm) + ", " +
               std::to_string(svc.origin().y / world::kOriginStepCm) + ") regions";
    }

    // Teleport: the origin goes to the nearest region corner to the destination, so the camera lands inside its window, then the
    // camera is placed exactly (f64 absolute minus the origin, one rounding to f32).
    const world::OriginCm cur = svc.origin();
    const auto snap = [](f64 v) { return static_cast<i64>(std::llround(v / static_cast<f64>(world::kOriginStepCm))) * world::kOriginStepCm; };
    const world::OriginCm want{snap(req.absCm.x), snap(req.absCm.y)};
    const world::OriginCm delta{want.x - cur.x, want.y - cur.y};
    if (delta.x != 0 || delta.y != 0) {
        // Urgent: a teleport is a decision, and an open gesture is ended rather than waited for.
        if (!svc.shiftBy(delta, /*urgent=*/true)) {
            ok = false;
            return std::string("world.tp refused: ") + (svc.stats().lastDeferReason ? svc.stats().lastDeferReason : "the service would not shift");
        }
    }
    const world::OriginCm now = svc.origin();
    if (h.camPos)
        *h.camPos = Vec3{static_cast<f32>(req.absCm.x - static_cast<f64>(now.x)), static_cast<f32>(req.absCm.y - static_cast<f64>(now.y)),
                         static_cast<f32>(req.absCm.z)};
    if (h.onTeleport) h.onTeleport();
    return "world.tp: origin (" + std::to_string(now.x / world::kOriginStepCm) + ", " + std::to_string(now.y / world::kOriginStepCm) +
           ") regions, camera placed";
}

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
