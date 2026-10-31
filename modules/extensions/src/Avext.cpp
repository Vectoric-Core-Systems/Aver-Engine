// .avext packages: a safe zip reader, the manifest check before anything is copied, unpacking into a staging folder, and the replace and uninstall
// decisions. See Avext.hpp.
#include "aver/extensions/Avext.hpp"

#include "aver/core/Version.hpp"
#include "aver/platform/Inflate.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <set>

namespace aver::ext {

namespace fsys = std::filesystem;

namespace {

fsys::path toPath(const std::string& utf8) { return fsys::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())); }
std::string toUtf8(const fsys::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

bool isDeviceName(std::string_view stem) {
    const std::string s = lower(stem);
    if (s == "con" || s == "prn" || s == "aux" || s == "nul") return true;
    return s.size() == 4 && (s.compare(0, 3, "com") == 0 || s.compare(0, 3, "lpt") == 0) && s[3] >= '1' && s[3] <= '9';
}

// ---- SHA-256 ----

constexpr u32 kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

class Sha256 {
public:
    void update(const u8* data, usize size) {
        length_ += size;
        while (size > 0) {
            const usize take = std::min<usize>(size, 64 - fill_);
            std::memcpy(block_ + fill_, data, take);
            fill_ += take;
            data += take;
            size -= take;
            if (fill_ == 64) { compress(block_); fill_ = 0; }
        }
    }

    std::string finish() {
        const u64 bits = length_ * 8;
        const u8 one = 0x80;
        update(&one, 1);
        const u8 zero = 0;
        while (fill_ != 56) update(&zero, 1);
        u8 tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = u8(bits >> (56 - 8 * i));
        update(tail, 8);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (const u32 word : h_)
            for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 15]);
        return out;
    }

private:
    static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const u8* p) {
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = (u32(p[4 * i]) << 24) | (u32(p[4 * i + 1]) << 16) | (u32(p[4 * i + 2]) << 8) | u32(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
            const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    u32 h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 block_[64] = {};
    usize fill_ = 0;
    u64 length_ = 0;
};

const std::array<u32, 256>& crcTable() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

constexpr u32 kEocdSig = 0x06054b50, kCentralSig = 0x02014b50, kLocalSig = 0x04034b50;
constexpr u64 kMaxDirectoryBytes = 16ull << 20;
constexpr u64 kMaxManifestBytes = 1ull << 20;

}  // namespace

// ---- digests ----

std::string sha256Hex(const void* data, usize size) {
    Sha256 s;
    s.update(static_cast<const u8*>(data), size);
    return s.finish();
}

bool sha256FileHex(const std::string& path, std::string& hex, std::string* why) {
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    Sha256 s;
    std::vector<u8> buf(1u << 20);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) s.update(buf.data(), usize(got));
    }
    if (f.bad()) { if (why) *why = "cannot read " + path; return false; }
    hex = s.finish();
    return true;
}

u32 crc32(const void* data, usize size, u32 seed) {
    const std::array<u32, 256>& t = crcTable();
    u32 c = ~seed;
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- names ----

bool isSafeEntryName(std::string_view name, std::string* why) {
    auto bad = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (name.empty()) return bad("an empty name");
    if (name.front() == '/') return bad("an absolute path");
    std::string_view body = name;
    if (body.back() == '/') body.remove_suffix(1);
    if (body.empty()) return bad("an empty name");
    for (const char c : body) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') return bad("a backslash");
        if (c == ':') return bad("a colon (a drive or a stream)");
        if (u < 0x20 || u == 0x7f) return bad("a control character");
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return bad("a character a Windows file name cannot hold");
    }
    for (usize at = 0; at <= body.size();) {
        usize end = body.find('/', at);
        if (end == std::string_view::npos) end = body.size();
        const std::string_view segment = body.substr(at, end - at);
        if (segment.empty()) return bad("an empty path segment");
        if (segment == "." || segment == "..") return bad("a . or .. path segment");
        if (segment.back() == '.' || segment.back() == ' ') return bad("a path segment ending in a dot or a space");
        if (isDeviceName(segment.substr(0, segment.find('.')))) return bad("a Windows device name");
        at = end + 1;
    }
    return true;
}

bool isValidExtensionName(std::string_view name, std::string* why) {
    auto bad = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (name.empty() || name.size() > 64) return bad("a name is 1 to 64 characters");
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) return bad("a name starts with a letter or a digit");
    for (const char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return bad("a name uses only letters, digits, '.', '_' and '-'");
    if (name.back() == '.') return bad("a name does not end in a dot");
    if (isDeviceName(name.substr(0, name.find('.')))) return bad("that name is reserved on Windows");
    return true;
}

