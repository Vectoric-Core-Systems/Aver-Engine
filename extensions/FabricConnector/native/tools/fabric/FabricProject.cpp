// project.godot to .ocproject, the [input] map to .ocinput, translations to Content/Localization.
#include "FabricInternal.hpp"

#include "aver/core/Version.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/godot/GodotEnvironment.hpp"
#include "aver/formats/Json.hpp"
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

// The "version" of <extension dir>/extension.json, or empty when the manifest cannot be read.
std::string manifestVersion(const std::string& extensionDir) {
    std::string text;
    fmt::JsonValue root;
    if (!readFileText(extensionDir + "/extension.json", text) || !fmt::parseJson(text, root)) return {};
    return std::string(root["version"].asString());
}

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

// Godot 3 wrote a special key (arrows, Enter, Tab, ...) as 1<<24 | n and Godot 4 writes 1<<22 | n; the low bits are the same.
// Old projects keep the old codes in their [input] section. Returns the Godot 4 code, or 0 when `code` is not a Godot 3 special key.
long long legacySpecialKey(long long code) {
    constexpr long long kOld = 1LL << 24, kNew = 1LL << 22;
    return (code >> 24) == 1 ? (code & (kOld - 1)) | kNew : 0;
}

// The names of Godot 4's special keys (1<<22 | n), for the report.
const char* godotSpecialKeyName(long long n) {
    static const char* const names[] = {"", "Escape", "Tab", "Backtab", "Backspace", "Enter", "KP Enter", "Insert", "Delete", "Pause", "Print", "SysReq",
                                        "Clear", "Home", "End", "Left", "Up", "Right", "Down", "PageUp", "PageDown", "Shift", "Ctrl", "Meta", "Alt",
                                        "CapsLock", "NumLock", "ScrollLock"};
    return n >= 1 && n < static_cast<long long>(sizeof(names) / sizeof(names[0])) ? names[n] : nullptr;
}

