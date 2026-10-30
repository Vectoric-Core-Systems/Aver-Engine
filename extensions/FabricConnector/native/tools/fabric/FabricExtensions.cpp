// GDExtension plugins of a Godot project: each .gdextension file and the libraries it names are copied into Content/ at the same
// res:// path (so Aver.Fabric's GDExtensionManager finds them where the file says), and the classes they register are remembered so a
// node of such a class is placed as a GDExtensionNode(3D). Contract: docs/plans/fabric/G3_GDEXT.md section 7.
#include "FabricInternal.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/godot/GodotProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace aver::fabric {

namespace {

namespace fsys = std::filesystem;

// The operating systems Aver runs on; a library for any other platform (android, ios, web) is of no use here and is not asked for.
bool isDesktopKey(const std::vector<std::string>& tags, std::string* os) {
    for (const std::string& t : tags)
        if (t == "windows" || t == "linux" || t == "macos") { if (os) *os = t; return true; }
    return false;
}

std::string hostOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// Copies a file or a folder (a macOS .framework is one) from the Godot project into Content, creating parents. Returns false if
// nothing was there to copy.
bool copyAsset(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (!fsys::exists(from, ec)) return false;
    fsys::create_directories(fsys::path(to).parent_path(), ec);
    // a .framework folder holds symlinks (Versions/Current): they are copied as symlinks
    if (fsys::is_directory(from, ec))
        fsys::copy(from, to, fsys::copy_options::recursive | fsys::copy_options::overwrite_existing | fsys::copy_options::copy_symlinks, ec);
    else fsys::copy_file(from, to, fsys::copy_options::overwrite_existing, ec);
    return !ec;
}

std::string contentPathOf(const std::string& res) { return res.rfind("res://", 0) == 0 ? res.substr(6) : res; }

}  // namespace

