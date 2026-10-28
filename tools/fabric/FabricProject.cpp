// project.godot to .ocproject, the [input] map to .ocinput, translations to Content/Localization.
#include "FabricInternal.hpp"

#include "aver/core/Version.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/GodotEnvironment.hpp"
#include "aver/formats/OcInput.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <filesystem>

namespace aver::fabric {

double Converter::setting(const char* path, double def) const {
    const Var* v = project_.setting(path);
    return numOf(v, def);
}

std::string Converter::settingString(const char* path, const std::string& def) const {
    return stringOf(project_.setting(path), def);
}

bool Converter::settingBool(const char* path, bool def) const { return boolOf(project_.setting(path), def); }

namespace {

constexpr const char* kProject = "res://project.godot";

// Godot's Key enum values for the keys Aver's Key enum has (scripting/csharp/Aver.Framework/Input.cs).
const char* averKeyName(long long code) {
    static char buf[4];
    if (code >= 65 && code <= 90) { buf[0] = static_cast<char>(code); buf[1] = 0; return buf; }
    if (code >= 48 && code <= 57) { buf[0] = 'D'; buf[1] = static_cast<char>(code); buf[2] = 0; return buf; }
    switch (code) {
        case 32: return "Space";
        case 4194325: return "LeftShift";
        case 4194326: return "LeftCtrl";
        case 4194328: return "LeftAlt";
        case 4194309: case 4194310: return "Enter";
        case 4194305: return "Escape";
        case 4194306: return "Tab";
        case 4194319: return "Left";
        case 4194320: return "Up";
        case 4194321: return "Right";
        case 4194322: return "Down";
        default: return nullptr;
    }
}

const char* averPadButton(long long idx) {
    switch (idx) {
        case 0: return "A";
        case 1: return "B";
        case 2: return "X";
        case 3: return "Y";
        case 4: return "Back";
        case 6: return "Start";
        case 7: return "LeftThumb";
        case 8: return "RightThumb";
        case 9: return "LeftShoulder";
        case 10: return "RightShoulder";
        case 11: return "DPadUp";
        case 12: return "DPadDown";
        case 13: return "DPadLeft";
        case 14: return "DPadRight";
        default: return nullptr;   // 5 guide, 15 and up: misc, paddles, touchpad
    }
}

const char* averPadAxis(long long idx) {
    switch (idx) {
        case 0: return "LeftX";
        case 1: return "LeftY";
        case 2: return "RightX";
        case 3: return "RightY";
        case 4: return "LeftTrigger";
        case 5: return "RightTrigger";
        default: return nullptr;
    }
}

std::string keyDescription(long long code) {
    const char* n = averKeyName(code);
    return n ? n : "keycode " + std::to_string(code);
}

}  // namespace

void Converter::convertInput() {
    if (project_.inputActions.empty()) return;
    fmt::OcInputData data;
    data.name = name_;
    data.contextName = name_ + "Input";
    data.contextPriority = 0;

    for (const gd::GodotInputAction& act : project_.inputActions) {
        fmt::OcInputAction a;
        a.name = act.name;
        std::vector<fmt::OcInputBinding> binds;
        bool axis = false;
        int converted = 0, unmapped = 0;
        const auto skip = [&](const std::string& why, int line) {
            ++unmapped;
            report_.add("input", kProject, act.name, "InputEvent", "", Rating::Unsupported, why, line);
        };
        for (const Var& ev : act.events) {
            if (ev.type != VType::Object) { skip("an event that is not an InputEvent object", act.line); continue; }
            const std::string& cls = ev.s;
            fmt::OcInputBinding b;
            b.action = act.name;
            if (cls == "InputEventKey") {
                long long code = static_cast<long long>(numOf(ev.find("keycode"), 0));
                if (code == 0) code = static_cast<long long>(numOf(ev.find("physical_keycode"), 0));
                if (code == 0) code = static_cast<long long>(numOf(ev.find("key_label"), 0));
                const char* n = averKeyName(code);
                if (!n) { skip("the key (" + keyDescription(code) + ") is not in Aver's key set", act.line); continue; }
                b.source = fmt::OcInputSource::Key;
                b.key = n;
                if (boolOf(ev.find("shift_pressed"), false) || boolOf(ev.find("ctrl_pressed"), false) || boolOf(ev.find("alt_pressed"), false) ||
                    boolOf(ev.find("meta_pressed"), false))
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated, "the key's modifier requirement is dropped; the bare key triggers the action", act.line);
            } else if (cls == "InputEventMouseButton") {
                const int idx = static_cast<int>(numOf(ev.find("button_index"), 0));
                if (idx == 1 || idx == 2 || idx == 3) {
                    b.source = fmt::OcInputSource::Key;
                    b.key = idx == 1 ? "MouseLeft" : idx == 2 ? "MouseRight" : "MouseMiddle";
                } else if (idx == 4 || idx == 5) {
                    b.source = fmt::OcInputSource::MouseWheel;
                    b.scale = idx == 4 ? 1.0f : -1.0f;
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated, "a wheel notch is an axis value in Aver, a button press in Godot", act.line);
                } else {
                    skip("mouse button " + std::to_string(idx) + " (side buttons, horizontal wheel) is not in Aver's input set", act.line);
                    continue;
                }
            } else if (cls == "InputEventJoypadButton") {
                const char* n = averPadButton(static_cast<long long>(numOf(ev.find("button_index"), -1)));
                if (!n) { skip("gamepad button " + std::to_string(static_cast<long long>(numOf(ev.find("button_index"), -1))) + " is not in Aver's gamepad set", act.line); continue; }
                b.source = fmt::OcInputSource::GamepadButton;
                b.key = n;
            } else if (cls == "InputEventJoypadMotion") {
                const long long idx = static_cast<long long>(numOf(ev.find("axis"), -1));
                const char* n = averPadAxis(idx);
                if (!n) { skip("gamepad axis " + std::to_string(idx) + " is not in Aver's gamepad set", act.line); continue; }
                b.source = fmt::OcInputSource::GamepadAxis;
                b.key = n;
                // Godot's stick Y runs down; XInput's (and so Aver's) runs up.
                const double value = numOf(ev.find("axis_value"), 1.0);
                const bool yAxis = idx == 1 || idx == 3;
                b.scale = static_cast<float>((value < 0 ? -1.0 : 1.0) * (yAxis ? -1.0 : 1.0));
                axis = true;
                report_.add("input", kProject, act.name, cls, "", Rating::Approximated,
                            "Godot's action strength is never negative; the Aver axis binding is signed" +
                                std::string(yAxis ? " (the stick's Y direction is flipped to XInput's)" : ""), act.line);
            } else {
                skip(cls + " is not an input the converter maps (mouse motion, touch and shortcuts are not bound by action)", act.line);
                continue;
            }
            ++converted;
            binds.push_back(std::move(b));
        }
        a.type = axis ? fmt::OcInputValueType::Axis1D : fmt::OcInputValueType::Digital;
        if (axis) {
            fmt::OcModifierSpec dz;
            dz.kind = fmt::OcModifierKind::DeadZone;
            dz.v[0] = static_cast<f32>(act.deadzone);
            dz.v[1] = 1.0f;
            a.modifiers.push_back(fmt::formatModifierSpec(dz));
        }
        data.actions.push_back(std::move(a));
        for (auto& b : binds) data.bindings.push_back(std::move(b));
        if (converted > 0) report_.add("input", kProject, act.name, "InputAction", "", unmapped ? Rating::Approximated : Rating::Converted,
                                       std::to_string(converted) + " binding(s)" + (unmapped ? ", " + std::to_string(unmapped) + " not mapped" : ""), act.line);
        else report_.add("input", kProject, act.name, "InputAction", "", Rating::Unsupported, "none of its events could be mapped", act.line);
    }
    report_.add("input", kProject, "", "InputMap", "", Rating::Approximated,
                "only the actions the project overrides are written; Godot's built-in ui_* actions are not", 0);

    const std::string path = contentDir_ + "/Input/" + name_ + ".ocinput";
    createDirectories(contentDir_ + "/Input");
    if (!writeFileText(path, fmt::writeOcinput(data))) {
        report_.add("input", kProject, "", "InputMap", "", Rating::Unsupported, "could not write " + path, 0);
        return;
    }
    fmt::OcInputData back;
    std::string err;
    if (!fmt::loadOcinput(path, back, &err))
        report_.add("input", kProject, "", "InputMap", "", Rating::Unsupported, "the written .ocinput did not read back: " + err, 0);
}

