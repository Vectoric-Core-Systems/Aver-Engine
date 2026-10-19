#pragma once
// The .ocvehicle editor tab (docs/VEHICLES.md "Editor"): a vehicle spec opened as an asset. Twelve sections (Body, Engine with an editable
// torque curve and a computed power curve, Gearbox with each gear's top speed, Axles and drive with the derived FWD / RWD / AWD label,
// Wheels with a viewport preview over the selected entity, Tyres with the two friction curves, Assists, Aero, Audio, Effects, Damage,
// Camera / seat / HUD) and a validation list that is vehicle::validate's output plus the advisories VehicleEditorModel.hpp adds.
//
// THE HOOK INTO SandboxApp IS THREE LINES, like FoliageTypeEditor:
//   #include "VehicleEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeVehicleEditor);   // APPENDED -- order is precedence
// and the Content Browser opens a double-clicked .ocvehicle through AssetEditorHost::open as for every asset editor.
//
// Edits apply to vehicles built AFTER the save (the next Play start or spawn); the Details section's live-tune sliders are the in-Play
// loop. The preview is written to vehicleSpecPreview() (VehicleDebugOverlay.hpp) each frame the tab is drawn and cleared when it closes.
//
// Undo is the shared SnapshotUndo<State> with the whole spec as the state (a spec is a few kilobytes), the same as BtEditor.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// What the Content Browser's "New Vehicle" writes: the sedan the Vehicle template ships, already valid and immediately openable.
inline std::string vehicleStarterText() {
    const vehicle::VehicleSpec* spec = vehicle::builtinSpec("sedan");
    return spec ? vehicle::writeOcVehicle(*spec) : std::string{};
}

class VehicleEditor final : public AssetEditor {
public:
    explicit VehicleEditor(std::string path);
    ~VehicleEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const vehicle::VehicleSpec& spec() const { return spec_; }
    void markDirty() { dirty_ = true; }
    int selectedWheel() const { return selectedWheel_; }

    // Whole-spec edit: snapshot for undo, run `change` on the spec, mark dirty. The widgets and the tests both go through it.
    template <typename Fn>
    void edit(Fn&& change) {
        if (!loaded_) return;
        history_.push(spec_);
        change(spec_);
        dirty_ = true;
    }

    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Changing the vehicle kind keeps the spec buildable: tracked gets tracks, wheeled drops them (and needs a driven wheel).
    void setKind(vehicle::Kind kind);
    bool addWheelPair(f32 x);
    bool removeSelectedWheel();
    void selectWheel(int index) { selectedWheel_ = index; }

private:
    void loadFromDisk();
    void publishPreview();

    std::string path_;
    vehicle::VehicleSpec spec_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    SnapshotUndo<vehicle::VehicleSpec> history_;
    int selectedWheel_ = -1;

    // The undo bracket of a drag / slider / text gesture: the spec as it was when the gesture began.
    vehicle::VehicleSpec gestureBefore_;
    bool gestureOpen_ = false;

#if AVER_WITH_IMGUI
    // ---- widget helpers: call straight after the ImGui widget with its return value ----
    void gesture(bool changed, const std::function<void()>& write);   // drag / slider / text: one undo step per gesture
    void instant(bool changed, const std::function<void()>& write);   // checkbox / combo / button: one undo step per change
    bool dragF(const char* label, f32& field, f32 speed, f32 lo, f32 hi, const char* format = "%.1f");
    bool dragI(const char* label, int& field, f32 speed, int lo, int hi);
    bool check(const char* label, bool& field);
    bool text(const char* label, std::string& field, const char* hint = nullptr);
    bool drag3(const char* label, Vec3& field, f32 speed, f32 lo, f32 hi, const char* format = "%.1f");
    // A plot of `pts` with draggable points (right-click removes, double-click adds). Returns whether the curve changed.
    bool curveEditor(const char* id, std::vector<Vec2>& pts, f32 xMin, f32 xMax, f32 yMax, f32 height, unsigned colour, usize maxPoints);

    void drawToolbar();
    void drawIssues();
    void drawBody();
    void drawEngine();
    void drawGearbox();
    void drawAxles();
    void drawWheels();
    void drawTyres();
    void drawAssists();
    void drawAero();
    void drawAudio();
    void drawEffects();
    void drawDamage();
    void drawCamera();
#endif
};

// Creates a vehicle editor for a .ocvehicle, else nullptr.
std::unique_ptr<AssetEditor> makeVehicleEditor(const std::string& path);

} // namespace aver::editor
