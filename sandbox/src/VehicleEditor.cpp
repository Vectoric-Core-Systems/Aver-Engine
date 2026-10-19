// The .ocvehicle editor tab. See the header for the SandboxApp hook and VehicleEditorModel.hpp for the rules the widgets draw.
#include "VehicleEditor.hpp"

#include "VehicleDebugOverlay.hpp"
#include "VehicleEditorModel.hpp"

#include "EditorKeybinds.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {

namespace ve = vehedit;
using vehicle::VehicleSpec;

VehicleEditor::VehicleEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

VehicleEditor::~VehicleEditor() {
#if AVER_MODULE_VEHICLE_SCENE
    VehicleSpecPreview& p = vehicleSpecPreview();
    if (p.owner == this) { p.active = false; p.owner = nullptr; }
#endif
}

void VehicleEditor::loadFromDisk() {
    std::string text, err;
    VehicleSpec parsed;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "could not read the file";
        return;
    }
    if (!vehicle::parseOcVehicle(text, parsed, err)) {
        loaded_ = false;
        loadError_ = err;
        return;
    }
    spec_ = std::move(parsed);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    gestureOpen_ = false;
    if (selectedWheel_ >= static_cast<int>(spec_.wheels.size())) selectedWheel_ = -1;
}

std::string VehicleEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor whose dirty() is true (AssetEditor.cpp).
    return std::filesystem::path(path_).filename().string() + "###vehicle:" + path_;
}

bool VehicleEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // Never write what the loader would refuse: the game would drop the vehicle with one log line.
    std::string problem;
    if (!vehicle::validate(spec_, problem)) {
        if (why) *why = problem;
        return false;
    }
    if (!writeFileTextAtomic(path_, vehicle::writeOcVehicle(spec_))) {
        if (why) *why = "could not write the file";
        return false;
    }
    dirty_ = false;
    return true;
}

void VehicleEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS: reloading here would discard what the author typed because something else touched the file.
    if (dirty_) {
        AVER_WARN("[VehicleEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void VehicleEditor::undo() {
    if (!history_.undo(spec_)) return;
    dirty_ = true;
    gestureOpen_ = false;
    if (selectedWheel_ >= static_cast<int>(spec_.wheels.size())) selectedWheel_ = -1;
}

void VehicleEditor::redo() {
    if (!history_.redo(spec_)) return;
    dirty_ = true;
    gestureOpen_ = false;
    if (selectedWheel_ >= static_cast<int>(spec_.wheels.size())) selectedWheel_ = -1;
}

void VehicleEditor::setKind(vehicle::Kind kind) {
    if (!loaded_ || spec_.kind == kind) return;
    edit([&](VehicleSpec& s) {
        s.kind = kind;
        if (kind == vehicle::Kind::Tracked) {
            ve::autoTracks(s);
        } else {
            for (vehicle::TrackDef& t : s.tracks) t.present = false;
            bool any = false;
            for (const vehicle::WheelDef& w : s.wheels) any = any || w.driven;
            if (!any && !s.wheels.empty()) {
                // A wheeled vehicle needs a driven wheel: drive the rearmost axle.
                const std::vector<ve::AxleSlot> axles = ve::axleLayout(s);
                if (!axles.empty()) {
                    const ve::AxleSlot& rear = axles.back();
                    if (rear.left >= 0) s.wheels[static_cast<usize>(rear.left)].driven = true;
                    if (rear.right >= 0) s.wheels[static_cast<usize>(rear.right)].driven = true;
                }
            }
        }
    });
}

bool VehicleEditor::addWheelPair(f32 x) {
    if (!loaded_ || spec_.wheels.size() + 2 > ve::kMaxWheels) return false;
    edit([&](VehicleSpec& s) {
        const int first = ve::addWheelPair(s, x);
        if (first >= 0) selectedWheel_ = first;
        if (s.kind == vehicle::Kind::Tracked) ve::autoTracks(s);
    });
    return true;
}

bool VehicleEditor::removeSelectedWheel() {
    if (!loaded_ || selectedWheel_ < 0 || selectedWheel_ >= static_cast<int>(spec_.wheels.size()) || spec_.wheels.size() <= 1) return false;
    const usize at = static_cast<usize>(selectedWheel_);
    edit([&](VehicleSpec& s) { ve::removeWheel(s, at); });
    selectedWheel_ = std::min(selectedWheel_, static_cast<int>(spec_.wheels.size()) - 1);
    return true;
}

void VehicleEditor::publishPreview() {
#if AVER_MODULE_VEHICLE_SCENE
    VehicleSpecPreview& p = vehicleSpecPreview();
    p.active = loaded_;
    p.owner = this;
    p.spec = spec_;
    p.selectedWheel = selectedWheel_;
#endif
}

// ---- drawing ----------------------------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

namespace {

const ImVec4 kRed{0.95f, 0.35f, 0.25f, 1.0f};
const ImVec4 kAmber{0.95f, 0.75f, 0.3f, 1.0f};

// Read-only polyline plot in a dark box, for the computed power curve.
void plotLine(const char* id, const std::vector<Vec2>& pts, f32 xMin, f32 xMax, f32 yMax, f32 height, ImU32 colour) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 width = std::max(120.0f, ImGui::GetContentRegionAvail().x - 6.0f);
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(24, 24, 30, 255));
    const auto toScreen = [&](const Vec2& p) {
        return ImVec2(origin.x + (p.x - xMin) / std::max(xMax - xMin, 1.0e-6f) * width, origin.y + height - std::min(std::max(p.y / std::max(yMax, 1.0e-6f), 0.0f), 1.0f) * height);
    };
    for (usize i = 1; i < pts.size(); ++i) dl->AddLine(toScreen(pts[i - 1]), toScreen(pts[i]), colour, 2.0f);
    ImGui::Dummy(ImVec2(width, height + 4.0f));
    ImGui::PopID();
}

} // namespace