// ---- the zip reader ----

void ZipReader::close() {
    if (file_.is_open()) file_.close();
    file_.clear();
    open_ = false;
    archiveSize_ = directoryOffset_ = total_ = 0;
    entries_.clear();
}

bool ZipReader::open(const std::string& path, std::string* why, const ZipLimits& limits) {
    close();
    auto fail = [&](std::string message) {
        if (why) *why = std::move(message);
        close();
        return false;
    };
    file_.open(toPath(path), std::ios::binary);
    if (!file_) return fail("cannot open " + path);
    file_.seekg(0, std::ios::end);
    const std::streamoff endPos = file_.tellg();
    if (endPos < 0) return fail("cannot read " + path);
    archiveSize_ = u64(endPos);
    if (archiveSize_ < 22) return fail("not a zip file (too small)");

    const u64 tailLen = std::min<u64>(archiveSize_, 22 + 65535);
    std::vector<u8> tail(static_cast<usize>(tailLen));
    file_.seekg(std::streamoff(archiveSize_ - tailLen));
    file_.read(reinterpret_cast<char*>(tail.data()), std::streamsize(tailLen));
    if (u64(file_.gcount()) != tailLen) return fail("cannot read " + path);
    i64 eocd = -1;
    for (i64 p = i64(tailLen) - 22; p >= 0; --p)
        if (rd32(&tail[usize(p)]) == kEocdSig && u64(p) + 22 + rd16(&tail[usize(p) + 20]) == tailLen) { eocd = p; break; }
    if (eocd < 0) return fail("not a zip file (no end-of-central-directory record)");
    const u8* e = &tail[usize(eocd)];
    const u32 count = rd16(e + 10), directorySize = rd32(e + 12), directoryOffset = rd32(e + 16);
    if (count == 0xFFFF || directorySize == 0xFFFFFFFFu || directoryOffset == 0xFFFFFFFFu) return fail("Zip64 archives are not supported");
    if (rd16(e + 4) != 0 || rd16(e + 6) != 0 || rd16(e + 8) != count) return fail("archives split over several files are not supported");
    if (count == 0) return fail("the archive is empty");
    if (count > limits.maxEntries) return fail("the archive has " + std::to_string(count) + " entries; at most " + std::to_string(limits.maxEntries) + " are read");
    const u64 eocdOffset = archiveSize_ - tailLen + u64(eocd);
    if (directorySize > kMaxDirectoryBytes || u64(directoryOffset) + directorySize > eocdOffset) return fail("the central directory lies outside the archive");
    directoryOffset_ = directoryOffset;

    std::vector<u8> dir(directorySize);
    file_.seekg(std::streamoff(directoryOffset));
    file_.read(reinterpret_cast<char*>(dir.data()), std::streamsize(directorySize));
    if (u64(file_.gcount()) != directorySize) return fail("cannot read the central directory");

    std::set<std::string> filesLower, anyLower;
    usize pos = 0;
    for (u32 i = 0; i < count; ++i) {
        if (pos + 46 > dir.size() || rd32(&dir[pos]) != kCentralSig) return fail("the central directory is damaged");
        const u8* h = &dir[pos];
        const u16 madeBy = rd16(h + 4), flags = rd16(h + 8), method = rd16(h + 10), nameLength = rd16(h + 28), extraLength = rd16(h + 30), commentLength = rd16(h + 32);
        const u32 externalAttributes = rd32(h + 38);
        ZipEntry entry;
        entry.method = method;
        entry.crc = rd32(h + 16);
        entry.compressedSize = rd32(h + 20);
        entry.size = rd32(h + 24);
        entry.headerOffset = rd32(h + 42);
        const usize next = pos + 46 + usize(nameLength) + extraLength + commentLength;
        if (next > dir.size()) return fail("the central directory is damaged");
        entry.name.assign(reinterpret_cast<const char*>(h + 46), nameLength);
        pos = next;

        const std::string label = "entry \"" + entry.name + "\"";
        if (entry.name.size() > limits.maxNameLength) return fail(label + " has a name longer than " + std::to_string(limits.maxNameLength) + " characters");
        std::string nameWhy;
        if (!isSafeEntryName(entry.name, &nameWhy)) return fail(label + " is refused: its name is " + nameWhy);
        entry.directory = entry.name.back() == '/';
        if (flags & 0x0001 || flags & 0x0040) return fail(label + " is encrypted");
        const u32 host = madeBy >> 8;
        if (host == 3) {
            const u32 mode = externalAttributes >> 16, type = mode & 0170000;
            if (type != 0 && type != 0100000 && type != 0040000) return fail(label + " is a link or a device file, which a package may not hold");
            entry.unixMode = mode & 07777;
        }
        if (externalAttributes & 0x400) return fail(label + " is a reparse point (a link), which a package may not hold");
        if (entry.directory) {
            if (entry.size != 0) return fail(label + " is a directory with data");
        } else {
            if (method != 0 && method != 8) return fail(label + " uses compression method " + std::to_string(method) + "; only stored and deflate are read");
            if (method == 0 && entry.compressedSize != entry.size) return fail(label + " is stored with two different sizes");
            if (entry.size > limits.maxEntrySize) return fail(label + " unpacks to more than " + std::to_string(limits.maxEntrySize >> 20) + " MiB");
            if (entry.compressedSize > limits.maxEntrySize + (limits.maxEntrySize >> 8) + 1024) return fail(label + " has an implausible compressed size");
            if (method == 8 && entry.size > entry.compressedSize * 1100 + 1024) return fail(label + " claims a compression ratio no deflate stream reaches");
            total_ += entry.size;
            if (total_ > limits.maxTotalSize) return fail("the package unpacks to more than " + std::to_string(limits.maxTotalSize >> 20) + " MiB");
        }
        if (entry.headerOffset + 30 > directoryOffset) return fail(label + " has a local header outside the archive");

        const std::string key = lower(entry.directory ? entry.name.substr(0, entry.name.size() - 1) : entry.name);
        if (!anyLower.insert(key).second) return fail(label + " appears twice (names that differ only in case count as the same)");
        if (!entry.directory) filesLower.insert(key);
        entries_.push_back(std::move(entry));
    }
    if (pos != dir.size()) return fail("the central directory is longer than its entries");

    // A file and a folder of one name, or an entry below a file.
    for (const ZipEntry& entry : entries_) {
        const std::string key = lower(entry.directory ? entry.name.substr(0, entry.name.size() - 1) : entry.name);
        for (usize slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
            if (filesLower.count(key.substr(0, slash))) return fail("entry \"" + entry.name + "\" lies below a file of the same path");
    }
    open_ = true;
    return true;
}

const ZipEntry* ZipReader::find(std::string_view name) const {
    for (const ZipEntry& e : entries_) if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::read(const ZipEntry& entry, std::vector<u8>& out, std::string* why) {
    auto fail = [&](const std::string& message) {
        if (why) *why = "entry \"" + entry.name + "\": " + message;
        out.clear();
        return false;
    };
    out.clear();
    if (!open_) return fail("the archive is not open");
    if (entry.directory) return fail("is a directory");
    if (entry.size == 0) return entry.crc == 0 ? true : fail("is empty but has a checksum");

    file_.clear();
    file_.seekg(std::streamoff(entry.headerOffset));
    u8 lh[30];
    file_.read(reinterpret_cast<char*>(lh), sizeof lh);
    if (file_.gcount() != 30 || rd32(lh) != kLocalSig) return fail("its local header is damaged");
    if (rd16(lh + 8) != entry.method) return fail("its local header disagrees with the central directory");
    const u16 nameLength = rd16(lh + 26), extraLength = rd16(lh + 28);
    std::string localName(nameLength, '\0');
    file_.read(localName.data(), nameLength);
    if (file_.gcount() != nameLength || localName != entry.name) return fail("its local header names a different file");
    const u64 dataStart = entry.headerOffset + 30 + nameLength + extraLength;
    if (dataStart + entry.compressedSize > directoryOffset_) return fail("its data lies outside the archive");

    std::vector<u8> packed(usize(entry.compressedSize));
    file_.seekg(std::streamoff(dataStart));
    file_.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()));
    if (u64(file_.gcount()) != packed.size()) return fail("is cut short");
    if (entry.method == 0) {
        out = std::move(packed);
    } else {
        out.resize(usize(entry.size));
        if (!inflateRaw(packed.data(), packed.size(), out.data(), out.size())) return fail("its deflate data is damaged");
    }
    if (crc32(out.data(), out.size()) != entry.crc) return fail("does not match its checksum");
    return true;
}

