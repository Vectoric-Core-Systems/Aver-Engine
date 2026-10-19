#pragma once
// Vehicle viewport overlay (Show > Vehicles) for every built vehicle: suspension (attachment to hub to contact, green to red by
// compression), tyre forces (longitudinal red, lateral blue; 1 cm per 100 N times a scale), slip (a ring at each contact, hot when
// skidding), the chassis collision boxes and centre of mass, the velocity arrow, and a label (speed, gear, rpm, pedals, ABS / TCS).
//
// Also the spec PREVIEW the .ocvehicle editor tab asks for: the hubs, suspension travel, tyre cylinders, chassis boxes and centre of
// mass of the spec being edited, drawn over the selected entity. The editor writes vehicleSpecPreview(); the host draws it when
// vehicleDebugCollect is given the selection.
//
// Pure feed -> rhi::LineVertex, drawn like the motion overlay (see MotionDebugOverlay.hpp): once per frame while an option is on,
// vehicleDebugCollect(opts, ...), upload frame.lines as a line mesh and draw it with an identity matrix, then
// vehicleDebugDrawLabels inside the viewport window. The packaged game has no line overlay and none is added: its text overlay is
// the HUD (docs/VEHICLES.md).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_VEHICLE_SCENE
#  include "aver/vehicle/VehiclePhysicsSystem.hpp"
#endif

#include <string>
#include <vector>

namespace aver::editor {

struct VehicleDebugOptions {
    bool suspension = false;   // attachment -> hub -> contact, green to red by compression
    bool forces = false;       // longitudinal (red) and lateral (blue) tyre force arrows
    bool slip = false;         // a ring at each contact, tinted by slip, hot when skidding
    bool chassis = false;      // the collision boxes and the centre of mass
    bool velocity = false;     // the velocity arrow
    bool labels = false;       // speed, gear, rpm, pedals, lamps at each vehicle
    f32 forceScaleCmPer100N = 1.0f;
    f32 labelMaxDistanceCm = 6000.0f;
    bool any() const { return suspension || forces || slip || chassis || velocity || labels; }
};

struct VehicleDebugLabel {
    Vec3 world;
    std::string text;
    u32 rgba = 0xFFFFFFFFu;
};

struct VehicleDebugFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    std::vector<VehicleDebugLabel> labels;
    void clear() { lines.clear(); labels.clear(); }
};

// Green (0) through yellow to red (1) by how far the spring is compressed. Pure; a test pins the ends.
void vehicleCompressionColour(f32 t01, f32& r, f32& g, f32& b);
// A slip magnitude (slip ratio, or tan of the slip angle) against its working range: pale yellow when small, orange near the limit,
// red-white when the wheel is skidding.
void vehicleSlipColour(f32 slip01, bool skidding, f32& r, f32& g, f32& b);

#if AVER_MODULE_VEHICLE_SCENE
// The spec the .ocvehicle editor is showing (a copy, so closing the tab never leaves the overlay holding a dangling pointer).
struct VehicleSpecPreview {
    bool active = false;
    const void* owner = nullptr;   // the editor tab that wrote it, so only that tab clears it
    vehicle::VehicleSpec spec;
    int selectedWheel = -1;
};
VehicleSpecPreview& vehicleSpecPreview();

// Appends one built vehicle's live data. Exposed so a test can feed a hand-built view.
void vehicleDebugFeed(const vehicle::DebugView& d, const VehicleDebugOptions& opts, VehicleDebugFrame& out);

// Appends a spec's geometry at a pose: hubs, suspension travel, tyre cylinders, chassis boxes, centre of mass; the selected wheel highlighted.
void vehicleSpecPreviewFeed(const vehicle::VehicleSpec& spec, const Vec3& position, const Quat& rotation, int selectedWheel, VehicleDebugFrame& out);

// Clears `out`, then feeds every built vehicle of `sys`, and the editor's spec preview over `previewAt` when it is active.
// `previewAt` is the selected entity's pose (null = no preview).
struct PreviewPose { Vec3 position; Quat rotation; };
void vehicleDebugCollect(const VehicleDebugOptions& opts, vehicle::VehiclePhysicsSystem& sys, const PreviewPose* previewAt, VehicleDebugFrame& out);
#endif

#if AVER_WITH_IMGUI
// The labels, projected into the viewport (`viewProj` is the camera's), skipping those beyond `maxDistCm` of `eye`.
void vehicleDebugDrawLabels(const VehicleDebugFrame& frame, const Mat4& viewProj, f32 vpX, f32 vpY, f32 vpW, f32 vpH, const Vec3& eye, f32 maxDistCm);
#endif

} // namespace aver::editor
