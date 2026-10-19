// Vehicle viewport overlay. See the header for the wiring.

#include "VehicleDebugOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if AVER_WITH_IMGUI
#  include "AiDebugOverlay.hpp"   // aiDebugProject
#  include "imgui.h"
#endif

#if AVER_MODULE_VEHICLE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/vehicle/OcVehicle.hpp"
#  include "aver/vehicle/VehicleBuild.hpp"
#endif

namespace aver::editor {

void vehicleCompressionColour(f32 t01, f32& r, f32& g, f32& b) {
    const f32 t = std::min(std::max(t01, 0.0f), 1.0f);
    // green (0) -> yellow (0.5) -> red (1)
    r = std::min(1.0f, t * 2.0f);
    g = std::min(1.0f, (1.0f - t) * 2.0f);
    b = 0.1f;
}

void vehicleSlipColour(f32 slip01, bool skidding, f32& r, f32& g, f32& b) {
    if (skidding) { r = 1.0f; g = 0.35f; b = 0.25f; return; }
    const f32 t = std::min(std::max(slip01, 0.0f), 1.0f);
    r = 1.0f;
    g = 0.95f - 0.45f * t;
    b = 0.6f - 0.5f * t;
}

#if AVER_MODULE_VEHICLE_SCENE

namespace {

void line(VehicleDebugFrame& f, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    f.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    f.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

void mark(VehicleDebugFrame& f, const Vec3& c, f32 size, f32 r, f32 g, f32 b) {
    line(f, c - Vec3{size, 0, 0}, c + Vec3{size, 0, 0}, r, g, b);
    line(f, c - Vec3{0, size, 0}, c + Vec3{0, size, 0}, r, g, b);
    line(f, c - Vec3{0, 0, size}, c + Vec3{0, 0, size}, r, g, b);
}

// A circle of `radius` around `c` in the plane spanned by the unit vectors u and v.
void circle(VehicleDebugFrame& f, const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, f32 r, f32 g, f32 b, int segments = 20) {
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= segments; ++i) {
        const f32 a = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const Vec3 p = c + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        line(f, prev, p, r, g, b);
        prev = p;
    }
}

// A line with two short barbs at the tip. `up` picks the plane the barbs open in.
void arrow(VehicleDebugFrame& f, const Vec3& from, const Vec3& vec, const Vec3& up, f32 r, f32 g, f32 b) {
    const f32 len = vec.size();
    if (len < 0.5f) return;
    const Vec3 tip = from + vec;
    line(f, from, tip, r, g, b);
    const Vec3 dir = vec * (1.0f / len);
    Vec3 side = cross(dir, up);
    if (side.sizeSquared() < 1.0e-6f) side = cross(dir, Vec3{1, 0, 0});
    side = side.getSafeNormal();
    const f32 barb = std::min(len * 0.25f, 12.0f);
    line(f, tip, tip - dir * barb + side * (barb * 0.5f), r, g, b);
    line(f, tip, tip - dir * barb - side * (barb * 0.5f), r, g, b);
}

// A box's twelve edges: centre and half extents in the frame (pos, rot).
void box(VehicleDebugFrame& f, const Vec3& pos, const Quat& rot, const Vec3& centre, const Vec3& half, f32 r, f32 g, f32 b) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = pos + rot.rotate(centre + Vec3{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z});
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) line(f, c[e[0]], c[e[1]], r, g, b);
}

// The collision boxes of a spec in a frame: the BOX records when there are any, else the body box sitting `clearance` above the ground.
void chassisBoxes(VehicleDebugFrame& f, const vehicle::VehicleSpec& s, const Vec3& pos, const Quat& rot, f32 r, f32 g, f32 b) {
    if (!s.boxes.empty()) {
        for (const vehicle::BoxDef& x : s.boxes) box(f, pos, rot, Vec3{x.cx, x.cy, x.cz}, Vec3{x.hx, x.hy, x.hz}, r, g, b);
        return;
    }
    box(f, pos, rot, Vec3{0.0f, 0.0f, s.body.clearance + s.body.half.z}, s.body.half, r, g, b);
}

