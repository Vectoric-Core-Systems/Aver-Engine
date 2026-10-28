// ConfigFile-style Godot files: project.godot, *.import, *.gdextension.
#include "aver/formats/godot/GodotProject.hpp"

#include "aver/platform/FileSystem.hpp"

#include <cstdlib>

namespace aver::fmt::godot {

// ---- config -----------------------------------------------------------------------------------------

const GodotConfigSection* GodotConfig::section(std::string_view name) const {
    for (const GodotConfigSection& s : sections)
        if (s.name == name) return &s;
    return nullptr;
}

const GodotVariant* GodotConfig::get(std::string_view sectionName, std::string_view key) const {
    for (const GodotConfigSection& s : sections) {
        if (s.name != sectionName) continue;
        for (const GodotProperty& p : s.entries)
            if (p.name == key) return &p.value;
    }
    return nullptr;
}

std::string GodotConfig::getString(std::string_view sectionName, std::string_view key, std::string_view def) const {
    const GodotVariant* v = get(sectionName, key);
    return v && v->isStringLike() ? v->s : std::string(def);
}

bool parseGodotConfig(std::string_view text, std::string_view file, GodotConfig& out, GodotDiagnostics& diag) {
    out = GodotConfig{};
    out.file = std::string(file);
    GodotSectionFile sf;
    const bool ok = parseGodotSections(text, file, sf, diag);
    for (GodotSection& s : sf.sections) {
        // A repeated [section] header continues the earlier one, as ConfigFile does.
        GodotConfigSection* dst = nullptr;
        for (GodotConfigSection& d : out.sections)
            if (d.name == s.tag.name) { dst = &d; break; }
        if (!dst) {
            out.sections.emplace_back();
            dst = &out.sections.back();
            dst->name = s.tag.name;
            dst->line = s.tag.line;
        }
        if (!s.tag.attrs.empty())
            diag.warn(file, s.tag.line, "attributes in the [" + s.tag.name + "] header are ignored");
        for (GodotProperty& p : s.props) dst->entries.push_back(std::move(p));
    }
    return ok;
}

bool loadGodotConfig(const std::string& path, GodotConfig& out, GodotDiagnostics& diag, std::string_view displayName) {
    std::string text;
    if (!readGodotTextFile(path, text, diag, displayName)) return false;
    return parseGodotConfig(text, displayName.empty() ? std::string_view(path) : displayName, out, diag);
}

namespace {

std::string scalarText(const GodotVariant* v) {
    if (!v) return {};
    if (v->isStringLike()) return v->s;
    if (v->type == GodotVType::Int) return std::to_string(v->i);
    if (v->type == GodotVType::Float) return godotVariantToText(*v);
    return {};
}

std::vector<std::string> stringArray(const GodotVariant* v) {
    std::vector<std::string> out;
    if (!v) return out;
    if (v->type == GodotVType::PackedStringArray) return v->strs;
    if (v->type == GodotVType::Array)
        for (const GodotVariant& e : v->items)
            if (e.isStringLike()) out.push_back(e.s);
    return out;
}

std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> out;
    usize start = 0;
    while (true) {
        const usize dot = s.find('.', start);
        if (dot == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return out;
}

}  // namespace

// ---- project.godot -----------------------------------------------------------------------------------

const GodotVariant* GodotProject::setting(std::string_view path) const {
    const usize slash = path.find('/');
    if (slash == std::string_view::npos) return config.get("", path);
    return config.get(path.substr(0, slash), path.substr(slash + 1));
}

bool GodotProject::hasFeature(std::string_view f) const {
    for (const std::string& s : features)
        if (s == f) return true;
    return false;
}

bool parseGodotProject(std::string_view text, std::string_view file, GodotProject& out, GodotDiagnostics& diag) {
    out = GodotProject{};
    const usize errorsBefore = diag.errorCount();
    parseGodotConfig(text, file, out.config, diag);

    if (const GodotVariant* cv = out.config.get("", "config_version")) out.configVersion = static_cast<int>(cv->asInt());
    if (out.configVersion == 0)
        diag.error(file, 0, "no config_version: this is not a project.godot", "godot.project");
    else if (out.configVersion < 5)
        diag.error(file, 0, "config_version=" + std::to_string(out.configVersion) + " is a Godot 3 project; the converter reads Godot 4.1+ projects", "godot.unsupported_version");
    else if (out.configVersion > 5)
        diag.warn(file, 0, "config_version=" + std::to_string(out.configVersion) + " is newer than the known version 5", "godot.project");

    out.name = scalarText(out.setting("application/config/name"));
    out.description = scalarText(out.setting("application/config/description"));
    out.mainScene = scalarText(out.setting("application/run/main_scene"));
    out.icon = scalarText(out.setting("application/config/icon"));
    out.features = stringArray(out.setting("application/config/features"));
    for (const std::string& f : out.features) {
        if (f == "C#") out.usesCSharp = true;
        if (out.engineVersion.empty() && !f.empty() && f[0] >= '0' && f[0] <= '9') out.engineVersion = f;
    }
    if (!out.engineVersion.empty()) {
        const int major = std::atoi(out.engineVersion.c_str());
        const usize dot = out.engineVersion.find('.');
        const int minor = dot == std::string::npos ? 0 : std::atoi(out.engineVersion.c_str() + dot + 1);
        if (major < 4 || (major == 4 && minor < 1))
            diag.warn(file, 0, "the project was last saved by Godot " + out.engineVersion + "; only 4.1+ is supported, so some data may not read", "godot.unsupported_version");
    }
    out.renderingMethod = scalarText(out.setting("rendering/renderer/rendering_method"));
    if (out.renderingMethod.empty()) {
        out.renderingMethod = "forward_plus";
        if (out.hasFeature("Mobile")) out.renderingMethod = "mobile";
        else if (out.hasFeature("GL Compatibility")) out.renderingMethod = "gl_compatibility";
    }

    if (const GodotConfigSection* al = out.config.section("autoload")) {
        int order = 0;
        for (const GodotProperty& p : al->entries) {
            GodotAutoload a;
            a.name = p.name;
            a.line = p.line;
            a.order = order++;
            std::string v = p.value.isStringLike() ? p.value.s : std::string();
            if (!v.empty() && v[0] == '*') {
                a.singleton = true;
                v.erase(0, 1);
            }
            a.path = v;
            if (a.path.empty()) diag.error(file, p.line, "autoload '" + p.name + "' has no path");
            out.autoloads.push_back(std::move(a));
        }
    }

    if (const GodotConfigSection* in = out.config.section("input")) {
        for (const GodotProperty& p : in->entries) {
            if (p.value.type != GodotVType::Dictionary) {
                diag.warn(file, p.line, "input action '" + p.name + "' is not a dictionary; ignored");
                continue;
            }
            GodotInputAction a;
            a.name = p.name;
            a.line = p.line;
            if (const GodotVariant* dz = p.value.find("deadzone")) a.deadzone = dz->asNumber(0.5);
            if (const GodotVariant* ev = p.value.find("events"); ev && ev->type == GodotVType::Array) a.events = ev->items;
            out.inputActions.push_back(std::move(a));
        }
    }

    out.enabledPlugins = stringArray(out.setting("editor_plugins/enabled"));

    if (const GodotConfigSection* ln = out.config.section("layer_names"))
        for (const GodotProperty& p : ln->entries)
            if (p.value.isStringLike()) out.layerNames.emplace_back(p.name, p.value.s);

    return diag.errorCount() == errorsBefore;
}

bool loadGodotProject(const std::string& path, GodotProject& out, GodotDiagnostics& diag) {
    std::string file = path;
    std::string dir = path;
    if (!aver::directoryExists(path)) {
        const usize cut = path.find_last_of("/\\");
        dir = cut == std::string::npos ? std::string(".") : path.substr(0, cut);
    } else {
        while (!file.empty() && (file.back() == '/' || file.back() == '\\')) file.pop_back();
        dir = file;
        file += "/project.godot";
    }
    std::string text;
    if (!readGodotTextFile(file, text, diag)) return false;
    const bool ok = parseGodotProject(text, file, out, diag);
    out.dir = dir;
    return ok;
}

// ---- *.import ----------------------------------------------------------------------------------------

const GodotVariant* GodotImport::param(std::string_view name) const {
    for (const GodotProperty& p : params)
        if (p.name == name) return &p.value;
    return nullptr;
}

bool parseGodotImport(std::string_view text, std::string_view file, GodotImport& out, GodotDiagnostics& diag) {
    out = GodotImport{};
    out.file = std::string(file);
    const usize errorsBefore = diag.errorCount();
    parseGodotConfig(text, file, out.config, diag);

    const GodotConfigSection* remap = out.config.section("remap");
    if (!remap) {
        diag.error(file, 0, "no [remap] section: not an .import file", "godot.import");
        return false;
    }
    for (const GodotProperty& p : remap->entries) {
        if (p.name == "importer") out.importer = scalarText(&p.value);
        else if (p.name == "type") out.type = scalarText(&p.value);
        else if (p.name == "uid") out.uid = scalarText(&p.value);
        else if (p.name == "valid") out.valid = p.value.asBool(true);
        else if (p.name == "path") out.path = scalarText(&p.value);
        else if (p.name == "metadata") out.metadata = p.value;
        else if (p.name.rfind("path.", 0) == 0) out.pathsByTag.emplace_back(p.name.substr(5), scalarText(&p.value));
    }
    if (out.importer.empty()) diag.error(file, remap->line, "[remap] has no importer", "godot.import");
    if (const GodotConfigSection* deps = out.config.section("deps")) {
        for (const GodotProperty& p : deps->entries) {
            if (p.name == "source_file") out.sourceFile = scalarText(&p.value);
            else if (p.name == "dest_files") out.destFiles = stringArray(&p.value);
        }
    }
    if (const GodotConfigSection* params = out.config.section("params")) out.params = params->entries;
    return diag.errorCount() == errorsBefore;
}

bool loadGodotImport(const std::string& path, GodotImport& out, GodotDiagnostics& diag, std::string_view displayName) {
    std::string text;
    if (!readGodotTextFile(path, text, diag, displayName)) return false;
    return parseGodotImport(text, displayName.empty() ? std::string_view(path) : displayName, out, diag);
}

// ---- *.gdextension ------------------------------------------------------------------------------------

const GodotGdLibrary* GodotGdExtension::selectLibrary(const std::vector<std::string>& features) const {
    for (const GodotGdLibrary& lib : libraries) {
        bool all = !lib.tags.empty();
        for (const std::string& t : lib.tags) {
            bool found = false;
            for (const std::string& f : features)
                if (f == t) { found = true; break; }
            if (!found) { all = false; break; }
        }
        if (all) return &lib;
    }
    return nullptr;
}

bool parseGodotGdExtension(std::string_view text, std::string_view file, GodotGdExtension& out, GodotDiagnostics& diag) {
    out = GodotGdExtension{};
    out.file = std::string(file);
    const usize errorsBefore = diag.errorCount();
    parseGodotConfig(text, file, out.config, diag);

    const GodotConfigSection* cfg = out.config.section("configuration");
    if (!cfg) {
        diag.error(file, 0, "no [configuration] section: not a .gdextension file", "godot.gdextension");
        return false;
    }
    for (const GodotProperty& p : cfg->entries) {
        if (p.name == "entry_symbol") out.entrySymbol = scalarText(&p.value);
        else if (p.name == "compatibility_minimum") out.compatibilityMinimum = scalarText(&p.value);
        else if (p.name == "compatibility_maximum") out.compatibilityMaximum = scalarText(&p.value);
        else if (p.name == "reloadable") out.reloadable = p.value.asBool(false);
    }
    if (out.entrySymbol.empty()) diag.error(file, cfg->line, "[configuration] has no entry_symbol", "godot.gdextension");
    if (out.compatibilityMinimum.empty()) diag.warn(file, cfg->line, "[configuration] has no compatibility_minimum", "godot.gdextension");

    if (const GodotConfigSection* libs = out.config.section("libraries")) {
        for (const GodotProperty& p : libs->entries) {
            if (!p.value.isStringLike()) {
                diag.warn(file, p.line, "library '" + p.name + "' is not a path; ignored");
                continue;
            }
            GodotGdLibrary l;
            l.key = p.name;
            l.tags = splitDots(p.name);
            l.path = p.value.s;
            l.line = p.line;
            out.libraries.push_back(std::move(l));
        }
    }
    if (out.libraries.empty()) diag.error(file, 0, "[libraries] lists no library", "godot.gdextension");

    if (const GodotConfigSection* icons = out.config.section("icons"))
        for (const GodotProperty& p : icons->entries)
            if (p.value.isStringLike()) out.icons.emplace_back(p.name, p.value.s);

    if (const GodotConfigSection* deps = out.config.section("dependencies")) {
        for (const GodotProperty& p : deps->entries) {
            GodotGdExtension::Dependency d;
            d.key = p.name;
            if (p.value.type == GodotVType::Dictionary) {
                for (usize k = 0; k < p.value.keys.size(); ++k)
                    if (p.value.keys[k].isStringLike())
                        d.files.emplace_back(p.value.keys[k].s, p.value.items[k].isStringLike() ? p.value.items[k].s : std::string());
            }
            out.dependencies.push_back(std::move(d));
        }
    }
    return diag.errorCount() == errorsBefore;
}

bool loadGodotGdExtension(const std::string& path, GodotGdExtension& out, GodotDiagnostics& diag, std::string_view displayName) {
    std::string text;
    if (!readGodotTextFile(path, text, diag, displayName)) return false;
    return parseGodotGdExtension(text, displayName.empty() ? std::string_view(path) : displayName, out, diag);
}

}  // namespace aver::fmt::godot
