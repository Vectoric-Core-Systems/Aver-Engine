#pragma once
// res:// and uid:// resolution against a Godot project folder. Contract: docs/plans/fabric/G3_FORMATS.md.
//
// Godot keeps its uid -> path table in the binary .godot/uid_cache.bin, which this reader does not
// parse (and which is often absent from a checked-out project). The table is rebuilt by scanning:
//   - `uid="uid://..."` in the header of every .tscn / .tres;
//   - `<file>.uid` sidecars (Godot 4.4+, one `uid://...` line each: foo.gd.uid -> res://foo.gd);
//   - `[remap] uid=` of every `<file>.import` (-> the source file res://<file>).
// Binary .scn/.res carry their uid in their header (GodotBinary.hpp reads it); they are also listed in
// binaryResources() so the converter can report each one.
#include "aver/formats/godot/GodotDiagnostics.hpp"
#include "aver/formats/godot/GodotProject.hpp"
#include "aver/formats/godot/GodotScene.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot {

class GodotUidMap {
public:
    // False when `uid` already maps to a different path (the first mapping is kept).
    bool add(const std::string& uid, const std::string& resPath);
    // The res:// path of a uid, or nullptr.
    const std::string* find(std::string_view uid) const;
    usize size() const { return map_.size(); }
    const std::map<std::string, std::string, std::less<>>& all() const { return map_; }

private:
    std::map<std::string, std::string, std::less<>> map_;
};

class GodotProjectFs {
public:
    // `rootDir` is the folder holding project.godot. A missing project.godot warns (the folder is
    // still usable as a res:// root). False when the folder does not exist.
    bool open(const std::string& rootDir, GodotDiagnostics& diag);
    const std::string& root() const { return root_; }

    static bool isResPath(std::string_view s) { return s.rfind("res://", 0) == 0; }
    static bool isUid(std::string_view s) { return s.rfind("uid://", 0) == 0; }
    // "res://a/./b/../c.png" -> "res://a/c.png"; backslashes become '/'. Empty when `s` is not a res://
    // path or climbs above the root.
    static std::string normalizeRes(std::string_view s);

    // A res:// path or a uid:// reference to the file on disk ('/' separators); empty if it cannot be
    // resolved (user://, an unknown uid, a path outside the project). Call scan() first for uids.
    std::string toDiskPath(std::string_view ref) const;
    // A disk path inside the project -> res://...; empty if outside.
    std::string toResPath(std::string_view diskPath) const;
    // Godot's rule for an [ext_resource]: the uid wins when it resolves, else the path. Returns the
    // res:// path or empty. A uid that resolves to a different file than `path` adds an Info diag.
    std::string resolve(std::string_view uid, std::string_view path, GodotDiagnostics* diag = nullptr,
                        std::string_view fromFile = {}, int line = 0) const;
    // Resolves an ext resource of a loaded document the same way.
    std::string resolve(const GodotExtResource& e, GodotDiagnostics* diag = nullptr, std::string_view fromFile = {}) const {
        return resolve(e.uid, e.path, diag, fromFile, e.line);
    }
    bool exists(std::string_view ref) const;

    // Walks the project (skipping dot-folders such as .godot and .git, and folders holding .gdignore),
    // lists every file and builds the uid map. Warns once per duplicate uid and per binary resource whose header cannot be read.
    bool scan(GodotDiagnostics& diag);
    const std::vector<std::string>& files() const { return files_; }          // res:// paths, sorted
    const std::vector<std::string>& binaryResources() const { return binary_; }  // .scn / .res
    const GodotUidMap& uids() const { return uids_; }
    GodotUidMap& uids() { return uids_; }

    // Resolve + read + parse. `ref` is a res:// path or a uid://. A binary .scn / .res is read through GodotBinary.hpp into the same model.
    bool loadScene(std::string_view ref, GodotScene& out, GodotDiagnostics& diag) const;
    bool loadResource(std::string_view ref, GodotResource& out, GodotDiagnostics& diag) const;
    bool loadImport(std::string_view sourceRef, GodotImport& out, GodotDiagnostics& diag) const;  // reads <ref>.import
    // Text only: a binary resource is an error.
    bool readText(std::string_view ref, std::string& out, GodotDiagnostics& diag) const;

private:
    bool readBytes(std::string_view ref, std::vector<u8>& out, std::string& res, GodotDiagnostics& diag) const;
    std::string root_;
    GodotUidMap uids_;
    std::vector<std::string> files_;
    std::vector<std::string> binary_;
};

}  // namespace aver::fmt::godot