std::string labelText(const vehicle::Telemetry& t) {
    const std::string gear = t.gear < 0 ? "R" : (t.gear == 0 ? "N" : std::to_string(t.gear));
    char b[160];
    std::snprintf(b, sizeof b, "%.0f km/h  gear %s  %.0f rpm\nthrottle %.0f%%  brake %.0f%%%s%s%s", static_cast<double>(std::fabs(t.speedCmS)) * 0.036,
                  gear.c_str(), static_cast<double>(t.rpm), static_cast<double>(t.throttle) * 100.0, static_cast<double>(t.brake) * 100.0,
                  t.absActive ? "  ABS" : "", t.tcsActive ? "  TCS" : "", t.flipped ? "  FLIPPED" : "");
    return b;
}

} // namespace

VehicleSpecPreview& vehicleSpecPreview() {
    static VehicleSpecPreview p;
    return p;
}

void vehicleDebugFeed(const vehicle::DebugView& d, const VehicleDebugOptions& opts, VehicleDebugFrame& out) {
    if (!d.valid) return;
    const Vec3 up = d.rotation.rotate(Vec3{0, 0, 1});
    for (u32 i = 0; i < d.wheelCount && i < 16; ++i) {
        const vehicle::DebugView::Wheel& w = d.wheels[i];
        const vehicle::WheelClass* wc = d.spec && i < d.spec->wheels.size() ? vehicle::findClass(*d.spec, d.spec->wheels[i].wheelClass) : nullptr;
        const f32 range = std::max(1.0f, wc ? wc->suspMax - wc->suspMin : w.suspMaxCm);   // the spring's working travel, cm
        const Vec3 axle = w.axle.getSafeNormal();
        Vec3 fwd = cross(axle, up).getSafeNormal();
        if (fwd.sizeSquared() < 0.5f) fwd = d.rotation.rotate(Vec3{1, 0, 0});

        if (opts.suspension) {
            f32 r, g, b;
            vehicleCompressionColour(w.compressionCm / range, r, g, b);
            line(out, w.attach, w.hub, r, g, b);
            if (w.inContact) line(out, w.hub, w.contactPoint, r, g, b);
            mark(out, w.hub, 4.0f, r, g, b);
        }
        if (opts.slip && w.inContact) {
            f32 r, g, b;
            // Longitudinal slip is a ratio (about 0.2 at the limit of grip), lateral is degrees (about 20); take the larger fraction.
            const f32 s = std::max(std::fabs(w.slipLong) / 0.3f, std::fabs(w.slipLatDeg) / 20.0f);
            vehicleSlipColour(s, w.skidding, r, g, b);
            const Vec3 n = w.normal.sizeSquared() > 0.5f ? w.normal.getSafeNormal() : up;
            Vec3 u = cross(n, fwd);
            if (u.sizeSquared() < 1.0e-6f) u = cross(n, Vec3{1, 0, 0});
            u = u.getSafeNormal();
            const Vec3 v = cross(n, u).getSafeNormal();
            circle(out, w.contactPoint + n * 1.0f, u, v, 10.0f + 14.0f * std::min(s, 1.5f), r, g, b);
        }
        if (opts.forces) {
            const Vec3 origin = w.inContact ? w.contactPoint : w.hub;
            const f32 k = opts.forceScaleCmPer100N * 0.01f;
            arrow(out, origin, fwd * (w.forceLongN * k), up, 1.0f, 0.25f, 0.2f);
            arrow(out, origin, axle * (w.forceLatN * k), up, 0.25f, 0.5f, 1.0f);
        }
    }
    if (opts.chassis && d.spec) {
        chassisBoxes(out, *d.spec, d.position, d.rotation, 0.3f, 0.9f, 0.9f);
        mark(out, d.comWorld, 10.0f, 1.0f, 0.2f, 0.9f);
    }
    if (opts.velocity && d.velocity.size() > 5.0f) arrow(out, d.comWorld, d.velocity * 0.5f, up, 1.0f, 1.0f, 0.3f);
    if (opts.labels) {
        VehicleDebugLabel l;
        const f32 top = d.spec ? d.spec->body.clearance + d.spec->body.half.z * 2.0f : 120.0f;
        l.world = d.position + up * (top + 40.0f);
        l.text = labelText(d.telemetry);
        out.labels.push_back(l);
    }
}

