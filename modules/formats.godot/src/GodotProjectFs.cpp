#include "aver/formats/godot/GodotProjectFs.hpp"

#include "aver/formats/godot/GodotBinary.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace aver::fmt::godot {

namespace fs = std::filesystem;

namespace {

std::string fwd(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string utf8Generic(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trimLine(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

// The uid of a binary resource, read from the first bytes of the file (the whole file only when a compressed one needs more). Empty when it has
// none; *why is set when the header cannot be read.
std::string binaryUid(const std::string& full, std::string& why) {
    aver::File f;
    if (!f.open(full, aver::File::Mode::Read)) {
        why = "cannot open the file";
        return {};
    }
    const i64 size = f.size();
    if (size <= 0) {
        why = "the file is empty";
        return {};
    }
    std::vector<u8> head(static_cast<usize>(std::min<i64>(size, 65536)));
    if (!f.readAt(0, head.data(), head.size())) {
        why = "cannot read the file";
        return {};
    }
    GodotBinaryHeader h;
    bool more = false;
    if (!readGodotBinaryHeader(head.data(), head.size(), static_cast<i64>(head.size()) == size, h, &why, &more)) {
        if (!more) return {};
        std::vector<u8> all;
        if (!aver::readFileBytes(full, all)) {
            why = "cannot read the file";
            return {};
        }
        why.clear();
        if (!readGodotBinaryHeader(all.data(), all.size(), true, h, &why)) return {};
    }
    return h.uid;
}

bool prefixEqual(const std::string& s, const std::string& prefix) {
#if defined(_WIN32)
    if (s.size() < prefix.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
#else
    return s.compare(0, prefix.size(), prefix) == 0;
#endif
}

// The uid="..." of a .tscn/.tres header (first line only).
std::string headerUid(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    char buf[4096];
    f.read(buf, sizeof buf);
    std::string_view head(buf, static_cast<usize>(f.gcount()));
    const usize nl = head.find('\n');
    if (nl != std::string_view::npos) head = head.substr(0, nl);
    if (detectGodotTextKind(head).empty()) return {};
    GodotDiagnostics scratch;
    GodotSectionFile sf;
    parseGodotSections(head, {}, sf, scratch);
    if (sf.sections.empty()) return {};
    return sf.sections[0].tag.attrString("uid");
}

}  // namespace

bool GodotUidMap::add(const std::string& uid, const std::string& resPath) {
    auto it = map_.find(uid);
    if (it == map_.end()) {
        map_.emplace(uid, resPath);
        return true;
    }
    return it->second == resPath;
}

const std::string* GodotUidMap::find(std::string_view uid) const {
    auto it = map_.find(uid);
    return it == map_.end() ? nullptr : &it->second;
}

bool GodotProjectFs::open(const std::string& rootDir, GodotDiagnostics& diag) {
    root_ = fwd(rootDir);
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    uids_ = GodotUidMap{};
    files_.clear();
    binary_.clear();
    if (!aver::directoryExists(root_)) {
        diag.error(root_, 0, "the project folder does not exist", "godot.project");
        return false;
    }
    if (!aver::fileExists(root_ + "/project.godot"))
        diag.warn(root_, 0, "no project.godot in this folder; using it as the res:// root anyway", "godot.project");
    return true;
}

std::string GodotProjectFs::normalizeRes(std::string_view s) {
    if (!isResPath(s)) return {};
    std::string rest = fwd(std::string(s.substr(6)));
    std::vector<std::string> parts;
    usize start = 0;
    while (start <= rest.size()) {
        usize end = rest.find('/', start);
        if (end == std::string::npos) end = rest.size();
        const std::string seg = rest.substr(start, end - start);
        if (seg.empty() || seg == ".") {
        } else if (seg == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else {
            parts.push_back(seg);
        }
        start = end + 1;
    }
    std::string out = "res://";
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

std::string GodotProjectFs::toDiskPath(std::string_view ref) const {
    std::string res;
    if (isUid(ref)) {
        const std::string* p = uids_.find(ref);
        if (!p) return {};
        res = *p;
    } else {
        res = normalizeRes(ref);
    }
    if (res.empty()) return {};
    const std::string rel = res.substr(6);
    return rel.empty() ? root_ : root_ + "/" + rel;
}

std::string GodotProjectFs::toResPath(std::string_view diskPath) const {
    const std::string p = fwd(std::string(diskPath));
    if (p.size() < root_.size() || !prefixEqual(p, root_)) return {};
    if (p.size() == root_.size()) return "res://";
    if (p[root_.size()] != '/') return {};
    return normalizeRes("res://" + p.substr(root_.size() + 1));
}

std::string GodotProjectFs::resolve(std::string_view uid, std::string_view path, GodotDiagnostics* diag,
                                    std::string_view fromFile, int line) const {
    std::string byPath;
    if (!path.empty()) {
        if (isUid(path)) {
            if (const std::string* p = uids_.find(path)) byPath = *p;
        } else {
            byPath = normalizeRes(path);
        }
    }
    if (!uid.empty()) {
        if (const std::string* p = uids_.find(uid)) {
            if (diag && !byPath.empty() && *p != byPath)
                diag->info(fromFile, line, "uid " + std::string(uid) + " now names " + *p + " (the file said " + byPath + ")", "godot.uid_moved");
            return *p;
        }
    }
    return byPath;
}

bool GodotProjectFs::exists(std::string_view ref) const {
    const std::string disk = toDiskPath(ref);
    return !disk.empty() && aver::fileExists(disk);
}

bool GodotProjectFs::scan(GodotDiagnostics& diag) {
    files_.clear();
    binary_.clear();
    uids_ = GodotUidMap{};
    std::error_code ec;
    const fs::path rootPath(std::u8string(reinterpret_cast<const char8_t*>(root_.data()), root_.size()));
    if (!fs::is_directory(rootPath, ec)) {
        diag.error(root_, 0, "the project folder does not exist", "godot.project");
        return false;
    }
    const usize rootLen = utf8Generic(rootPath).size();

    struct Pending {
        std::string uid, res;
    };
    std::vector<Pending> found;

    fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& de = *it;
        const std::string name = utf8Generic(de.path().filename());
        std::error_code e2;
        if (de.is_directory(e2)) {
            if ((!name.empty() && name[0] == '.') || fs::exists(de.path() / ".gdignore", e2)) it.disable_recursion_pending();
            continue;
        }
        if (!de.is_regular_file(e2)) continue;
        const std::string full = utf8Generic(de.path());
        if (full.size() <= rootLen + 1) continue;
        const std::string rel = full.substr(rootLen + 1);
        const std::string res = "res://" + rel;
        files_.push_back(res);

        if (endsWith(rel, ".uid")) {
            std::string text;
            if (aver::readFileText(full, text)) {
                const std::string uid = trimLine(text.substr(0, text.find('\n')));
                if (isUid(uid)) found.push_back({uid, "res://" + rel.substr(0, rel.size() - 4)});
            }
        } else if (endsWith(rel, ".import")) {
            std::string text;
            GodotDiagnostics scratch;
            GodotConfig cfg;
            if (aver::readFileText(full, text) && parseGodotConfig(text, {}, cfg, scratch)) {
                const std::string uid = cfg.getString("remap", "uid");
                if (isUid(uid)) found.push_back({uid, "res://" + rel.substr(0, rel.size() - 7)});
            }
        } else if (endsWith(rel, ".tscn") || endsWith(rel, ".tres")) {
            const std::string uid = headerUid(de.path());
            if (isUid(uid)) found.push_back({uid, res});
        } else if (endsWith(rel, ".scn") || endsWith(rel, ".res")) {
            binary_.push_back(res);
            std::string why;
            const std::string uid = binaryUid(full, why);
            if (!why.empty()) diag.warn(res, 0, "binary Godot resource that could not be read: " + why, "godot.binary_resource");
            if (isUid(uid)) found.push_back({uid, res});
        }
    }
    if (ec) diag.warn(root_, 0, "the folder walk stopped early: " + ec.message(), "godot.scan");

    std::sort(files_.begin(), files_.end());
    std::sort(binary_.begin(), binary_.end());
    // Sorted so the first path wins deterministically when two files claim a uid.
    std::sort(found.begin(), found.end(), [](const Pending& a, const Pending& b) { return a.res < b.res; });
    for (const Pending& f : found) {
        if (!uids_.add(f.uid, f.res))
            diag.warn(f.res, 0, "uid " + f.uid + " is also used by " + *uids_.find(f.uid) + "; the first one wins", "godot.uid_duplicate");
    }
    return true;
}

bool GodotProjectFs::readText(std::string_view ref, std::string& out, GodotDiagnostics& diag) const {
    const std::string disk = toDiskPath(ref);
    if (disk.empty()) {
        diag.error(ref, 0, "cannot resolve this reference inside the project", "godot.unresolved");
        return false;
    }
    std::string display = isUid(ref) ? std::string(ref) : normalizeRes(ref);
    if (isUid(ref)) {
        if (const std::string* p = uids_.find(ref)) display = *p;
    }
    return readGodotTextFile(disk, out, diag, display);
}

bool GodotProjectFs::readBytes(std::string_view ref, std::vector<u8>& out, std::string& res, GodotDiagnostics& diag) const {
    const std::string disk = toDiskPath(ref);
    if (disk.empty()) {
        diag.error(ref, 0, "cannot resolve this reference inside the project", "godot.unresolved");
        return false;
    }
    res = isUid(ref) ? *uids_.find(ref) : normalizeRes(ref);
    if (!aver::readFileBytes(disk, out)) {
        diag.error(res, 0, "cannot read the file", "godot.unreadable");
        return false;
    }
    return true;
}

bool GodotProjectFs::loadScene(std::string_view ref, GodotScene& out, GodotDiagnostics& diag) const {
    std::vector<u8> bytes;
    std::string res;
    if (!readBytes(ref, bytes, res, diag)) return false;
    if (detectGodotFileKind(bytes.data(), bytes.size()) == GodotFileKind::BinaryResource) return parseGodotBinaryScene(bytes.data(), bytes.size(), res, out, diag);
    std::string text;
    if (!godotTextFromBytes(bytes, res, text, diag)) return false;
    return parseGodotScene(text, res, out, diag);
}

bool GodotProjectFs::loadResource(std::string_view ref, GodotResource& out, GodotDiagnostics& diag) const {
    std::vector<u8> bytes;
    std::string res;
    if (!readBytes(ref, bytes, res, diag)) return false;
    if (detectGodotFileKind(bytes.data(), bytes.size()) == GodotFileKind::BinaryResource) return parseGodotBinaryResource(bytes.data(), bytes.size(), res, out, diag);
    std::string text;
    if (!godotTextFromBytes(bytes, res, text, diag)) return false;
    return parseGodotResource(text, res, out, diag);
}

bool GodotProjectFs::loadImport(std::string_view sourceRef, GodotImport& out, GodotDiagnostics& diag) const {
    std::string res = isUid(sourceRef) ? (uids_.find(sourceRef) ? *uids_.find(sourceRef) : std::string()) : normalizeRes(sourceRef);
    if (res.empty()) {
        diag.error(sourceRef, 0, "cannot resolve this reference inside the project", "godot.unresolved");
        return false;
    }
    res += ".import";
    const std::string disk = toDiskPath(res);
    return loadGodotImport(disk, out, diag, res);
}

}  // namespace aver::fmt::godot