void VehicleEditor::gesture(bool changed, const std::function<void()>& write) {
    if (ImGui::IsItemActivated() && !gestureOpen_) {
        gestureBefore_ = spec_;
        gestureOpen_ = true;
    }
    bool oneShot = false;
    if (changed) {
        if (!gestureOpen_) { gestureBefore_ = spec_; oneShot = true; }   // a typed value or a keyboard step: no activation seen
        write();
        dirty_ = true;
        if (oneShot) history_.push(gestureBefore_);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && gestureOpen_) {
        history_.push(gestureBefore_);
        gestureOpen_ = false;
    } else if (ImGui::IsItemDeactivated() && gestureOpen_) {
        gestureOpen_ = false;   // clicked, changed nothing
    }
}

void VehicleEditor::instant(bool changed, const std::function<void()>& write) {
    if (!changed) return;
    history_.push(spec_);
    write();
    dirty_ = true;
}

bool VehicleEditor::dragF(const char* label, f32& field, f32 speed, f32 lo, f32 hi, const char* format) {
    f32 v = field;
    const bool c = ImGui::DragFloat(label, &v, speed, lo, hi, format);
    gesture(c, [&] { field = v; });
    return c;
}

bool VehicleEditor::dragI(const char* label, int& field, f32 speed, int lo, int hi) {
    int v = field;
    const bool c = ImGui::DragInt(label, &v, speed, lo, hi);
    gesture(c, [&] { field = v; });
    return c;
}

bool VehicleEditor::check(const char* label, bool& field) {
    bool v = field;
    const bool c = ImGui::Checkbox(label, &v);
    instant(c, [&] { field = v; });
    return c;
}

bool VehicleEditor::text(const char* label, std::string& field, const char* hint) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", field.c_str());
    const bool c = ImGui::InputText(label, buf, sizeof buf);
    gesture(c, [&] { field = buf; });
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    return c;
}

bool VehicleEditor::drag3(const char* label, Vec3& field, f32 speed, f32 lo, f32 hi, const char* format) {
    f32 v[3] = {field.x, field.y, field.z};
    const bool c = ImGui::DragFloat3(label, v, speed, lo, hi, format);
    gesture(c, [&] { field = Vec3(v[0], v[1], v[2]); });
    return c;
}

bool VehicleEditor::curveEditor(const char* id, std::vector<Vec2>& pts, f32 xMin, f32 xMax, f32 yMax, f32 height, unsigned colour, usize maxPoints) {
    bool any = false;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 width = std::max(120.0f, ImGui::GetContentRegionAvail().x - 6.0f);
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(24, 24, 30, 255));
    for (int g = 1; g < 4; ++g) {
        const f32 gx = origin.x + width * static_cast<f32>(g) / 4.0f, gy = origin.y + height * static_cast<f32>(g) / 4.0f;
        dl->AddLine(ImVec2(gx, origin.y), ImVec2(gx, origin.y + height), IM_COL32(60, 60, 70, 255));
        dl->AddLine(ImVec2(origin.x, gy), ImVec2(origin.x + width, gy), IM_COL32(60, 60, 70, 255));
    }
    const auto toScreen = [&](const Vec2& p) {
        return ImVec2(origin.x + (p.x - xMin) / (xMax - xMin) * width, origin.y + height - std::min(std::max(p.y / yMax, 0.0f), 1.0f) * height);
    };
    const auto fromScreen = [&](const ImVec2& m) {
        return Vec2(xMin + (m.x - origin.x) / width * (xMax - xMin), (origin.y + height - m.y) / height * yMax);
    };
    ImGui::InvisibleButton("##plot", ImVec2(width, height));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        const Vec2 at = fromScreen(mouse);
        std::vector<Vec2> trial = pts;
        if (ve::addCurvePoint(trial, at.x, xMin, xMax, maxPoints) >= 0) { instant(true, [&] { pts = trial; }); any = true; }
    }
    for (usize i = 1; i < pts.size(); ++i) dl->AddLine(toScreen(pts[i - 1]), toScreen(pts[i]), colour, 2.0f);

    for (usize i = 0; i < pts.size(); ++i) {
        const ImVec2 sp = toScreen(pts[i]);
        ImGui::SetCursorScreenPos(ImVec2(sp.x - 6.0f, sp.y - 6.0f));
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##pt", ImVec2(12.0f, 12.0f));
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        dl->AddCircleFilled(sp, hovered || active ? 6.0f : 4.0f, colour);
        std::vector<Vec2> trial = pts;
        const bool moved = active && ImGui::IsMouseDragging(0, 0.0f) && ve::setCurvePoint(trial, i, fromScreen(mouse).x, fromScreen(mouse).y, xMin, xMax, yMax);
        gesture(moved, [&] { pts = trial; });
        any = any || moved;
        if (hovered) {
            ImGui::SetTooltip("%.3f, %.3f\nDrag to move, right-click to remove", static_cast<double>(pts[i].x), static_cast<double>(pts[i].y));
            if (ImGui::IsMouseClicked(1)) {
                std::vector<Vec2> fewer = pts;
                if (ve::removeCurvePoint(fewer, i)) { instant(true, [&] { pts = fewer; }); any = true; ImGui::PopID(); break; }
            }
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + 4.0f));
    ImGui::Dummy(ImVec2(width, 4.0f));
    ImGui::PopID();
    return any;
}