// Godot 3's joypad buttons, which the same old projects keep: the d-pad is 12 to 15 there and 11 to 14 in Godot 4, the shoulders 4 and 5,
// the triggers buttons 6 and 7 (axes in Godot 4), the sticks 8 and 9, Select 10, Start 11. Null for a trigger (see the caller).
const char* averPadButtonLegacy(long long idx) {
    switch (idx) {
        case 0: return "A";
        case 1: return "B";
        case 2: return "X";
        case 3: return "Y";
        case 4: return "LeftShoulder";
        case 5: return "RightShoulder";
        case 8: return "LeftThumb";
        case 9: return "RightThumb";
        case 10: return "Back";
        case 11: return "Start";
        case 12: return "DPadUp";
        case 13: return "DPadDown";
        case 14: return "DPadLeft";
        case 15: return "DPadRight";
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
    if (const char* n = averKeyName(code)) return n;
    if ((code >> 22) == 1)
        if (const char* s = godotSpecialKeyName(code & ((1LL << 22) - 1))) return s;
    if ((code >> 22) == 1 && (code & ((1LL << 22) - 1)) >= 28 && (code & ((1LL << 22) - 1)) < 28 + 35) return "F" + std::to_string((code & ((1LL << 22) - 1)) - 27);
    if (code > 32 && code < 127) return std::string("'") + static_cast<char>(code) + "'";
    return "keycode " + std::to_string(code);
}

const char* gamepadButtonName(long long idx, bool legacy) {
    if (legacy) return idx == 6 ? "L2 (a trigger)" : idx == 7 ? "R2 (a trigger)" : "unknown";
    switch (idx) {
        case 5: return "Guide";
        case 15: return "Misc 1";
        case 16: return "Paddle 1";
        case 17: return "Paddle 2";
        case 18: return "Paddle 3";
        case 19: return "Paddle 4";
        case 20: return "Touchpad";
        default: return "unknown";
    }
}

constexpr const char* kAverKeySet =
    "A-Z, 0-9, Space, Enter, Escape, Tab, the arrows, Shift, Ctrl, Alt and the three mouse buttons";

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
        int converted = 0, unmapped = 0, empty = 0;
        const auto skip = [&](const std::string& why, int line) {
            ++unmapped;
            report_.add("input", kProject, act.name, "InputEvent", "", Rating::Unsupported, why, line);
        };
        const auto keyCodeOf = [](const Var& ev) {
            long long code = static_cast<long long>(numOf(ev.find("keycode"), 0));
            if (code == 0) code = static_cast<long long>(numOf(ev.find("physical_keycode"), 0));
            if (code == 0) code = static_cast<long long>(numOf(ev.find("key_label"), 0));
            return code;
        };
        // An action that still holds Godot 3 key codes holds Godot 3 joypad numbers too (the old d-pad is 12 to 15).
        bool legacyPad = false;
        for (const Var& ev : act.events)
            if (ev.type == VType::Object && ev.s == "InputEventKey" && legacySpecialKey(keyCodeOf(ev)) != 0) legacyPad = true;
        for (const Var& ev : act.events) {
            if (ev.type != VType::Object) { skip("an event that is not an InputEvent object", act.line); continue; }
            const std::string& cls = ev.s;
            fmt::OcInputBinding b;
            b.action = act.name;
            if (cls == "InputEventKey") {
                long long code = keyCodeOf(ev);
                if (code == 0) {
                    ++empty;
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated,
                                "an empty key event (no keycode, physical keycode or key label) binds no key in Godot either; nothing is written for it", act.line);
                    continue;
                }
                const long long modern = legacySpecialKey(code);
                if (modern) code = modern;
                const char* n = averKeyName(code);
                if (!n) {
                    skip(std::string("the ") + keyDescription(code) + " key" + (modern ? " (in Godot 3's key encoding)" : "") + " is not in Aver's Key enum (" + kAverKeySet + ")", act.line);
                    continue;
                }
                if (modern)
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated,
                                "the key code is in Godot 3's encoding (special-key flag 1<<24, Godot 4 uses 1<<22); read as the " + keyDescription(code) + " key", act.line);
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
                const long long idx = static_cast<long long>(numOf(ev.find("button_index"), -1));
                if (legacyPad && (idx == 6 || idx == 7)) {   // Godot 3's L2 and R2 are buttons; Aver reads the triggers as axes
                    b.source = fmt::OcInputSource::GamepadAxis;
                    b.key = idx == 6 ? "LeftTrigger" : "RightTrigger";
                    axis = true;
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated,
                                "Godot 3 joypad numbering (this action holds Godot 3 key codes): the trigger button is bound as Aver's trigger axis", act.line);
                    ++converted;
                    binds.push_back(std::move(b));
                    continue;
                }
                const char* n = legacyPad ? averPadButtonLegacy(idx) : averPadButton(idx);
                if (!n) {
                    skip("gamepad button " + std::to_string(idx) + " (" + gamepadButtonName(idx, legacyPad) + ") is not in Aver's GamepadButton enum (A, B, X, Y, Back, Start, the two thumbs, the two shoulders, the d-pad)",
                         act.line);
                    continue;
                }
                if (legacyPad)
                    report_.add("input", kProject, act.name, cls, "", Rating::Approximated,
                                "Godot 3 joypad numbering (this action holds Godot 3 key codes): button " + std::to_string(idx) + " read as " + n, act.line);
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
            // Two events can name one Aver input (Enter and KP Enter): one binding is enough.
            bool duplicate = false;
            for (const fmt::OcInputBinding& o : binds)
                duplicate = duplicate || (o.source == b.source && o.key == b.key && o.scale == b.scale);
            ++converted;
            if (!duplicate) binds.push_back(std::move(b));
        }
        a.type = axis ? fmt::OcInputValueType::Axis1D : fmt::OcInputValueType::Digital;
        if (axis) {
            fmt::OcModifierSpec dz;
            dz.kind = fmt::OcModifierKind::DeadZone;
            dz.v[0] = static_cast<f32>(act.deadzone);
            dz.v[1] = 1.0f;
            a.modifiers.push_back(fmt::formatModifierSpec(dz));
        }
        const size_t written = binds.size();
        data.actions.push_back(std::move(a));
        for (auto& b : binds) data.bindings.push_back(std::move(b));
        if (converted > 0) report_.add("input", kProject, act.name, "InputAction", "", unmapped ? Rating::Approximated : Rating::Converted,
                                       std::to_string(written) + " binding(s)" + (unmapped ? ", " + std::to_string(unmapped) + " not mapped" : ""), act.line);
        else if (empty > 0 && unmapped == 0)
            report_.add("input", kProject, act.name, "InputAction", "", Rating::Approximated, "its only event is an empty key event; the action is written with no bindings", act.line);
        else report_.add("input", kProject, act.name, "InputAction", "", Rating::Unsupported,
                         "none of its events could be mapped (" + std::to_string(unmapped) + " event(s) are outside Aver's input set, see the rows above)", act.line);
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
    // The project's scripts and nodes run on this extension's managed runtime; opening it without the extension installed says so by name.
    d.extensions.push_back({"FabricConnector", manifestVersion(extensionDir())});
    if (!project_.inputActions.empty()) d.inputScheme = "Input/" + name_ + ".ocinput";
    d.localeDefault = settingString("internationalization/locale/fallback", "en");
    d.audioBusLayout = audioBusLayout_;   // G5 S1: the default AudioBusLayout converted by convertStandaloneResources, "" when the project has none

    // Window.
    d.windowTitle = project_.name;
    d.windowWidth = static_cast<int>(setting("display/window/size/viewport_width", 1152));
    d.windowHeight = static_cast<int>(setting("display/window/size/viewport_height", 648));
    d.windowResizable = settingBool("display/window/size/resizable", true) ? 1 : 0;
    const int mode = static_cast<int>(setting("display/window/size/mode", 0));
    d.windowFullscreen = (mode == 3 || mode == 4) ? 1 : 0;
    report_.add("setting", kProject, "", "", "display/window/size", Rating::Converted, "WINDOW.SIZE / RESIZABLE / FULLSCREEN", 0);
    convertProjectUi(d);   // G5 U1: stretch mode, aspect, scale, the project theme, the tooltip delay

    // Physics: Godot's rules, stated.
    const V3 gdir = vec3Of(project_.setting("physics/3d/default_gravity_vector"), {0, -1, 0});
    const double gmag = setting("physics/3d/default_gravity", 9.8);
    const Vec3 g = fmt::axis::position(static_cast<f32>(gdir.x * gmag), static_cast<f32>(gdir.y * gmag), static_cast<f32>(gdir.z * gmag));
    d.hasGravity = true;
    d.gravity[0] = g.x + 0.0f; d.gravity[1] = g.y + 0.0f; d.gravity[2] = g.z + 0.0f;   // + 0.0f: no negative zero in the file
    const double ticks = std::max(1.0, setting("physics/common/physics_ticks_per_second", 60));
    d.fixedStep = static_cast<f32>(1.0 / ticks);
    d.physCombine = 1;   // PHYSICS.COMBINE material: friction min or max when rough, bounce a clamped sum
    d.physInterpolation = settingBool("physics/common/physics_interpolation", false) ? 1 : 0;
    report_.add("setting", kProject, "", "", "physics/3d/default_gravity", Rating::Converted, "PHYSICS.GRAVITY, converted to centimetres and Aver's axes", 0);
    report_.add("setting", kProject, "", "", "physics/common/physics_ticks_per_second", Rating::Converted, "PHYSICS.FIXEDSTEP", 0);
    report_.add("setting", kProject, "", "", "physics/common/physics_interpolation", Rating::Converted, "PHYSICS.INTERPOLATION", 0);
    report_.add("setting", kProject, "", "", "physics", Rating::Converted, "PHYSICS.COMBINE material", 0);

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
                                    "application/config/icon", "application/run/main_scene", "display/window/size/", "display/window/stretch/",
                                    "gui/theme/custom", "gui/timers/tooltip_delay_sec", "physics/3d/default_gravity",
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
            if (key.rfind("shader_globals/", 0) == 0) {   // G6 SH2: a global shader uniform's default rides in each shader material's uniform table
                report_.add("setting", kProject, "", "", key, Rating::Approximated,
                            "the default is written into every shader material that declares this global uniform; Fabric/shaders.json lists the globals "
                            "and RenderingServer.global_shader_parameter_set updates them at run time", e.line);
                continue;
            }
            // Editor and import-time settings change nothing a running game does.
            static const char* editorOnly[] = {"application/config/tags", "debug/gdscript/warnings/", "debug/shader_language/warnings/",
                                               "rendering/textures/vram_compression/"};
            bool editorSetting = false;
            for (const char* h : editorOnly)
                if (key.rfind(h, 0) == 0) { editorSetting = true; break; }
            if (editorSetting) {
                report_.add("setting", kProject, "", "", key, Rating::Approximated,
                            "an editor or import-time setting (project tags, script warnings, texture compression formats); it changes nothing a running game does, so it is not written", e.line);
                continue;
            }
            report_.add("setting", kProject, "", "", key, Rating::Unsupported, "no Aver project key for this setting", e.line);
        }
    }
    return true;
}

}  // namespace aver::fabric