// ---- packages ----

bool inspectPackage(const std::string& path, PackageInfo& info, std::string* why, std::string_view engineVersion) {
    info = PackageInfo{};
    info.path = path;
    auto fail = [&](std::string message) {
        if (why) *why = std::move(message);
        return false;
    };
    ZipReader zip;
    std::string zipWhy;
    if (!zip.open(path, &zipWhy)) return fail(zipWhy);
    if (!sha256FileHex(path, info.sha256, &zipWhy)) return fail(zipWhy);
    info.fileSize = zip.archiveSize();

    const ZipEntry* manifestEntry = zip.find("extension.json");
    if (!manifestEntry || manifestEntry->directory) return fail("the package has no extension.json at its top level (is this an .avext?)");
    if (manifestEntry->size > kMaxManifestBytes) return fail("extension.json is larger than 1 MiB");
    std::vector<u8> bytes;
    if (!zip.read(*manifestEntry, bytes, &zipWhy)) return fail(zipWhy);
    std::string parseWhy;
    if (!parseExtensionManifest(std::string(bytes.begin(), bytes.end()), info.manifest, parseWhy)) return fail(parseWhy);
    info.manifest.dirName = info.manifest.name;

    std::set<std::string> platforms;
    for (const ZipEntry& e : zip.entries()) {
        if (!e.directory) ++info.fileCount;
        if (e.name.compare(0, 7, "native/") == 0) {
            const usize slash = e.name.find('/', 7);
            if (slash != std::string::npos && slash > 7) platforms.insert(e.name.substr(7, slash - 7));
        }
    }
    info.unpackedSize = zip.unpackedSize();
    info.platforms.assign(platforms.begin(), platforms.end());
    const Extension& m = info.manifest;
    info.hasThisPlatform = m.nativeName.empty() || zip.find(std::string("native/") + platformDirName() + "/" + nativeFileName(m.nativeName)) != nullptr;

    std::string nameWhy;
    if (!isValidExtensionName(m.name, &nameWhy)) {
        info.problem = m.name.empty() ? "extension.json has no name" : "the name \"" + m.name + "\" cannot be a folder name (" + nameWhy + ")";
    } else if (std::string incompatible = manifestIncompatibility(m, engineVersion.empty() ? kEngineVersion : engineVersion); !incompatible.empty()) {
        info.problem = std::move(incompatible);
    } else if (!info.hasThisPlatform) {
        std::string have;
        for (const std::string& p : info.platforms) have += (have.empty() ? "" : ", ") + p;
        info.problem = std::string("it has no build for ") + platformDirName() + (have.empty() ? " (it carries no native library)" : " (it carries " + have + ")");
    }
    return true;
}