void VehicleEditor::drawToolbar() {
    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[VehicleEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s   %zu wheels   %s", ve::driveLayout(spec_).c_str(), spec_.wheels.size(), spec_.kind == vehicle::Kind::Tracked ? "tracked" : "wheeled");
    ImGui::TextDisabled("Edits apply to vehicles built after the save (the next Play or spawn). The Vehicle section's live tuning is the in-Play loop.");
}

void VehicleEditor::drawIssues() {
    const std::vector<ve::Issue> issues = ve::validationList(spec_);
    char header[64];
    std::snprintf(header, sizeof header, "Validation (%zu)###issues", issues.size());
    if (!ImGui::CollapsingHeader(header, issues.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (issues.empty()) { ImGui::TextDisabled("No problems."); return; }
    for (const ve::Issue& i : issues) ImGui::TextColored(i.error ? kRed : kAmber, "%s %s", i.error ? "error:" : "note:", i.text.c_str());
    if (ve::hasErrors(issues)) ImGui::TextColored(kRed, "Save is refused until the errors are fixed.");
}

void VehicleEditor::drawBody() {
    vehicle::BodyDef& b = spec_.body;
    drag3("Half extents (cm)", b.half, 1.0f, 1.0f, 2000.0f, "%.0f");
    dragF("Clearance (cm)", b.clearance, 0.5f, 0.0f, 200.0f, "%.1f");
    drag3("Centre of mass (cm)", b.com, 0.5f, -500.0f, 500.0f, "%.1f");
    dragF("Mass (kg)", b.massKg, 10.0f, 1.0f, 200000.0f, "%.0f");
    dragF("Pitch / roll limit (deg)", b.pitchRollDeg, 1.0f, 0.0f, 180.0f, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The chassis' up stays within this of the world's; 0 or 180 turns the limit off.");

    ImGui::SeparatorText("Collision boxes");
    ImGui::TextDisabled(spec_.boxes.empty() ? "None: the body box above is the chassis." : "These replace the body box.");
    for (usize i = 0; i < spec_.boxes.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        vehicle::BoxDef& x = spec_.boxes[i];
        f32 c[3] = {x.cx, x.cy, x.cz}, h[3] = {x.hx, x.hy, x.hz};
        bool ch = ImGui::DragFloat3("Centre", c, 0.5f, -1000.0f, 1000.0f, "%.1f");
        gesture(ch, [&] { x.cx = c[0]; x.cy = c[1]; x.cz = c[2]; });
        bool hh = ImGui::DragFloat3("Half extents", h, 0.5f, 0.5f, 1000.0f, "%.1f");
        gesture(hh, [&] { x.hx = h[0]; x.hy = h[1]; x.hz = h[2]; });
        if (ImGui::SmallButton("Remove box")) { instant(true, [&] { spec_.boxes.erase(spec_.boxes.begin() + static_cast<std::ptrdiff_t>(i)); }); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(spec_.boxes.size() >= vehicle::kMaxBoxes);
    if (ImGui::SmallButton("Add box")) {
        instant(true, [&] {
            vehicle::BoxDef nb;
            nb.cz = b.clearance + b.half.z;
            nb.hx = b.half.x; nb.hy = b.half.y; nb.hz = b.half.z;
            spec_.boxes.push_back(nb);
        });
    }
    ImGui::EndDisabled();
}

void VehicleEditor::drawEngine() {
    vehicle::EngineDef& e = spec_.engine;
    dragF("Peak torque (N*m)", e.torque, 5.0f, 1.0f, 20000.0f, "%.0f");
    dragF("Idle rpm", e.minRpm, 10.0f, 0.0f, 20000.0f, "%.0f");
    dragF("Rev limit (rpm)", e.maxRpm, 10.0f, 100.0f, 30000.0f, "%.0f");
    dragF("Inertia (kg m^2)", e.inertia, 0.01f, 0.0f, 50.0f, e.inertia > 0.0f ? "%.2f" : "auto");
    dragF("Engine braking", e.damping, 0.005f, 0.0f, 5.0f, "%.3f");

    ImGui::SeparatorText("Torque curve");
    const bool custom = !e.curve.empty();
    bool customOn = custom;
    if (ImGui::Checkbox("Custom curve", &customOn))
        instant(true, [&] { e.curve = customOn ? ve::defaultTorqueCurve() : std::vector<Vec2>{}; });
    ImGui::TextDisabled("x: engine speed as a fraction of the rev limit, y: fraction of the peak torque. Double-click adds a point.");
    if (custom) {
        curveEditor("torque", e.curve, 0.0f, 1.0f, 1.25f, 140.0f, IM_COL32(255, 190, 70, 255), vehicle::kMaxCurvePoints);
    } else {
        std::vector<Vec2> shown = ve::defaultTorqueCurve();
        plotLine("torque_default", shown, 0.0f, 1.0f, 1.25f, 140.0f, IM_COL32(150, 150, 150, 255));
        ImGui::TextDisabled("The engine's built-in curve.");
    }
    ImGui::SeparatorText("Power curve (computed)");
    plotLine("power", ve::powerCurve(spec_), e.minRpm, e.maxRpm, std::max(1.0f, ve::enginePeaks(spec_).powerKw * 1.1f), 110.0f, IM_COL32(120, 200, 255, 255));
    ImGui::TextDisabled("%s", ve::peaksText(ve::enginePeaks(spec_)).c_str());
}

void VehicleEditor::drawGearbox() {
    vehicle::GearboxDef& g = spec_.gearbox;
    bool present = g.present;
    if (ImGui::Checkbox("Custom gearbox", &present)) {
        instant(true, [&] {
            g.present = present;
            if (present && g.forward.empty()) g.forward = ve::defaultForwardRatios();
            if (present && g.reverse.empty()) g.reverse = ve::defaultReverseRatios();
        });
    }
    ImGui::TextDisabled("Off: the engine's five-speed automatic.");
    if (g.present) {
        int mode = g.manual ? 1 : 0;
        static const char* const kModes[] = {"Automatic", "Manual"};
        if (ImGui::Combo("Mode", &mode, kModes, 2)) instant(true, [&] { g.manual = mode == 1; });
        ImGui::SeparatorText("Forward ratios");
        for (usize i = 0; i < g.forward.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            f32 v = g.forward[i];
            char label[24];
            std::snprintf(label, sizeof label, "Gear %zu", i + 1);
            const bool c = ImGui::DragFloat(label, &v, 0.01f, 0.05f, 20.0f, "%.2f");
            gesture(c, [&] { g.forward[i] = v; });
            ImGui::PopID();
        }
        ImGui::BeginDisabled(g.forward.size() >= 16);
        if (ImGui::SmallButton("Add gear")) instant(true, [&] { g.forward.push_back(g.forward.empty() ? 3.0f : std::max(0.3f, g.forward.back() * 0.8f)); });
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(g.forward.size() <= 1);
        if (ImGui::SmallButton("Remove last")) instant(true, [&] { g.forward.pop_back(); });
        ImGui::EndDisabled();
        ImGui::SeparatorText("Reverse ratio");
        if (g.reverse.empty()) g.reverse = ve::defaultReverseRatios();   // a present gearbox always shows one; the writer emits it
        dragF("Reverse", g.reverse[0], 0.01f, 0.05f, 20.0f, "%.2f");
        ImGui::SeparatorText("Shifting");
        dragF("Switch time (s)", g.switchTime, 0.01f, 0.0f, 5.0f, "%.2f");
        dragF("Clutch release (s)", g.clutchRelease, 0.01f, 0.0f, 5.0f, "%.2f");
        dragF("Latency (s)", g.latency, 0.01f, 0.0f, 5.0f, "%.2f");
        dragF("Shift up at", g.up, 0.005f, 0.05f, 0.99f, "%.2f");
        dragF("Shift down at", g.down, 0.005f, 0.01f, 0.95f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fractions of the rev range. Down must be below up.");
        dragF("Clutch strength", g.clutch, 0.5f, 0.0f, 500.0f, g.clutch > 0.0f ? "%.1f" : "auto");
    }
    ImGui::SeparatorText("Top speed per gear (computed)");
    for (const ve::GearRow& row : ve::gearRows(spec_)) ImGui::TextUnformatted(ve::gearRowText(row).c_str());
    ImGui::TextDisabled("At the rev limit, from the driven wheel's radius and the first driven axle's final drive.");
}

void VehicleEditor::drawAxles() {
    ImGui::Text("Layout: %s", ve::driveLayout(spec_).c_str());
    int kind = spec_.kind == vehicle::Kind::Tracked ? 1 : 0;
    static const char* const kKinds[] = {"Wheeled", "Tracked"};
    if (ImGui::Combo("Kind", &kind, kKinds, 2)) setKind(kind == 1 ? vehicle::Kind::Tracked : vehicle::Kind::Wheeled);

    static const char* const kDiffs[] = {"Open", "Limited slip", "Locked"};
    if (spec_.kind == vehicle::Kind::Wheeled) {
        const std::vector<ve::AxleSlot> axles = ve::axleLayout(spec_);
        const std::vector<bool> driven = ve::drivenAxles(spec_);
        for (usize i = 0; i < axles.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char header[48];
            std::snprintf(header, sizeof header, "Axle %zu%s%s", i, i == 0 ? " (front)" : "", driven[i] ? "  driven" : "");
            if (ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                const vehicle::AxleDef a = ve::axleDef(spec_, i);
                f32 fd = a.finalDrive, split = a.split, lsd = a.lsd, share = a.share, roll = a.antiroll;
                int diff = static_cast<int>(a.diff);
                bool c = ImGui::DragFloat("Final drive", &fd, 0.01f, 0.1f, 30.0f, "%.2f");
                gesture(c, [&] { ve::ensureAxles(spec_); spec_.axles[i].finalDrive = fd; });
                if (ImGui::Combo("Differential", &diff, kDiffs, 3)) instant(true, [&] { ve::ensureAxles(spec_); spec_.axles[i].diff = static_cast<vehicle::DiffMode>(diff); });
                if (a.diff == vehicle::DiffMode::LimitedSlip) {
                    c = ImGui::DragFloat("Limited-slip ratio", &lsd, 0.01f, 1.01f, 20.0f, "%.2f");
                    gesture(c, [&] { ve::ensureAxles(spec_); spec_.axles[i].lsd = lsd; });
                }
                c = ImGui::SliderFloat("Left / right split", &split, 0.0f, 1.0f, "%.2f");
                gesture(c, [&] { ve::ensureAxles(spec_); spec_.axles[i].split = split; });
                c = ImGui::DragFloat("Torque share", &share, 0.01f, 0.0f, 10.0f, share > 0.0f ? "%.2f" : "equal");
                gesture(c, [&] { ve::ensureAxles(spec_); spec_.axles[i].share = share; });
                c = ImGui::DragFloat("Anti-roll bar (N/m)", &roll, 10.0f, 0.0f, 100000.0f, roll > 0.0f ? "%.0f" : "none");
                gesture(c, [&] { ve::ensureAxles(spec_); spec_.axles[i].antiroll = roll; });
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ve::drivenAxles(spec_).size() > 1) {
            ImGui::SeparatorText("Centre differential");
            bool cp = spec_.centerPresent;
            if (ImGui::Checkbox("Set the centre differential", &cp)) instant(true, [&] { spec_.centerPresent = cp; });
            if (spec_.centerPresent) {
                int cd = static_cast<int>(spec_.center);
                if (ImGui::Combo("Centre mode", &cd, kDiffs, 3)) instant(true, [&] { spec_.center = static_cast<vehicle::DiffMode>(cd); });
                if (spec_.center == vehicle::DiffMode::LimitedSlip) dragF("Centre limited-slip ratio", spec_.centerLsd, 0.01f, 1.01f, 20.0f, "%.2f");
            }
        }
    } else {
        if (ImGui::Button("Rebuild tracks from the wheel sides")) instant(true, [&] { ve::autoTracks(spec_); });
        static const char* const kSides[] = {"Left track", "Right track"};
        for (int side = 0; side < 2; ++side) {
            vehicle::TrackDef& t = spec_.tracks[side];
            if (!ImGui::TreeNodeEx(kSides[side], ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(side);
            ImGui::TextDisabled("%zu wheels", t.wheels.size());
            int driven = static_cast<int>(t.driven);
            if (ImGui::InputInt("Driven wheel", &driven)) {
                const u32 candidate = static_cast<u32>(std::max(driven, 0));
                if (std::find(t.wheels.begin(), t.wheels.end(), candidate) != t.wheels.end()) instant(true, [&] { t.driven = candidate; });
            }
            dragF("Inertia", t.inertia, 0.1f, 0.01f, 1000.0f, "%.2f");
            dragF("Damping", t.damping, 0.01f, 0.0f, 50.0f, "%.2f");
            dragF("Brake (N*m)", t.brakeNm, 100.0f, 0.0f, 1000000.0f, "%.0f");
            dragF("Final drive", t.finalDrive, 0.05f, 0.1f, 50.0f, "%.2f");
            dragF("Steer gain", t.steerGain, 0.01f, 0.0f, 10.0f, "%.2f");
            dragF("Pivot", t.pivot, 0.01f, 0.0f, 5.0f, "%.2f");
            ImGui::PopID();
            ImGui::TreePop();
        }
    }
}

void VehicleEditor::drawWheels() {
    ImGui::TextDisabled("Hub = the SETTLED wheel centre, relative to the vehicle origin (the bottom centre). The tyre bottom sits on the ground when z equals the radius.");
    if (ImGui::BeginTable("##wheels", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders)) {
        static const char* const kCols[] = {"#", "Name", "Class", "Hub (cm)", "Steer", "Driven", "Handbrake"};
        for (const char* c : kCols) ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < spec_.wheels.size(); ++i) {
            const vehicle::WheelDef& w = spec_.wheels[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char id[16];
            std::snprintf(id, sizeof id, "%zu##w", i);
            if (ImGui::Selectable(id, static_cast<int>(i) == selectedWheel_, ImGuiSelectableFlags_SpanAllColumns)) selectedWheel_ = static_cast<int>(i);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(w.name.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(w.wheelClass.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f  %.0f  %.0f", static_cast<double>(w.hub.x), static_cast<double>(w.hub.y), static_cast<double>(w.hub.z));
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.0f", static_cast<double>(w.steerDeg));
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(w.driven ? "yes" : "");
            ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(w.handbrake ? "yes" : "");
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(spec_.wheels.size() + 2 > ve::kMaxWheels);
    if (ImGui::SmallButton("Add axle (two wheels)")) {
        f32 minX = 0.0f;
        for (const vehicle::WheelDef& w : spec_.wheels) minX = std::min(minX, w.hub.x);
        addWheelPair(spec_.wheels.empty() ? 0.0f : minX - 100.0f);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedWheel_ < 0 || spec_.wheels.size() <= 1);
    if (ImGui::SmallButton("Remove selected wheel")) removeSelectedWheel();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("The selected wheel is highlighted over the selected entity in the viewport.");

    if (selectedWheel_ >= 0 && selectedWheel_ < static_cast<int>(spec_.wheels.size())) {
        ImGui::SeparatorText("Selected wheel");
        vehicle::WheelDef& w = spec_.wheels[static_cast<usize>(selectedWheel_)];
        text("Name", w.name);
        if (ImGui::BeginCombo("Class", w.wheelClass.c_str())) {
            for (const vehicle::WheelClass& c : spec_.classes)
                if (ImGui::Selectable(c.name.c_str(), c.name == w.wheelClass)) instant(true, [&] { w.wheelClass = c.name; });
            ImGui::EndCombo();
        }
        drag3("Hub (cm)", w.hub, 0.5f, -2000.0f, 2000.0f, "%.1f");
        dragF("Steer (deg)", w.steerDeg, 0.5f, -90.0f, 90.0f, "%.1f");
        check("Driven", w.driven);
        ImGui::SameLine();
        check("Handbrake", w.handbrake);
        dragF("Brake (N*m)", w.brakeNm, 10.0f, -1.0f, 100000.0f, w.brakeNm >= 0.0f ? "%.0f" : "the class'");
        text("Visual node", w.node, "The name of the child entity that draws this wheel. Empty: no visual.");
        text("Bone", w.bone, "The skeleton bone for the skeletal route (named control-rig inputs).");
    }

    ImGui::SeparatorText("Wheel classes");
    for (usize i = 0; i < spec_.classes.size(); ++i) {
        vehicle::WheelClass& c = spec_.classes[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNode(c.name.c_str())) {
            text("Name", c.name);
            dragF("Radius (cm)", c.radius, 0.2f, 1.0f, 300.0f, "%.1f");
            dragF("Width (cm)", c.width, 0.2f, 1.0f, 200.0f, "%.1f");
            dragF("Suspension min (cm)", c.suspMin, 0.2f, 0.0f, 200.0f, "%.1f");
            dragF("Suspension max (cm)", c.suspMax, 0.2f, 0.1f, 300.0f, "%.1f");
            dragF("Spring (Hz)", c.suspHz, 0.01f, 0.1f, 20.0f, "%.2f");
            dragF("Damping", c.suspDamping, 0.01f, 0.0f, 5.0f, "%.2f");
            dragF("Brake (N*m)", c.brakeNm, 10.0f, 0.0f, 100000.0f, "%.0f");
            dragF("Handbrake (N*m)", c.handbrakeNm, 10.0f, 0.0f, 100000.0f, "%.0f");
            dragF("Inertia (kg m^2)", c.inertia, 0.01f, 0.01f, 100.0f, "%.2f");
            dragF("Spin damping", c.spinDamping, 0.01f, 0.0f, 5.0f, "%.2f");
            if (ImGui::BeginCombo("Tyre", c.tyre.empty() ? "(engine default)" : c.tyre.c_str())) {
                if (ImGui::Selectable("(engine default)", c.tyre.empty())) instant(true, [&] { c.tyre.clear(); });
                for (const vehicle::TyreCurve& t : spec_.tyres)
                    if (ImGui::Selectable(t.name.c_str(), t.name == c.tyre)) instant(true, [&] { c.tyre = t.name; });
                ImGui::EndCombo();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void VehicleEditor::drawTyres() {
    ImGui::TextDisabled("A tyre is two friction curves: longitudinal (slip ratio against mu) and lateral (slip angle in degrees against mu). Double-click adds a point.");
    for (usize i = 0; i < spec_.tyres.size(); ++i) {
        vehicle::TyreCurve& t = spec_.tyres[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNodeEx(t.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            text("Name", t.name);
            ImGui::TextDisabled("Longitudinal");
            curveEditor("long", t.longitudinal, 0.0f, 1.0f, 2.0f, 110.0f, IM_COL32(255, 120, 100, 255), vehicle::kMaxTyrePoints);
            ImGui::TextDisabled("Lateral");
            curveEditor("lat", t.lateral, 0.0f, 45.0f, 2.0f, 110.0f, IM_COL32(110, 160, 255, 255), vehicle::kMaxTyrePoints);
            if (ImGui::SmallButton("Delete tyre")) {
                instant(true, [&] {
                    const std::string gone = t.name;
                    for (vehicle::WheelClass& c : spec_.classes) if (c.tyre == gone) c.tyre.clear();
                    spec_.tyres.erase(spec_.tyres.begin() + static_cast<std::ptrdiff_t>(i));
                });
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add tyre")) {
        instant(true, [&] {
            vehicle::TyreCurve t;
            t.name = "tyre" + std::to_string(spec_.tyres.size() + 1);
            t.longitudinal = ve::defaultLongitudinalCurve();
            t.lateral = ve::defaultLateralCurve();
            spec_.tyres.push_back(t);
        });
    }
}

void VehicleEditor::drawAssists() {
    vehicle::AssistDef& a = spec_.assist;
    bool present = a.present;
    if (ImGui::Checkbox("Driver aids", &present)) instant(true, [&] { a.present = present; });
    ImGui::TextDisabled("Off: the pedals reach the physics unchanged.");
    if (!a.present) return;
    check("ABS", a.abs);
    ImGui::SameLine();
    check("Traction control", a.tcs);
    check("Brake to reverse", a.reverseBrake);
    if (a.abs) { dragF("ABS slip", a.absSlip, 0.005f, 0.01f, 1.0f, "%.2f"); dragF("ABS release rate", a.absRelease, 0.1f, 0.1f, 50.0f, "%.1f"); }
    if (a.tcs) dragF("TCS slip", a.tcsSlip, 0.005f, 0.01f, 1.0f, "%.2f");
    ImGui::SeparatorText("Pedal and steering rates (per second)");
    dragF("Steer in", a.steerIn, 0.1f, 0.1f, 50.0f, "%.1f");
    dragF("Steer out", a.steerOut, 0.1f, 0.1f, 50.0f, "%.1f");
    dragF("Throttle in", a.throttleIn, 0.1f, 0.1f, 50.0f, "%.1f");
    dragF("Throttle out", a.throttleOut, 0.1f, 0.1f, 50.0f, "%.1f");
    dragF("Brake in", a.brakeIn, 0.1f, 0.1f, 50.0f, "%.1f");
    ImGui::SeparatorText("Standstill");
    dragF("Reverse below (cm/s)", a.reverseSpeed, 1.0f, 0.0f, 500.0f, "%.0f");
    dragF("Hold brake", a.holdBrake, 0.01f, 0.0f, 1.0f, "%.2f");
    dragF("Hold below (cm/s)", a.holdSpeed, 1.0f, 0.0f, 500.0f, "%.0f");
    ImGui::SeparatorText("Skid detection");
    dragF("Skid slip", a.skidSlip, 0.005f, 0.05f, 2.0f, "%.2f");
    dragF("Skid angle (deg)", a.skidAngle, 0.1f, 1.0f, 90.0f, "%.1f");
}

void VehicleEditor::drawAero() {
    bool present = spec_.aeroPresent;
    if (ImGui::Checkbox("Aerodynamics", &present)) instant(true, [&] { spec_.aeroPresent = present; });
    if (spec_.aeroPresent) {
        dragF("Drag (N per (m/s)^2)", spec_.drag, 0.01f, 0.0f, 100.0f, "%.2f");
        dragF("Downforce (N per (m/s)^2)", spec_.downforce, 0.01f, 0.0f, 100.0f, "%.2f");
    }
    ImGui::SeparatorText("Steering by speed");
    ImGui::TextDisabled("Points of (speed cm/s, fraction of full lock); up to four. Empty: full lock at every speed.");
    for (usize i = 0; i < spec_.steer.points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        Vec2& p = spec_.steer.points[i];
        f32 v[2] = {p.x, p.y};
        const bool c = ImGui::DragFloat2("Speed, fraction", v, 1.0f, 0.0f, 100000.0f, "%.2f");
        gesture(c, [&] { p = Vec2(std::max(v[0], 0.0f), std::max(v[1], 0.0f)); });
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { instant(true, [&] { spec_.steer.points.erase(spec_.steer.points.begin() + static_cast<std::ptrdiff_t>(i)); }); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(spec_.steer.points.size() >= vehicle::kMaxSteerPoints);
    if (ImGui::SmallButton("Add steering point")) {
        instant(true, [&] {
            const f32 lastX = spec_.steer.points.empty() ? 0.0f : spec_.steer.points.back().x;
            spec_.steer.points.emplace_back(lastX + 500.0f, 1.0f);
        });
    }
    ImGui::EndDisabled();
}

void VehicleEditor::drawAudio() {
    vehicle::AudioDef& a = spec_.audio;
    bool present = a.present;
    if (ImGui::Checkbox("Sound", &present)) instant(true, [&] { a.present = present; });
    if (!a.present) return;
    text("Idle loop", a.idle, "Content-relative path to the idle engine loop.");
    text("High-rev loop", a.high, "Optional second loop crossfaded in with the revs. Empty: the idle loop is pitched.");
    dragF("Idle rpm", a.idleRpm, 10.0f, 0.0f, 20000.0f, "%.0f");
    dragF("High rpm", a.highRpm, 10.0f, 0.0f, 30000.0f, "%.0f");
    dragF("Pitch at idle", a.pitchLow, 0.01f, 0.1f, 4.0f, "%.2f");
    dragF("Pitch at the limit", a.pitchHigh, 0.01f, 0.1f, 8.0f, "%.2f");
    text("Skid sound", a.skid);
    text("Impact sound", a.impact);
    text("Shift sound", a.shift);
}

void VehicleEditor::drawEffects() {
    vehicle::EffectDef& f = spec_.effects;
    bool present = f.present;
    if (ImGui::Checkbox("Particle effects", &present)) instant(true, [&] { f.present = present; });
    if (!f.present) return;
    text("Skid smoke (.ocparticle)", f.skid);
    dragF("Smoke rate (per second per skidding wheel)", f.skidRate, 0.5f, 0.0f, 500.0f, "%.0f");
    text("Impact sparks (.ocparticle)", f.impact);
    int count = static_cast<int>(f.impactCount);
    const bool c = ImGui::DragInt("Spark count", &count, 0.2f, 0, 200);
    gesture(c, [&] { f.impactCount = static_cast<u32>(std::max(count, 0)); });
}

void VehicleEditor::drawDamage() {
    vehicle::DamageDef& d = spec_.damage;
    bool present = d.present;
    if (ImGui::Checkbox("Damage", &present)) instant(true, [&] { d.present = present; });
    ImGui::TextDisabled("Off: the vehicle is never hurt or wrecked.");
    if (!d.present) return;
    dragF("Health", d.health, 5.0f, 0.0f, 100000.0f, "%.0f");
    dragF("Hurts above (cm/s closing)", d.crash, 5.0f, 0.0f, 10000.0f, "%.0f");
    dragF("Energy share", d.energy, 0.01f, 0.0f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales the energy handed to the soft-body crash sink.");
    dragF("Crash radius (cm)", d.radius, 1.0f, 0.0f, 1000.0f, "%.0f");
    text("Shell entity", d.shell, "A child entity carrying a Destructible that tears on a hard hit.");
}

void VehicleEditor::drawCamera() {
    vehicle::CameraDef& c = spec_.camera;
    bool present = c.present;
    if (ImGui::Checkbox("Camera rigs", &present)) instant(true, [&] { c.present = present; });
    if (c.present) {
        static const char* const kNames[] = {"chase", "hood", "cockpit"};
        ImGui::TextDisabled("Views the camera key (Drive.CameraNext) cycles through:");
        for (const char* name : kNames) {
            const auto it = std::find(c.modes.begin(), c.modes.end(), name);
            bool on = it != c.modes.end();
            if (ImGui::Checkbox(name, &on))
                instant(true, [&] {
                    std::vector<std::string> next;
                    for (const char* n : kNames) {   // keep the canonical order
                        const bool had = std::find(c.modes.begin(), c.modes.end(), n) != c.modes.end();
                        if (std::string(n) == name ? on : had) next.emplace_back(n);
                    }
                    c.modes = next;
                });
            ImGui::SameLine();
        }
        ImGui::NewLine();
        dragF("Chase arm (cm)", c.arm, 1.0f, 50.0f, 5000.0f, "%.0f");
        drag3("Cockpit eye (cm)", c.eye, 0.5f, -1000.0f, 1000.0f, "%.1f");
        drag3("Hood point (cm)", c.hood, 0.5f, -1000.0f, 1000.0f, "%.1f");
    }
    ImGui::SeparatorText("Seat");
    bool seat = spec_.seat.present;
    if (ImGui::Checkbox("Seat positions", &seat)) instant(true, [&] { spec_.seat.present = seat; });
    if (spec_.seat.present) {
        drag3("Driver (cm)", spec_.seat.driver, 0.5f, -1000.0f, 1000.0f, "%.1f");
        drag3("Exit (cm)", spec_.seat.exit, 0.5f, -1000.0f, 1000.0f, "%.1f");
    }
    ImGui::SeparatorText("HUD");
    text("HUD layout (.ocui)", spec_.hud, "Content-relative path; the template's graphs open it on enter.");
}

void VehicleEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        if (ImGui::Button("Retry")) loadFromDisk();
        return;
    }
    drawToolbar();
    ImGui::Separator();
    drawIssues();

    struct Section { const char* key; void (VehicleEditor::*draw)(); };
    static const Section kDraw[] = {
        {"body", &VehicleEditor::drawBody}, {"engine", &VehicleEditor::drawEngine}, {"gearbox", &VehicleEditor::drawGearbox},
        {"axles", &VehicleEditor::drawAxles}, {"wheels", &VehicleEditor::drawWheels}, {"tyres", &VehicleEditor::drawTyres},
        {"assists", &VehicleEditor::drawAssists}, {"aero", &VehicleEditor::drawAero}, {"audio", &VehicleEditor::drawAudio},
        {"effects", &VehicleEditor::drawEffects}, {"damage", &VehicleEditor::drawDamage}, {"camera", &VehicleEditor::drawCamera},
    };
    for (usize i = 0; i < ve::kSectionCount; ++i) {
        ImGui::PushID(ve::kSections[i].key);
        if (ImGui::CollapsingHeader(ve::kSections[i].label, i < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            for (const Section& s : kDraw)
                if (std::strcmp(s.key, ve::kSections[i].key) == 0) (this->*s.draw)();
        }
        ImGui::PopID();
    }
    publishPreview();
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's own targets) still gets load / save / dirty / undo; only the window is absent.
void VehicleEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeVehicleEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocvehicle") return nullptr;
    return std::make_unique<VehicleEditor>(path);
}

} // namespace aver::editor