void Converter::convertLocalization() {
    const Var* list = project_.setting("internationalization/locale/translations");
    if (!list) return;
    for (const std::string& ref : list->strs) {
        const std::string res = gd::GodotProjectFs::normalizeRes(ref);
        const std::string disk = fs_.toDiskPath(res);
        const size_t d = res.find_last_of('.');
        const std::string ext = d == std::string::npos ? std::string{} : lower(res.substr(d));
        if (ext != ".po") {
            report_.add("setting", kProject, "", "Translation", "internationalization/locale/translations", Rating::Unsupported,
                        res + ": only .po translations are converted (resave a .translation or .csv as .po)", 0);
            continue;
        }
        std::string text;
        if (disk.empty() || !readFileText(disk, text)) {
            report_.add("setting", kProject, "", "Translation", "internationalization/locale/translations", Rating::Unsupported, res + " is missing", 0);
            continue;
        }
        // The culture is the file's Language header, else its name.
        std::string culture;
        const size_t at = text.find("Language:");
        if (at != std::string::npos) {
            size_t b = at + 9;
            while (b < text.size() && text[b] == ' ') ++b;
            size_t e = b;
            while (e < text.size() && text[e] != '\\' && text[e] != '\n' && text[e] != '"' && text[e] != ' ') ++e;
            culture = text.substr(b, e - b);
        }
        if (culture.empty()) culture = stemOf(res);
        culture = safeName(culture, "en");
        const std::string dst = contentDir_ + "/Localization/" + culture + "/" + stemOf(res) + ".po";
        createDirectories(std::filesystem::path(dst).parent_path().string());
        if (!writeFileText(dst, text)) {
            report_.add("setting", kProject, "", "Translation", "", Rating::Unsupported, "could not write " + dst, 0);
            continue;
        }
        report_.add("setting", kProject, "", "Translation", "", Rating::Converted, res + " copied to Localization/" + culture + "/", 0);
    }
}