void Converter::convertExtensions() {
    extensions_.clear();
    std::map<std::string, bool> classes;
    setExtensionClasses(classes);

    std::vector<std::string> found;
    for (const std::string& res : fs_.files())
        if (res.size() > 12 && res.compare(res.size() - 12, 12, ".gdextension") == 0) found.push_back(res);
    if (found.empty()) return;

    for (const std::string& res : found) {
        const std::string disk = fs_.toDiskPath(res);
        gd::GodotGdExtension ext;
        gd::GodotDiagnostics diag;
        if (!gd::loadGodotGdExtension(disk, ext, diag, res)) {
            for (const gd::GodotDiag& x : diag.items)
                if (x.severity == gd::GodotSeverity::Error)
                    report_.add("file", res, "", "GDExtension", "", Rating::Unsupported, "the .gdextension file could not be read: " + x.message, x.line);
            continue;
        }
        ExtensionInfo info;
        info.resPath = res;
        info.contentPath = contentPathOf(res);
        info.entrySymbol = ext.entrySymbol;
        info.compatibilityMinimum = ext.compatibilityMinimum;
        info.compatibilityMaximum = ext.compatibilityMaximum;

        if (!options_.dryRun) copyAsset(disk, contentDir_ + "/" + info.contentPath);

        // Libraries for the desktop platforms, and what they need beside them.
        std::set<std::string> copied;
        const std::string os = hostOs();
        for (const gd::GodotGdLibrary& lib : ext.libraries) {
            std::string libOs;
            if (!isDesktopKey(lib.tags, &libOs)) continue;
            const std::string libDisk = fs_.toDiskPath(lib.path);
            std::error_code ec;
            const bool present = !libDisk.empty() && fsys::exists(libDisk, ec);
            if (!present) {
                info.missing.push_back(lib.key);
                report_.add("file", res, "", "GDExtensionLibrary", lib.key, Rating::Unsupported,
                            "the library " + lib.path + " is not in the project: build the plugin for " + libOs + " and add it, or this platform cannot load the extension",
                            lib.line);
                continue;
            }
            if (libOs == os) info.hostLibrary = true;
            if (copied.insert(lib.path).second) {
                if (!options_.dryRun) copyAsset(libDisk, contentDir_ + "/" + contentPathOf(lib.path));
                info.libraries.push_back(contentPathOf(lib.path));
            }
        }
        for (const gd::GodotGdExtension::Dependency& dep : ext.dependencies) {
            std::string depOs;
            std::vector<std::string> tags;
            for (size_t b = 0; b <= dep.key.size();) {
                size_t e = dep.key.find('.', b);
                if (e == std::string::npos) e = dep.key.size();
                tags.push_back(dep.key.substr(b, e - b));
                b = e + 1;
            }
            if (!isDesktopKey(tags, &depOs)) continue;
            for (const auto& [source, destination] : dep.files) {
                (void)destination;   // Godot copies a dependency to this folder beside the exported binary; here it keeps its res:// place
                const std::string srcDisk = fs_.toDiskPath(source);
                std::error_code ec;
                if (srcDisk.empty() || !fsys::exists(srcDisk, ec)) {
                    report_.add("file", res, "", "GDExtensionLibrary", "dependencies." + dep.key, Rating::Unsupported,
                                "the dependency " + source + " is not in the project", 0);
                    continue;
                }
                if (copied.insert(source).second) {
                    if (!options_.dryRun) copyAsset(srcDisk, contentDir_ + "/" + contentPathOf(source));
                    info.libraries.push_back(contentPathOf(source));
                }
            }
        }

        // The classes the plugin registers: its [icons] section names them (the editor shows them), the only class list a .gdextension holds.
        for (const auto& [className, icon] : ext.icons) {
            (void)icon;
            if (!chooseClass(className).carrier) continue;   // an icon for a class the runtime already has
            info.classes.push_back(className);
            classes[className] = false;
        }

        // Compatibility and the overall verdict.
        bool refused = false;
        const auto minor = [](const std::string& v) { return v.empty() ? -1 : std::atoi(v.substr(v.find('.') == std::string::npos ? 0 : v.find('.') + 1).c_str()); };
        if (ext.compatibilityMinimum.empty() || (ext.compatibilityMinimum.rfind("4.", 0) != 0) || minor(ext.compatibilityMinimum) < 1) {
            refused = true;
            report_.add("file", res, "", "GDExtension", "compatibility_minimum", Rating::Unsupported,
                        "the extension targets Godot " + (ext.compatibilityMinimum.empty() ? std::string("4.0 (no compatibility_minimum)") : ext.compatibilityMinimum) +
                            "; the host loads Godot 4.1 and later extensions",
                        0);
        }
        std::string verdict = "hosted by modules/fabric.gdext (Fabric GDExtension host, written from Godot's published interface and not yet run against a real godot-cpp plugin); " +
                              std::to_string(info.libraries.size()) + " library file(s) copied beside Content/" + info.contentPath + "; " +
                              std::to_string(info.classes.size()) + " class(es) named by [icons]";
        if (!info.hostLibrary) verdict += "; no library for " + os + " is in the project";
        report_.add("file", res, "", "GDExtension", "", refused || !info.hostLibrary ? Rating::Unsupported : Rating::Approximated, verdict, 0);
        for (const std::string& c : info.classes)
            report_.add("resource", res, "", c, "", Rating::Approximated,
                        "a class the plugin registers: nodes of it are placed as GDExtensionNode(3D) with extension_class; reached through Call/Get/Set by name", 0);
        extensions_.push_back(std::move(info));
    }

    // Which plugin classes are spatial: a scene that gives a node of the class a 3D transform says so.
    for (const std::string& res : fs_.files()) {
        if (res.size() < 5 || res.compare(res.size() - 5, 5, ".tscn") != 0) continue;
        const gd::GodotScene* doc = sceneDoc(res);
        if (!doc) continue;
        for (const gd::GodotNode& n : doc->nodes) {
            const auto it = classes.find(n.type);
            if (it == classes.end() || it->second) continue;
            for (const gd::GodotProperty& p : n.props) {
                const bool spatialValue = (p.name == "transform" && p.value.type == VType::Transform3D) ||
                                          ((p.name == "position" || p.name == "rotation" || p.name == "scale") && p.value.type == VType::Vector3) ||
                                          (p.name == "basis" && p.value.type == VType::Basis);
                if (spatialValue) { it->second = true; break; }
            }
        }
    }
    setExtensionClasses(classes);
}

}  // namespace aver::fabric