void vehicleSpecPreviewFeed(const vehicle::VehicleSpec& s, const Vec3& pos, const Quat& rot, int selectedWheel, VehicleDebugFrame& out) {
    chassisBoxes(out, s, pos, rot, 0.3f, 0.8f, 0.9f);
    mark(out, pos + rot.rotate(s.body.com), 10.0f, 1.0f, 0.2f, 0.9f);
    const Vec3 fwdAxis = rot.rotate(Vec3{1, 0, 0}), upAxis = rot.rotate(Vec3{0, 0, 1}), sideAxis = rot.rotate(Vec3{0, 1, 0});
    line(out, pos, pos + fwdAxis * 80.0f, 0.9f, 0.3f, 0.3f);   // forward, so a mirrored spec is obvious
    for (usize i = 0; i < s.wheels.size(); ++i) {
        const vehicle::WheelDef& w = s.wheels[i];
        const vehicle::WheelClass* c = vehicle::findClass(s, w.wheelClass);
        const f32 radius = c ? c->radius : 33.0f, width = c ? c->width : 22.0f;
        const f32 suspMin = c ? c->suspMin : 5.0f, suspMax = c ? c->suspMax : 30.0f;
        const f32 sag = vehicle::springSagCm(c ? c->suspHz : 2.0f);
        const bool selected = static_cast<int>(i) == selectedWheel;
        const f32 r = selected ? 1.0f : (w.driven ? 1.0f : 0.8f), g = selected ? 1.0f : (w.driven ? 0.6f : 0.8f), b = selected ? 0.2f : (w.driven ? 0.2f : 0.8f);
        const Vec3 hub = pos + rot.rotate(w.hub);
        // The suspension: from the attachment, down through the shortest and the longest spring.
        const f32 attachZ = w.hub.z + suspMax - sag;
        const Vec3 top = pos + rot.rotate(Vec3{w.hub.x, w.hub.y, attachZ});
        const Vec3 shortest = pos + rot.rotate(Vec3{w.hub.x, w.hub.y, attachZ - suspMin});
        const Vec3 longest = pos + rot.rotate(Vec3{w.hub.x, w.hub.y, attachZ - suspMax});
        line(out, top, longest, 0.5f, 0.9f, 0.4f);
        mark(out, shortest, 2.5f, 0.5f, 0.9f, 0.4f);
        mark(out, longest, 2.5f, 0.9f, 0.5f, 0.4f);
        // The tyre: two rings along the axle.
        circle(out, hub - sideAxis * (width * 0.5f), fwdAxis, upAxis, radius, r, g, b, 24);
        circle(out, hub + sideAxis * (width * 0.5f), fwdAxis, upAxis, radius, r, g, b, 24);
        line(out, hub - sideAxis * (width * 0.5f), hub + sideAxis * (width * 0.5f), r, g, b);
        if (std::fabs(w.steerDeg) > 0.01f) line(out, hub, hub + fwdAxis * (radius * 1.4f), 0.4f, 0.8f, 1.0f);
    }
}

void vehicleDebugCollect(const VehicleDebugOptions& opts, vehicle::VehiclePhysicsSystem& sys, const PreviewPose* previewAt, VehicleDebugFrame& out) {
    out.clear();
    if (opts.any()) {
        for (const scene::Entity e : sys.entities()) {
            if (!sys.isVehicle(e)) continue;   // the list holds the wheel children too
            vehicleDebugFeed(sys.debug(e), opts, out);
        }
    }
    const VehicleSpecPreview& p = vehicleSpecPreview();
    if (p.active && previewAt) vehicleSpecPreviewFeed(p.spec, previewAt->position, previewAt->rotation, p.selectedWheel, out);
}

#endif  // AVER_MODULE_VEHICLE_SCENE

#if AVER_WITH_IMGUI
void vehicleDebugDrawLabels(const VehicleDebugFrame& frame, const Mat4& viewProj, f32 vpX, f32 vpY, f32 vpW, f32 vpH, const Vec3& eye, f32 maxDistCm) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const VehicleDebugLabel& l : frame.labels) {
        if (dist(l.world, eye) > maxDistCm) continue;
        f32 x, y;
        if (!aiDebugProject(viewProj, l.world, vpX, vpY, vpW, vpH, x, y)) continue;
        const ImU32 col = IM_COL32((l.rgba >> 16) & 0xFF, (l.rgba >> 8) & 0xFF, l.rgba & 0xFF, (l.rgba >> 24) & 0xFF);
        dl->AddText(ImVec2(x, y), col, l.text.c_str());
    }
}
#endif

} // namespace aver::editor