bool Converter::convertProject(const std::string& startLevelAsset) {
    fmt::ProjectDesc d;
    d.name = name_;
    d.engineName = std::string(kEngineName);
    d.engineMinVersion = std::string(kEngineVersion);
    d.createdWith = std::string(kEngineVersion);
    d.contentRoot = "Content";
    d.startMap = startLevelAsset;
    if (!project_.inputActions.empty()) d.inputScheme = "Input/" + name_ + ".ocinput";
    d.localeDefault = settingString("internationalization/locale/fallback", "en");

    // Window.
    d.windowTitle = project_.name;
    d.windowWidth = static_cast<int>(setting("display/window/size/viewport_width", 1152));
    d.windowHeight = static_cast<int>(setting("display/window/size/viewport_height", 648));
    d.windowResizable = settingBool("display/window/size/resizable", true) ? 1 : 0;
    const int mode = static_cast<int>(setting("display/window/size/mode", 0));
    d.windowFullscreen = (mode == 3 || mode == 4) ? 1 : 0;
    report_.add("setting", kProject, "", "", "display/window/size", Rating::Converted, "WINDOW.SIZE / RESIZABLE / FULLSCREEN", 0);

    // Physics: Godot's rules, stated.
    const V3 gdir = vec3Of(project_.setting("physics/3d/default_gravity_vector"), {0, -1, 0});
    const double gmag = setting("physics/3d/default_gravity", 9.8);
    const Vec3 g = fmt::axis::position(static_cast<f32>(gdir.x * gmag), static_cast<f32>(gdir.y * gmag), static_cast<f32>(gdir.z * gmag));
    d.hasGravity = true;
    d.gravity[0] = g.x + 0.0f; d.gravity[1] = g.y + 0.0f; d.gravity[2] = g.z + 0.0f;   // + 0.0f: no negative zero in the file
    const double ticks = std::max(1.0, setting("physics/common/physics_ticks_per_second", 60));
    d.fixedStep = static_cast<f32>(1.0 / ticks);
    d.physCombine = 1;   // PHYSICS.COMBINE godot: friction min or max when rough, bounce a clamped sum
    d.physInterpolation = settingBool("physics/common/physics_interpolation", false) ? 1 : 0;
    report_.add("setting", kProject, "", "", "physics/3d/default_gravity", Rating::Converted, "PHYSICS.GRAVITY, converted to centimetres and Aver's axes", 0);
    report_.add("setting", kProject, "", "", "physics/common/physics_ticks_per_second", Rating::Converted, "PHYSICS.FIXEDSTEP", 0);
    report_.add("setting", kProject, "", "", "physics/common/physics_interpolation", Rating::Converted, "PHYSICS.INTERPOLATION", 0);
    report_.add("setting", kProject, "", "", "physics", Rating::Converted, "PHYSICS.COMBINE godot", 0);

    // The start scene's environment: bloom and exposure are project-wide in Aver.
    // Godot's exposure is fixed unless CameraAttributes asks for auto exposure, so eye adaptation is off; the tonemapper is Godot's.
    if (projectEnv_.found) {
        namespace ge = fmt::godotenv;
        d.postBloom = projectEnv_.glow ? static_cast<f32>(projectEnv_.glowIntensity) : 0.0f;
        const double exposure = ge::exposure(projectEnv_.tonemapExposure, projectEnv_.cameraExposure);
        if (exposure != 1.0) d.postExposure = static_cast<f32>(exposure);
        d.postAutoExposure = projectEnv_.autoExposure ? 1 : 0;
        d.postTonemap = ge::tonemapMode(projectEnv_.tonemapMode);
        d.postTonemapWhite = static_cast<f32>(projectEnv_.tonemapWhite);
        if (projectEnv_.adjust) {
            d.postAdjustBrightness = static_cast<f32>(projectEnv_.adjBrightness);
            d.postAdjustContrast = static_cast<f32>(projectEnv_.adjContrast);
            d.postAdjustSaturation = static_cast<f32>(projectEnv_.adjSaturation);
        }
        if (projectEnv_.volumetricFog) {
            d.volFog = 1;   // G4 R8: the light-shaft fog, where FogVolumes draw. The uniform medium stays 0: the level's fog already carries the density
            d.volFogDensity = 0.0f;
            report_.add("setting", kProject, "", "Environment", "volumetric_fog_enabled", Rating::Approximated,
                        "RENDER.VOLFOG 1 and RENDER.VOLFOGDENSITY 0: sun shafts through the level's fog, and FogVolume nodes add their own density", 0);
        }
        report_.add("setting", kProject, "", "Environment", "tonemap", Rating::Converted,
                    "RENDER.TONEMAP " + std::to_string(d.postTonemap) + " / TONEMAPWHITE / EXPOSURE / AUTOEXPOSURE / ADJUST* from the start scene's WorldEnvironment", 0);
    }

    // Comments the writer keeps verbatim: the layer names, which have no key of their own.
    std::string comments = "# Converted from a Godot " + (project_.engineVersion.empty() ? std::string("4") : project_.engineVersion) + " project by AverAssetC fabric.\n";
    comments += "# The conversion report is FabricReport.md next to this folder; the contract is docs/plans/fabric/G3_SCENES.md.\n";
    for (const auto& [key, val] : project_.layerNames) {
        // "3d_physics/layer_3" -> "FABRIC.LAYER 3d_physics 3 <name>"
        const size_t slash = key.find('/');
        const size_t us = key.rfind('_');
        if (slash == std::string::npos || us == std::string::npos || us < slash) continue;
        comments += "# FABRIC.LAYER " + key.substr(0, slash) + " " + key.substr(us + 1) + " " + val + "\n";
        report_.add("setting", kProject, "", "", "layer_names/" + key, Rating::Approximated,
                    "kept as a FABRIC.LAYER comment in the .ocproject and in Fabric/project.json; the engine has no layer names", 0);
    }
    const std::string text = fmt::writeOcproject(d, comments);
    createDirectories(projectDir_);
    const std::string path = projectDir_ + "/" + name_ + ".ocproject";
    if (!writeFileText(path, text)) {
        report_.add("project", kProject, "", "", "", Rating::Unsupported, "could not write " + path, 0);
        return false;
    }
    fmt::ProjectDesc back;
    std::string err;
    if (!fmt::loadOcproject(path, back, &err)) {
        report_.add("project", kProject, "", "", "", Rating::Unsupported, "the written .ocproject did not read back: " + err, 0);
        return false;
    }
    report_.add("project", kProject, "", "", "", Rating::Converted, "written as " + name_ + ".ocproject (start level " + (startLevelAsset.empty() ? "none" : startLevelAsset) + ")", 0);

    // Settings nobody converted, each named.
    static const char* handled[] = {"application/config/name", "application/config/description", "application/config/features",
                                    "application/config/icon", "application/run/main_scene", "display/window/size/", "physics/3d/default_gravity",
                                    "physics/3d/default_gravity_vector", "physics/3d/default_linear_damp", "physics/3d/default_angular_damp",
                                    "physics/common/physics_ticks_per_second", "physics/common/physics_interpolation", "internationalization/locale/",
                                    "input/", "autoload/", "layer_names/", "editor_plugins/", "dotnet/project/", "config_version"};
    for (const gd::GodotConfigSection& sec : project_.config.sections) {
        for (const gd::GodotProperty& e : sec.entries) {
            const std::string key = sec.name.empty() ? e.name : sec.name + "/" + e.name;
            bool ok = false;
            for (const char* h : handled)
                if (key.rfind(h, 0) == 0) { ok = true; break; }
            if (ok) continue;
            report_.add("setting", kProject, "", "", key, Rating::Unsupported, "no Aver project key for this setting", e.line);
        }
    }
    return true;
}

}  // namespace aver::fabric