namespace {

InstallResult failed(InstallResult r, std::string message) {
    r.ok = false;
    r.message = std::move(message);
    return r;
}

// The entry of `root` whose name equals `name` ignoring case, or empty.
std::string findFolderIgnoringCase(const fsys::path& root, const std::string& name) {
    std::error_code ec;
    for (fsys::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string leaf = toUtf8(it->path().filename());
        if (lower(leaf) == lower(name)) return leaf;
    }
    return {};
}

std::string installedVersionOf(const fsys::path& dir) {
    std::ifstream f(dir / "extension.json", std::ios::binary);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Extension e;
    std::string why;
    return parseExtensionManifest(text, e, why) ? e.version : std::string();
}

bool writeEntry(const fsys::path& dest, const std::vector<u8>& bytes) {
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    f.close();
    return !f.fail();
}

}  // namespace

InstallResult installPackage(const std::string& path, const std::string& extensionsRoot, const InstallOptions& options) {
    InstallResult result;
    const std::string engine = options.engineVersion.empty() ? std::string(kEngineVersion) : options.engineVersion;
    PackageInfo info;
    std::string why;
    if (!inspectPackage(path, info, &why, engine)) return failed(result, "Cannot read the package: " + why);
    const std::string& name = info.manifest.name;
    result.name = name;
    if (!info.problem.empty()) return failed(result, name + " cannot be installed: " + info.problem);

    const fsys::path root = toPath(extensionsRoot);
    result.dir = toUtf8(root / toPath(name));
    std::error_code ec;
    fsys::create_directories(root, ec);
    if (!fsys::is_directory(root, ec)) return failed(result, "Cannot create " + extensionsRoot);

    const std::string existingLeaf = findFolderIgnoringCase(root, name);
    fsys::path existing;
    if (!existingLeaf.empty()) {
        existing = root / toPath(existingLeaf);
        result.dir = toUtf8(existing);
        if (!fsys::is_directory(existing, ec)) return failed(result, "A file named " + existingLeaf + " is in the way in " + extensionsRoot);
        result.installed = true;
        if (!options.replace) {
            const std::string version = installedVersionOf(existing);
            return failed(result, name + (version.empty() ? "" : " " + version) + " is already installed. Choose Replace to install " + info.manifest.version +
                                      " over it; the installed folder moves to the Recycle Bin.");
        }
        if (!options.moveToTrash) return failed(result, "Cannot replace " + name + ": no trash is available to move the installed folder to.");
    }

    // Unpack into <root>/.staging/<name>: the loader skips dot folders, so a half-unpacked package is never an extension.
    const fsys::path staging = root / ".staging", stage = staging / toPath(name);
    auto cleanup = [&] {
        std::error_code cec;
        fsys::remove_all(stage, cec);
        fsys::remove(staging, cec);   // only when empty
    };
    if (fsys::is_symlink(fsys::symlink_status(staging, ec))) return failed(result, "Refusing to unpack: " + toUtf8(staging) + " is a link");
    fsys::create_directories(staging, ec);
    fsys::remove_all(stage, ec);
    fsys::create_directories(stage, ec);
    if (ec || !fsys::is_directory(stage)) return failed(result, "Cannot create " + toUtf8(stage));

    ZipReader zip;
    if (!zip.open(path, &why)) return failed(result, "Cannot read the package: " + why);
    std::vector<u8> bytes;
    for (const ZipEntry& entry : zip.entries()) {
        const std::string rel = entry.directory ? entry.name.substr(0, entry.name.size() - 1) : entry.name;
        const fsys::path dest = stage / toPath(rel);
        const fsys::path inside = dest.lexically_relative(stage);
        if (inside.empty() || *inside.begin() == "..") { cleanup(); return failed(result, "Refusing \"" + entry.name + "\": it would leave the extension's folder"); }
        if (entry.directory) {
            fsys::create_directories(dest, ec);
            if (ec) { cleanup(); return failed(result, "Cannot create " + toUtf8(dest)); }
            continue;
        }
        if (!zip.read(entry, bytes, &why)) { cleanup(); return failed(result, "The package is damaged: " + why); }
        fsys::create_directories(dest.parent_path(), ec);
        if (ec || !writeEntry(dest, bytes)) { cleanup(); return failed(result, "Cannot write " + toUtf8(dest)); }
#if !defined(_WIN32)
        const bool executable = (entry.unixMode & 0111) != 0 || entry.name.compare(0, 6, "tools/") == 0;
        fsys::permissions(dest, executable ? fsys::perms(0755) : fsys::perms(0644), fsys::perm_options::replace, ec);
#endif
    }
    zip.close();

    // The loader's own checks, on what was written: every listed file is there, this platform's library included.
    {
        Registry staged;
        staged.setEngineVersion(engine);
        staged.scan(toUtf8(staging));
        const Extension* e = staged.find(name);
        if (!e || !e->ready()) {
            const std::string reason = e ? e->reason : "it did not unpack";
            cleanup();
            return failed(result, name + " cannot be installed: " + reason);
        }
    }

    if (result.installed) {
        if (!options.moveToTrash(toUtf8(existing))) {
            cleanup();
            return failed(result, "Could not move the installed " + name + " to the Recycle Bin (is it in use? close the editor's scripts or the editor and try again). Nothing was changed.");
        }
        result.replaced = true;
    }
    fsys::rename(stage, root / toPath(name), ec);
    if (ec) {
        const std::string detail = ec.message();
        cleanup();
        return failed(result, "Could not move " + name + " into place (" + detail + ")." + (result.replaced ? " The previous version is in the Recycle Bin." : ""));
    }
    fsys::remove(staging, ec);
    result.dir = toUtf8(root / toPath(name));
    result.ok = true;
    result.message = std::string(result.replaced ? "Replaced " : "Installed ") + (info.manifest.displayName.empty() ? name : info.manifest.displayName) + " " + info.manifest.version;
    return result;
}

InstallResult uninstallExtension(const std::string& extensionsRoot, std::string_view dirName, const TrashFn& moveToTrash) {
    InstallResult result;
    result.name = std::string(dirName);
    const bool plain = !dirName.empty() && dirName != "." && dirName != ".." && dirName.front() != '.' && dirName.find_first_of("/\\:") == std::string_view::npos;
    if (!plain) return failed(result, "\"" + std::string(dirName) + "\" is not an extension folder name.");
    const fsys::path dir = toPath(extensionsRoot) / toPath(result.name);
    result.dir = toUtf8(dir);
    std::error_code ec;
    if (!fsys::is_directory(dir, ec)) return failed(result, result.name + " is not installed in " + extensionsRoot);
    if (!moveToTrash) return failed(result, "Cannot uninstall " + result.name + ": no trash is available to move it to.");
    if (!moveToTrash(result.dir)) return failed(result, "Could not move " + result.name + " to the Recycle Bin (is it in use? close the editor and try again). Nothing was changed.");
    result.ok = true;
    result.message = "Moved " + result.name + " to the Recycle Bin";
    return result;
}

}  // namespace aver::ext
