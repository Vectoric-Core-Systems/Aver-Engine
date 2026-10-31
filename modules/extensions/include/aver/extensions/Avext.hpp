#pragma once
// .avext packages (docs/EXTENSIONS.md, "Installing"): a zip holding one extension's directory. Everything in this file is the part of installing
// that needs no editor and no UI, so a headless test drives it: reading a zip, reading and checking the manifest inside it before anything is
// copied, unpacking safely, and the replace and uninstall decisions. The Extensions window (sandbox/src/ExtensionsWindow.cpp) only asks and shows.
//
// A package is not trusted. The reader refuses what could write outside the extension's folder (absolute and drive paths, "..", backslashes,
// links), what lies about its sizes (a CRC-32 and the size are checked on every entry), what would blow up in memory (entry, size and ratio caps) and
// what it does not read (encryption, Zip64, methods but stored and deflate).
#include "aver/core/Types.hpp"
#include "aver/extensions/Extensions.hpp"

#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ext {

// ---- digests ----

// The SHA-256 of `size` bytes as 64 lower-case hex digits.
std::string sha256Hex(const void* data, usize size);
// The SHA-256 of a file, read in pieces. False with `why` when it cannot be read.
bool sha256FileHex(const std::string& path, std::string& hex, std::string* why = nullptr);
// CRC-32 (the zip polynomial) of `size` bytes, continuing from `seed` (0 to start).
u32 crc32(const void* data, usize size, u32 seed = 0);

// ---- names ----

// True when `name` is a safe zip entry name: relative, '/'-separated, no empty, "." or ".." segment, no backslash, drive colon, control character or
// character Windows cannot name, no segment ending in '.' or ' ' or naming a Windows device (CON, NUL, COM1...). A trailing '/' (a directory) is
// allowed. `why` says what is wrong.
bool isSafeEntryName(std::string_view name, std::string* why = nullptr);
// True when `name` can be an extension's name and so its directory: 1-64 of [A-Za-z0-9._-], starting with a letter or digit, no trailing '.', not a
// Windows device name.
bool isValidExtensionName(std::string_view name, std::string* why = nullptr);

// ---- the zip reader ----

struct ZipLimits {
    usize maxEntries = 4096;
    usize maxNameLength = 200;                  // the install path is <exe dir>/Extensions/<Name>/<entry>; Windows paths are short
    u64 maxEntrySize = 512ull << 20;            // one entry, unpacked
    u64 maxTotalSize = 2048ull << 20;           // all of them
};

struct ZipEntry {
    std::string name;                           // '/'-separated, as stored; a directory ends in '/'
    u16 method = 0;                             // 0 stored, 8 deflate
    u32 crc = 0;
    u64 compressedSize = 0;
    u64 size = 0;                               // unpacked
    u64 headerOffset = 0;                       // of the local header
    u32 unixMode = 0;                           // permission bits when the archive came from a Unix host, else 0
    bool directory = false;
};

class ZipReader {
public:
    // Opens `path` and reads its central directory, checking every entry's name, kind, sizes and limits. False with `why` for anything it will not
    // read; the reader is then closed.
    bool open(const std::string& path, std::string* why = nullptr, const ZipLimits& limits = {});
    void close();
    bool isOpen() const { return open_; }

    const std::vector<ZipEntry>& entries() const { return entries_; }
    // The entry named exactly `name` (case-sensitive), or null.
    const ZipEntry* find(std::string_view name) const;
    u64 archiveSize() const { return archiveSize_; }
    u64 unpackedSize() const { return total_; }

    // Unpacks one file entry: its local header must agree with the central directory, the data must lie inside the archive, and the unpacked bytes
    // must have the stored size and CRC-32.
    bool read(const ZipEntry& entry, std::vector<u8>& out, std::string* why = nullptr);

private:
    std::ifstream file_;
    bool open_ = false;
    u64 archiveSize_ = 0, directoryOffset_ = 0, total_ = 0;
    std::vector<ZipEntry> entries_;
};

// ---- packages ----

// What a package says about itself, read without copying anything.
struct PackageInfo {
    std::string path;                           // the .avext
    u64 fileSize = 0;
    std::string sha256;                         // of the whole file, for the person to compare with the release page
    Extension manifest;                         // the fields of the extension.json inside (status and directory unset)
    usize fileCount = 0;
    u64 unpackedSize = 0;
    std::vector<std::string> platforms;         // the native/<platform>/ folders it carries
    bool hasThisPlatform = false;               // native/<this platform>/ is there (or the manifest names no native library)
    std::string problem;                        // why this engine cannot install it; empty when it can
};

// Opens the package, reads its extension.json and checks the name, the ABI, the engine range and that it carries a native build for this platform.
// False with `why` when it is not a package at all (unreadable, not a zip, no extension.json, a manifest that does not parse, a hostile entry);
// otherwise `info` is filled and `info.problem` says whether it can be installed. `engineVersion` empty: the engine's own.
bool inspectPackage(const std::string& path, PackageInfo& info, std::string* why = nullptr, std::string_view engineVersion = {});

using TrashFn = std::function<bool(const std::string& path)>;

struct InstallResult {
    bool ok = false;
    bool installed = false;         // a folder of that name already exists (ok is then false unless Replace was chosen)
    bool replaced = false;
    std::string name;
    std::string dir;                // where it was (or would be) installed
    std::string message;            // one line for a person: what happened, or why not
};

struct InstallOptions {
    bool replace = false;                       // allowed to move an installed folder of the same name to the trash
    std::string engineVersion;                  // empty: the engine's own
    TrashFn moveToTrash;                        // required for replace; the platform trash, never a delete
};

// Installs the package into <extensionsRoot>/<Name>/. The package is inspected first; a folder of that name (any case) that exists is refused unless
// options.replace, and then it is moved to the trash, never deleted. The package is unpacked into <extensionsRoot>/.staging/<Name> (the loader skips
// dot folders), checked by the loader's own rules there (every file the manifest lists is present, this platform's library included), and only a
// package that passes is moved into place. On any failure nothing at <extensionsRoot>/<Name> has changed and the staging folder is removed.
InstallResult installPackage(const std::string& path, const std::string& extensionsRoot, const InstallOptions& options = {});

// Moves <extensionsRoot>/<dirName>/ to the trash. Refuses a name that is not a plain folder name, and a folder that is not there.
InstallResult uninstallExtension(const std::string& extensionsRoot, std::string_view dirName, const TrashFn& moveToTrash);

}  // namespace aver::ext
