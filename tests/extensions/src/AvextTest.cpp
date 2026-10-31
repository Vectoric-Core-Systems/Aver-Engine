// AvextTest -- .avext packages (modules/extensions/src/Avext.cpp), headless: SHA-256 and CRC-32 against known answers; the zip reader on valid archives
// (stored, deflate with fixed and dynamic Huffman codes) and on hostile ones (zip-slip names, absolute and drive paths, backslashes, device names, links,
// encryption, Zip64, unknown methods, size and ratio lies, a wrong checksum, a local header that names another file, duplicates, a file under a file,
// damaged records); the manifest read from a package before anything is copied; install into a scratch Extensions folder (Replace, the trash, a refused
// package leaving nothing behind); uninstall.
//
// With `--avext <file> <exe dir>` it instead installs a real package into <exe dir>/Extensions and reports what the loader then says (the check that a
// package built by scripts/pack-avext.ps1 in the Fabric repository installs and is Ready).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/extensions/Avext.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace aver;
namespace fsys = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---- a zip writer that can lie ----

struct Spec {
    std::string name;
    std::vector<u8> data;
    u16 method = 0;                       // 0 stored, 8 deflate
    std::optional<std::vector<u8>> packed;   // the bytes to store instead of `data` (a deflate stream)
    std::optional<u32> crc;               // a checksum other than data's
    std::optional<u32> size;              // an unpacked size other than data's
    std::optional<u32> packedSize;        // a compressed size other than the stored bytes'
    u16 madeBy = 20;                      // version made by: host in the high byte (3: Unix)
    u16 flags = 0;
    u32 attributes = 0;                   // external attributes
    std::string localName;                // the name in the local header, when it should differ
};

static void put16(std::vector<u8>& o, u32 v) { o.push_back(u8(v)); o.push_back(u8(v >> 8)); }
static void put32(std::vector<u8>& o, u32 v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
static void putText(std::vector<u8>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); }

static Spec file(const std::string& name, const std::string& text) {
    Spec s;
    s.name = name;
    s.data.assign(text.begin(), text.end());
    return s;
}

// A deflate stream of one stored block: valid for any inflater, and it exercises the deflate path without a compressor.
static std::vector<u8> storedBlock(const std::vector<u8>& data) {
    std::vector<u8> r{0x01};
    put16(r, u32(data.size()));
    put16(r, ~u32(data.size()) & 0xFFFF);
    r.insert(r.end(), data.begin(), data.end());
    return r;
}

static Spec deflated(const std::string& name, const std::string& text) {
    Spec s = file(name, text);
    s.method = 8;
    s.packed = storedBlock(s.data);
    return s;
}

struct ZipOptions {
    bool zip64Marker = false;
    u32 eocdComment = 0;
    std::optional<u32> directoryOffset;   // a lie about where the central directory is
    std::optional<u16> entryCount;
};

static std::vector<u8> buildZip(const std::vector<Spec>& specs, const ZipOptions& opt = {}) {
    std::vector<u8> out;
    std::vector<u32> offsets;
    for (const Spec& s : specs) {
        offsets.push_back(u32(out.size()));
        const std::vector<u8>& bytes = s.packed ? *s.packed : s.data;
        const std::string& local = s.localName.empty() ? s.name : s.localName;
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, s.flags);
        put16(out, s.method);
        put32(out, 0);
        put32(out, s.crc ? *s.crc : ext::crc32(s.data.data(), s.data.size()));
        put32(out, s.packedSize ? *s.packedSize : u32(bytes.size()));
        put32(out, s.size ? *s.size : u32(s.data.size()));
        put16(out, u32(local.size()));
        put16(out, 0);
        putText(out, local);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    const u32 directoryStart = u32(out.size());
    for (usize i = 0; i < specs.size(); ++i) {
        const Spec& s = specs[i];
        const std::vector<u8>& bytes = s.packed ? *s.packed : s.data;
        put32(out, 0x02014b50);
        put16(out, s.madeBy);
        put16(out, 20);
        put16(out, s.flags);
        put16(out, s.method);
        put32(out, 0);
        put32(out, s.crc ? *s.crc : ext::crc32(s.data.data(), s.data.size()));
        put32(out, s.packedSize ? *s.packedSize : u32(bytes.size()));
        put32(out, s.size ? *s.size : u32(s.data.size()));
        put16(out, u32(s.name.size()));
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, s.attributes);
        put32(out, offsets[i]);
        putText(out, s.name);
    }
    const u32 directorySize = u32(out.size()) - directoryStart;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    const u16 count = opt.entryCount ? *opt.entryCount : (opt.zip64Marker ? u16(0xFFFF) : u16(specs.size()));
    put16(out, count);
    put16(out, count);
    put32(out, directorySize);
    put32(out, opt.directoryOffset ? *opt.directoryOffset : directoryStart);
    put16(out, opt.eocdComment);
    for (u32 i = 0; i < opt.eocdComment; ++i) out.push_back('c');
    return out;
}

// ---- scratch files ----

static std::string g_work;
static int g_counter = 0;

static std::string writeBytes(const std::string& name, const std::vector<u8>& bytes) {
    const std::string path = g_work + "/" + name;
    createDirectories(g_work);
    std::ofstream f(fsys::path(path), std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return path;
}

static std::string writeZip(const std::vector<Spec>& specs, const ZipOptions& opt = {}) {
    return writeBytes("pkg" + std::to_string(++g_counter) + ".avext", buildZip(specs, opt));
}

// Opens `path` with the reader and returns why it was refused ("" when it opened).
static std::string openWhy(const std::string& path, const ext::ZipLimits& limits = {}) {
    ext::ZipReader zip;
    std::string why;
    return zip.open(path, &why, limits) ? std::string() : (why.empty() ? std::string("(no reason)") : why);
}

static void expectRefused(const std::string& label, const std::vector<Spec>& specs, const std::string& reasonPart, const ZipOptions& opt = {}) {
    const std::string why = openWhy(writeZip(specs, opt));
    check(!why.empty() && contains(why, reasonPart), label + " is refused: " + why);
}

// ---- the packages the install tests use ----

static std::string manifestJson(const std::string& name, int abi, const std::string& engine, const std::string& extra = {}) {
    return "{\"name\":\"" + name + "\",\"displayName\":\"" + name + " Display\",\"version\":\"1.2.3\",\"abi\":" + std::to_string(abi) + ",\"engine\":\"" + engine +
           "\",\"license\":\"MIT\",\"source\":\"https://example.invalid/" + name + "\",\"native\":\"TestExt\",\"managed\":{\"runtime\":[\"Pkg.dll\"]},"
           "\"runtimeFiles\":[\"native/{platform}/TestExt.*\",\"managed/Pkg.dll\",\"data/x.json\"]" + extra + "}";
}

static std::vector<Spec> packageFiles(const std::string& name, int abi = AVER_EXT_ABI, const std::string& engine = ">=0.7.0 <2.0.0", const std::string& platform = ext::platformDirName()) {
    std::vector<Spec> s;
    s.push_back(file("extension.json", manifestJson(name, abi, engine)));
    s.push_back(file("native/" + platform + "/" + ext::nativeFileName("TestExt"), "fake native library"));
    s.push_back(deflated("managed/Pkg.dll", "fake managed assembly, stored in a deflate stream"));
    s.push_back(file("data/x.json", "{}"));
    s.push_back(file("tools/run.dll", "tool"));
    s.push_back(file("LICENSE", "MIT"));
    return s;
}

struct FakeTrash {
    std::string dir;
    int moved = 0;
    bool fail = false;
    bool operator()(const std::string& path) {
        if (fail) return false;
        std::error_code ec;
        fsys::create_directories(fsys::path(dir), ec);
        fsys::rename(fsys::path(path), fsys::path(dir) / ("item" + std::to_string(++moved)), ec);
        return !ec;
    }
};

static ext::InstallOptions optionsWith(FakeTrash& trash, bool replace) {
    ext::InstallOptions o;
    o.replace = replace;
    o.engineVersion = "1.0.0";
    o.moveToTrash = [&trash](const std::string& p) { return trash(p); };
    return o;
}

static bool dirExists(const std::string& p) { return fsys::is_directory(fsys::path(p)); }

static void testDigests() {
    check(ext::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of \"abc\"");
    check(ext::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    const std::string long56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(ext::sha256Hex(long56.data(), long56.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 across the padding boundary");
    const std::string million(1000000, 'a');
    check(ext::sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million 'a'");
    check(ext::crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    check(ext::crc32("56789", 5, ext::crc32("1234", 4)) == 0xCBF43926u, "CRC-32 continues from a seed");
    const std::string path = writeBytes("digest.bin", std::vector<u8>(million.begin(), million.end()));
    std::string hex;
    check(ext::sha256FileHex(path, hex) && hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a file");
    std::string why;
    check(!ext::sha256FileHex(g_work + "/nope.bin", hex, &why) && !why.empty(), "a missing file has no digest: " + why);
}

static void testNames() {
    const char* good[] = {"extension.json", "native/win-x64/Aver.Fabric.Native.dll", "tools/cs/Microsoft.CodeAnalysis.resources.dll", "data/", "a b/c d.txt", "LICENSE"};
    for (const char* n : good) check(ext::isSafeEntryName(n), std::string("safe name: ") + n);
    struct Bad { const char* name; const char* part; };
    const Bad bad[] = {{"", "empty"}, {"/", "absolute"}, {"/etc/passwd", "absolute"}, {"../evil", ".."}, {"a/../../evil", ".."}, {"a/./b", "."}, {"a//b", "empty"},
                       {"C:/evil", "colon"}, {"C:evil", "colon"}, {"a\\b", "backslash"}, {"..\\evil", "backslash"}, {"file.txt:stream", "colon"}, {"con", "device"},
                       {"native/NUL.txt", "device"}, {"COM3", "device"}, {"lpt1.dll", "device"}, {"trailing.", "dot"}, {"trailing /x", "space"}, {"a\nb", "control"},
                       {"wild*card", "Windows"}, {"pipe|name", "Windows"}};
    for (const Bad& b : bad) {
        std::string why;
        check(!ext::isSafeEntryName(b.name, &why) && contains(why, b.part), std::string("unsafe name \"") + b.name + "\": " + why);
    }
    check(ext::isValidExtensionName("FabricConnector") && ext::isValidExtensionName("a.b-c_d9") && !ext::isValidExtensionName("") && !ext::isValidExtensionName(".hidden") &&
              !ext::isValidExtensionName("a/b") && !ext::isValidExtensionName("..") && !ext::isValidExtensionName("a b") && !ext::isValidExtensionName("end.") &&
              !ext::isValidExtensionName("nul") && !ext::isValidExtensionName(std::string(65, 'a')),
          "extension names: letters, digits, . _ - only");
}

// The same 679-byte text, compressed by zlib into a fixed-Huffman stream (a sentence) and a dynamic-Huffman one (a shuffle of ten words).
static std::string sentenceText() {
    std::string t;
    for (int i = 0; i < 20; ++i) t += "the quick brown fox jumps over the lazy dog ";
    return t;
}
static std::string wordsText() {
    static const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa"};
    std::string t;
    for (u32 i = 0; i < 120; ++i) {
        if (i) t += ' ';
        t += words[(i * i * 7 + i / 3 + (i >> 2) * 5) % 10];
    }
    return t;
}

static void testReadValid() {
    const std::string sentence = sentenceText(), words = wordsText();
    Spec fixedHuffman = file("fixed.txt", sentence);
    fixedHuffman.method = 8;
    fixedHuffman.packed = std::vector<u8>{0x2b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56,
                                          0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a, 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0x83, 0x39, 0xa3, 0x6a, 0x47, 0xd5, 0x8e, 0xaa, 0x25, 0x5f, 0x2d, 0x00};
    Spec dynamicHuffman = file("dynamic.txt", words);
    dynamicHuffman.method = 8;
    dynamicHuffman.packed = std::vector<u8>{
        0x75, 0xd0, 0xd1, 0x0a, 0x80, 0x20, 0x0c, 0x05, 0xd0, 0x5f, 0xf1, 0xd7, 0x16, 0x49, 0x45, 0x56, 0x42, 0x3e, 0xf5, 0xf5, 0xe5, 0x6e, 0xe1, 0xd8, 0xb5, 0x97, 0xe1, 0x64, 0x3b, 0x5e, 0x94,
        0x94, 0x67, 0x09, 0x65, 0x8e, 0x45, 0xc2, 0x72, 0x3c, 0x25, 0xe6, 0x73, 0x49, 0xc7, 0x8e, 0x66, 0xa8, 0xd7, 0xab, 0xe4, 0x2c, 0x41, 0x74, 0xd0, 0xd6, 0x31, 0xa6, 0xf2, 0x9d, 0x01, 0x4c,
        0xb2, 0x6d, 0xef, 0xd2, 0x55, 0x4b, 0x87, 0xc5, 0x12, 0xb9, 0x3a, 0xce, 0x2c, 0x40, 0xcf, 0xa2, 0xef, 0xb0, 0x5d, 0x55, 0x0b, 0xa3, 0xc8, 0xe6, 0xd1, 0xd7, 0x21, 0xf5, 0xbb, 0xf1, 0xae,
        0x86, 0x60, 0xb6, 0xab, 0x22, 0x19, 0x9b, 0x4d, 0x6b, 0xa4, 0x59, 0x6e, 0xa2, 0x7d, 0xc7, 0x92, 0xf8, 0x09, 0x2f, 0x9a, 0xa8, 0x8d, 0x04, 0xe6, 0x49, 0xf4, 0x44, 0x0e, 0x3f, 0x26, 0xaa,
        0x47, 0x91, 0xcb, 0xa3, 0x3a, 0xe5, 0x4d, 0xf4, 0x24, 0x22, 0x00, 0x91, 0x75, 0xe5, 0x06};
    check(ext::crc32(words.data(), words.size()) == 477553385u && words.size() == 679, "the dynamic sample text matches the CRC zlib computed for it");
    Spec empty = file("empty.txt", "");
    Spec dirEntry = file("sub/", "");
    Spec stored = file("sub/stored.txt", "stored bytes");
    Spec blocks = deflated("sub/blocks.txt", "one stored deflate block");
    const std::string path = writeZip({file("a.txt", "alpha"), stored, dirEntry, blocks, fixedHuffman, dynamicHuffman, empty});

    ext::ZipReader zip;
    std::string why;
    check(zip.open(path, &why), "a valid archive opens: " + why);
    check(zip.entries().size() == 7, "seven entries");
    std::vector<u8> bytes;
    auto text = [&](const char* name) -> std::string {
        const ext::ZipEntry* e = zip.find(name);
        std::string w;
        if (!e || !zip.read(*e, bytes, &w)) return "<failed: " + w + ">";
        return std::string(bytes.begin(), bytes.end());
    };
    check(text("a.txt") == "alpha" && text("sub/stored.txt") == "stored bytes", "stored entries read back");
    check(text("sub/blocks.txt") == "one stored deflate block", "a deflate stored block reads back");
    check(text("fixed.txt") == sentence, "a fixed-Huffman deflate stream reads back");
    check(text("dynamic.txt") == words, "a dynamic-Huffman deflate stream reads back");
    check(text("empty.txt").empty() && zip.find("empty.txt") != nullptr, "an empty entry reads back empty");
    const ext::ZipEntry* dir = zip.find("sub/");
    check(dir && dir->directory, "a directory entry is a directory");
    check(zip.find("SUB/a.txt") == nullptr && zip.find("a.txt") != nullptr, "find is exact");
    check(zip.unpackedSize() == 5 + 12 + 24 + sentence.size() + words.size(), "unpacked size is the sum of the files");
    std::string w2;
    check(!zip.read(*dir, bytes, &w2), "a directory cannot be read as a file");

    ZipOptions withComment;
    withComment.eocdComment = 40;
    ext::ZipReader commented;
    check(commented.open(writeZip({file("a.txt", "alpha")}, withComment), &why), "an archive with an end comment opens: " + why);
}

static void testHostile() {
    expectRefused("a ../ name", {file("../evil.txt", "x")}, "name is a . or .. path segment");
    expectRefused("a nested ../ name", {file("a/../../evil.txt", "x")}, ".. path segment");
    expectRefused("an absolute name", {file("/etc/evil", "x")}, "absolute");
    expectRefused("a drive name", {file("C:/Windows/evil.dll", "x")}, "colon");
    expectRefused("a backslash name", {file("..\\evil.dll", "x")}, "backslash");
    expectRefused("a stream name", {file("a.txt:evil", "x")}, "colon");
    expectRefused("a device name", {file("native/aux.dll", "x")}, "device");
    expectRefused("a name with a control character", {file("a\tb", "x")}, "control");
    expectRefused("an empty name", {file("", "x")}, "empty");

    Spec link = file("link", "target");
    link.madeBy = u16((3 << 8) | 20);
    link.attributes = (0120777u << 16);
    expectRefused("a Unix symlink", {link}, "link");
    Spec device = file("dev", "");
    device.madeBy = u16((3 << 8) | 20);
    device.attributes = (0020666u << 16);
    expectRefused("a Unix device file", {device}, "device file");
    Spec reparse = file("junction", "x");
    reparse.attributes = 0x400;
    expectRefused("a Windows reparse point", {reparse}, "reparse");
    Spec regular = file("fine.sh", "x");
    regular.madeBy = u16((3 << 8) | 20);
    regular.attributes = (0100755u << 16);
    ext::ZipReader okZip;
    std::string why;
    check(okZip.open(writeZip({regular}), &why) && okZip.entries()[0].unixMode == 0755, "a regular Unix file keeps its mode: " + why);

    Spec encrypted = file("secret", "x");
    encrypted.flags = 1;
    expectRefused("an encrypted entry", {encrypted}, "encrypted");
    Spec bzip = file("bz", "x");
    bzip.method = 12;
    expectRefused("an unknown method", {bzip}, "method 12");
    expectRefused("a Zip64 marker", {file("a", "x")}, "Zip64", ZipOptions{true, 0, {}, {}});

    Spec lyingStored = file("liar", "ten bytes!");
    lyingStored.packedSize = 4;
    expectRefused("a stored entry with two sizes", {lyingStored}, "two different sizes");
    Spec huge = deflated("huge", "x");
    huge.size = 0x7FFFFFF0u;
    expectRefused("an entry claiming more than the cap", {huge}, "more than");
    Spec bomb = deflated("bomb", "x");
    bomb.size = 200u << 20;
    expectRefused("a ratio no deflate stream reaches", {bomb}, "ratio");
    Spec directoryWithData = file("dir/", "data");
    expectRefused("a directory with data", {directoryWithData}, "directory with data");

    expectRefused("two entries of one name", {file("a.txt", "1"), file("a.txt", "2")}, "twice");
    expectRefused("names that differ only in case", {file("Data/x.json", "1"), file("data/X.json", "2")}, "twice");
    expectRefused("an entry below a file", {file("a", "1"), file("a/b", "2")}, "below a file");

    // Damage to the records
    const std::string badOffset = openWhy(writeZip({file("a", "x")}, ZipOptions{false, 0, 100000u, {}}));
    check(contains(badOffset, "outside the archive"), "a central directory offset past the end is refused: " + badOffset);
    const std::string moreEntries = openWhy(writeZip({file("a", "x")}, ZipOptions{false, 0, {}, u16(5)}));
    check(!moreEntries.empty(), "a count that does not match the directory is refused: " + moreEntries);
    check(!openWhy(writeBytes("garbage.avext", std::vector<u8>(300, 0x41))).empty(), "garbage is not a zip");
    check(contains(openWhy(writeBytes("tiny.avext", std::vector<u8>{'P', 'K'})), "too small"), "a two-byte file is not a zip");
    check(!openWhy(g_work + "/missing.avext").empty(), "a missing file does not open");
    std::vector<u8> truncated = buildZip({file("a", "alpha"), file("b", "beta")});
    truncated.resize(truncated.size() - 30);
    check(!openWhy(writeBytes("truncated.avext", truncated)).empty(), "a truncated archive is refused");
    std::vector<u8> brokenSignature = buildZip({file("a", "alpha")});
    for (usize i = 0; i + 4 <= brokenSignature.size(); ++i)
        if (brokenSignature[i] == 'P' && brokenSignature[i + 1] == 'K' && brokenSignature[i + 2] == 1 && brokenSignature[i + 3] == 2) brokenSignature[i + 2] = 9;
    check(contains(openWhy(writeBytes("brokensig.avext", brokenSignature)), "damaged"), "a damaged central record is refused");

    ext::ZipLimits tight;
    tight.maxEntries = 2;
    check(contains(openWhy(writeZip({file("a", "1"), file("b", "2"), file("c", "3")}), tight), "entries"), "too many entries");
    tight = {};
    tight.maxTotalSize = 10;
    check(contains(openWhy(writeZip({file("a", "123456"), file("b", "123456")}), tight), "more than"), "the total size cap");
    tight = {};
    tight.maxNameLength = 8;
    check(contains(openWhy(writeZip({file("a/very/long/name.txt", "1")}), tight), "name longer"), "the name length cap");

    // Damage found when an entry is read
    auto readWhy = [&](const std::vector<Spec>& specs, const char* entryName) {
        ext::ZipReader zip;
        std::string w;
        if (!zip.open(writeZip(specs), &w)) return "(open) " + w;
        std::vector<u8> bytes;
        const ext::ZipEntry* e = zip.find(entryName);
        return e && zip.read(*e, bytes, &w) ? std::string() : (w.empty() ? std::string("(no entry)") : w);
    };
    Spec badCrc = file("crc.txt", "content");
    badCrc.crc = 0x12345678;
    check(contains(readWhy({badCrc}, "crc.txt"), "checksum"), "a wrong CRC-32 is caught on read");
    Spec shortStream = deflated("short.txt", "twelve bytes");
    shortStream.size = 40;
    check(!readWhy({shortStream}, "short.txt").empty(), "a deflate stream shorter than its stated size is caught on read");
    Spec longStream = deflated("long.txt", "twelve bytes");
    longStream.size = 5;
    check(!readWhy({longStream}, "long.txt").empty(), "a deflate stream longer than its stated size is caught on read");
    Spec junkStream = file("junk.txt", "0123456789");
    junkStream.method = 8;
    junkStream.packed = std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    check(!readWhy({junkStream}, "junk.txt").empty(), "a junk deflate stream is caught on read");
    Spec wrongLocalName = file("good.txt", "content");
    wrongLocalName.localName = "other.txt";
    check(contains(readWhy({wrongLocalName}, "good.txt"), "different file"), "a local header naming another file is caught on read");
    Spec wrongPacked = file("p.txt", "content");
    wrongPacked.packedSize = 3000;   // stored: refused at open (two sizes)
    check(!readWhy({wrongPacked}, "p.txt").empty(), "a data size that runs past the archive is refused");
    Spec deflateTooBig = deflated("q.txt", "content");
    deflateTooBig.packedSize = 100000;
    check(!readWhy({deflateTooBig}, "q.txt").empty(), "a compressed size that runs past the archive is caught");
}

static void testInspect() {
    ext::PackageInfo info;
    std::string why;
    check(ext::inspectPackage(writeZip(packageFiles("PkgExt")), info, &why, "1.0.0"), "a package is inspected: " + why);
    check(info.problem.empty() && info.manifest.name == "PkgExt" && info.manifest.version == "1.2.3" && info.manifest.displayName == "PkgExt Display" &&
              info.manifest.license == "MIT" && info.manifest.source == "https://example.invalid/PkgExt" && info.manifest.abi == AVER_EXT_ABI,
          "its manifest fields are read before anything is copied: " + info.problem);
    check(info.sha256.size() == 64 && info.hasThisPlatform && info.platforms.size() == 1 && info.platforms[0] == ext::platformDirName() && info.fileCount == 6 && info.fileSize > 0,
          "digest, platform and counts");
    std::string fileHash;
    check(ext::sha256FileHex(info.path, fileHash) && fileHash == info.sha256, "its digest is the file's SHA-256");

    check(ext::inspectPackage(writeZip(packageFiles("PkgExt", AVER_EXT_ABI + 5)), info, &why, "1.0.0") && contains(info.problem, "ABI"), "another ABI is a problem: " + info.problem);
    check(ext::inspectPackage(writeZip(packageFiles("PkgExt", 1)), info, &why, "1.0.0") && contains(info.problem, "no longer loads"), "an old ABI says to rebuild: " + info.problem);
    check(ext::inspectPackage(writeZip(packageFiles("PkgExt", AVER_EXT_ABI, ">=2.0.0")), info, &why, "1.0.0") && contains(info.problem, "needs engine"), "an engine out of range is a problem: " + info.problem);
    check(ext::inspectPackage(writeZip(packageFiles("PkgExt", AVER_EXT_ABI, ">=0.7.0 <2.0.0", "plan9-mips")), info, &why, "1.0.0") && contains(info.problem, "no build for") &&
              contains(info.problem, "plan9-mips") && !info.hasThisPlatform,
          "a package without this platform's library says what it has: " + info.problem);
    check(ext::inspectPackage(writeZip(packageFiles("../evil")), info, &why, "1.0.0") && contains(info.problem, "cannot be a folder name"), "a name that is a path is a problem: " + info.problem);
    check(ext::inspectPackage(writeZip(packageFiles("")), info, &why, "1.0.0") && contains(info.problem, "no name"), "no name is a problem: " + info.problem);

    check(!ext::inspectPackage(writeZip({file("readme.txt", "no manifest")}), info, &why) && contains(why, "no extension.json"), "a zip without extension.json is not a package: " + why);
    check(!ext::inspectPackage(writeZip({file("extension.json", "{not json")}), info, &why) && contains(why, "JSON"), "a manifest that does not parse is not a package: " + why);
    check(!ext::inspectPackage(writeZip({file("sub/extension.json", "{}")}), info, &why), "a manifest below the top level does not count");
    check(!ext::inspectPackage(g_work + "/missing.avext", info, &why) && !why.empty(), "a missing file is not a package: " + why);
    check(!ext::inspectPackage(writeZip({file("extension.json", "{}"), file("../evil", "x")}), info, &why) && contains(why, ".."), "a hostile entry fails inspection: " + why);
}

static void testInstall() {
    const std::string root = g_work + "/exe1/Extensions";
    FakeTrash trash{g_work + "/trash1"};
    const std::string pkg = writeZip(packageFiles("PkgExt"));

    ext::InstallResult r = ext::installPackage(pkg, root, optionsWith(trash, false));
    check(r.ok && !r.replaced && !r.installed && r.name == "PkgExt" && contains(r.message, "Installed"), "installs into an empty Extensions folder: " + r.message);
    check(dirExists(root + "/PkgExt") && fileExists(root + "/PkgExt/extension.json") && fileExists(root + "/PkgExt/managed/Pkg.dll") && fileExists(root + "/PkgExt/tools/run.dll") &&
              fileExists(root + "/PkgExt/native/" + ext::platformDirName() + "/" + ext::nativeFileName("TestExt")),
          "the files are unpacked");
    std::string text;
    check(readFileText(root + "/PkgExt/managed/Pkg.dll", text) && text == "fake managed assembly, stored in a deflate stream", "a deflated file unpacks to its bytes");
    check(!dirExists(root + "/.staging"), "the staging folder is gone");
    {
        ext::Registry reg;
        reg.setEngineVersion("1.0.0");
        reg.scan(root);
        const ext::Extension* e = reg.find("PkgExt");
        check(reg.extensions().size() == 1 && e && e->ready(), "the loader lists it Ready" + std::string(e ? ": " + e->reason : ""));
    }

    r = ext::installPackage(pkg, root, optionsWith(trash, false));
    check(!r.ok && r.installed && contains(r.message, "already installed") && contains(r.message, "1.2.3") && contains(r.message, "Replace"), "a name that is installed is refused without Replace: " + r.message);
    check(trash.moved == 0 && dirExists(root + "/PkgExt"), "and nothing moved");

    trash.fail = true;
    r = ext::installPackage(pkg, root, optionsWith(trash, true));
    check(!r.ok && contains(r.message, "Nothing was changed") && dirExists(root + "/PkgExt") && !dirExists(root + "/.staging"), "Replace with a trash that fails changes nothing: " + r.message);
    trash.fail = false;

    // A marker proves the old folder is the one moved to the trash.
    check(writeFileText(root + "/PkgExt/old-marker.txt", "old"), "marker written");
    r = ext::installPackage(pkg, root, optionsWith(trash, true));
    check(r.ok && r.replaced && contains(r.message, "Replaced"), "Replace installs over it: " + r.message);
    check(trash.moved == 1 && fileExists(g_work + "/trash1/item1/old-marker.txt") && !fileExists(root + "/PkgExt/old-marker.txt") && fileExists(root + "/PkgExt/extension.json"),
          "the old folder went to the trash, not away");

    ext::InstallOptions noTrash = optionsWith(trash, true);
    noTrash.moveToTrash = nullptr;
    r = ext::installPackage(pkg, root, noTrash);
    check(!r.ok && contains(r.message, "no trash"), "Replace without a trash is refused: " + r.message);

    // A package that passes the manifest but not the loader (it lists a file it does not carry) leaves nothing behind.
    std::vector<Spec> missingFile = packageFiles("BrokenExt");
    missingFile.erase(missingFile.begin() + 3);   // data/x.json
    r = ext::installPackage(writeZip(missingFile), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "data/x.json") && !dirExists(root + "/BrokenExt") && !dirExists(root + "/.staging"), "a package missing a listed file is refused and leaves nothing: " + r.message);

    r = ext::installPackage(writeZip(packageFiles("OldAbi", 1)), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "no longer loads") && !dirExists(root + "/OldAbi"), "an old ABI is refused before anything is copied: " + r.message);
    r = ext::installPackage(writeZip(packageFiles("OtherOs", AVER_EXT_ABI, ">=0.7.0 <2.0.0", "linux-riscv64")), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "no build for") && !dirExists(root + "/OtherOs") && !dirExists(root + "/.staging"), "a package for other platforms is refused: " + r.message);

    // Hostile packages
    std::vector<Spec> slip = packageFiles("SlipExt");
    slip.push_back(file("../escaped.txt", "x"));
    r = ext::installPackage(writeZip(slip), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "..") && !dirExists(root + "/SlipExt") && !fileExists(g_work + "/exe1/escaped.txt") && !fileExists(root + "/escaped.txt"), "a zip-slip package writes nothing: " + r.message);
    std::vector<Spec> bad = packageFiles("CrcExt");
    bad[2].crc = 1;
    r = ext::installPackage(writeZip(bad), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "damaged") && !dirExists(root + "/CrcExt") && !dirExists(root + "/.staging"), "a package with a bad checksum is refused and cleaned up: " + r.message);
    std::vector<Spec> links = packageFiles("LinkExt");
    links.push_back(file("tools/link", "target"));
    links.back().madeBy = u16((3 << 8) | 20);
    links.back().attributes = 0120777u << 16;
    r = ext::installPackage(writeZip(links), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "link") && !dirExists(root + "/LinkExt"), "a package holding a symlink is refused: " + r.message);
    r = ext::installPackage(g_work + "/not-there.avext", root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "Cannot read"), "a missing package is refused: " + r.message);
    r = ext::installPackage(writeBytes("notzip.avext", std::vector<u8>(100, 7)), root, optionsWith(trash, false));
    check(!r.ok && contains(r.message, "Cannot read"), "a file that is not a zip is refused: " + r.message);

    // A file where the folder should be; and a folder that differs only in case
    createDirectories(g_work + "/exe2/Extensions");
    check(writeFileText(g_work + "/exe2/Extensions/PkgExt", "a file"), "file in the way written");
    r = ext::installPackage(pkg, g_work + "/exe2/Extensions", optionsWith(trash, false));
    check(!r.ok && contains(r.message, "in the way"), "a file named like the extension is in the way: " + r.message);
    createDirectories(g_work + "/exe3/Extensions/pkgext");
    r = ext::installPackage(pkg, g_work + "/exe3/Extensions", optionsWith(trash, false));
    check(!r.ok && r.installed && contains(r.message, "already installed"), "a folder that differs only in case counts as installed: " + r.message);

    // A leftover staging folder from a crash is replaced
    createDirectories(g_work + "/exe4/Extensions/.staging/PkgExt");
    check(writeFileText(g_work + "/exe4/Extensions/.staging/PkgExt/stale.txt", "stale"), "stale staging written");
    r = ext::installPackage(pkg, g_work + "/exe4/Extensions", optionsWith(trash, false));
    check(r.ok && !fileExists(g_work + "/exe4/Extensions/PkgExt/stale.txt") && !dirExists(g_work + "/exe4/Extensions/.staging"), "a stale staging folder is cleared: " + r.message);
}

static void testUninstall() {
    const std::string root = g_work + "/exe5/Extensions";
    FakeTrash trash{g_work + "/trash5"};
    ext::InstallResult r = ext::installPackage(writeZip(packageFiles("PkgExt")), root, optionsWith(trash, false));
    check(r.ok, "installed for the uninstall tests: " + r.message);
    createDirectories(g_work + "/exe5/outside");
    ext::TrashFn fn = [&trash](const std::string& p) { return trash(p); };
    for (const char* name : {"../outside", "..", ".", "", "a/b", "a\\b", ".staging", "C:"}) {
        r = ext::uninstallExtension(root, name, fn);
        check(!r.ok && trash.moved == 0, std::string("uninstall refuses the name \"") + name + "\": " + r.message);
    }
    r = ext::uninstallExtension(root, "Missing", fn);
    check(!r.ok && contains(r.message, "not installed"), "an extension that is not there: " + r.message);
    r = ext::uninstallExtension(root, "PkgExt", nullptr);
    check(!r.ok && dirExists(root + "/PkgExt"), "no trash, no uninstall: " + r.message);
    trash.fail = true;
    r = ext::uninstallExtension(root, "PkgExt", fn);
    check(!r.ok && dirExists(root + "/PkgExt") && contains(r.message, "Nothing was changed"), "a trash that fails leaves it installed: " + r.message);
    trash.fail = false;
    r = ext::uninstallExtension(root, "PkgExt", fn);
    check(r.ok && !dirExists(root + "/PkgExt") && trash.moved == 1 && fileExists(g_work + "/trash5/item1/extension.json"), "uninstall moves the folder to the trash: " + r.message);
    ext::Registry reg;
    reg.scan(root);
    check(reg.extensions().empty(), "the loader no longer lists it");
}

// --avext <package> <exe dir>: install a real package and ask the loader.
static int installReal(const std::string& package, const std::string& exeDir) {
    ext::PackageInfo info;
    std::string why;
    if (!ext::inspectPackage(package, info, &why)) { AVER_ERROR("cannot inspect {}: {}", package, why); return 1; }
    AVER_INFO("package {} {} abi {} engine {} licence {} source {}", info.manifest.name, info.manifest.version, info.manifest.abi, info.manifest.engineRange, info.manifest.license,
              info.manifest.source);
    AVER_INFO("sha256 {} ({} bytes, {} files, {} bytes unpacked, platforms: {}) problem: {}", info.sha256, info.fileSize, info.fileCount, info.unpackedSize,
              info.platforms.empty() ? "none" : info.platforms[0], info.problem.empty() ? "none" : info.problem);
    ext::InstallOptions options;
    options.replace = true;
    options.moveToTrash = [](const std::string& path) {
        std::error_code ec;
        fsys::create_directories(fsys::path(path + ".trashed").parent_path(), ec);
        fsys::remove_all(fsys::path(path + ".trashed"), ec);
        fsys::rename(fsys::path(path), fsys::path(path + ".trashed"), ec);
        return !ec;
    };
    const ext::InstallResult r = ext::installPackage(package, exeDir + "/Extensions", options);
    AVER_INFO("install: ok={} replaced={} {}", r.ok, r.replaced, r.message);
    if (!r.ok) return 1;
    ext::Registry reg;
    reg.setExecutableDir(exeDir);
    reg.scanDefault();
    const ext::Extension* e = reg.find(info.manifest.name);
    if (!e) { AVER_ERROR("the loader does not list {}", info.manifest.name); return 1; }
    AVER_INFO("loader: {} {} status={} reason=\"{}\" native={}", e->name, e->version, e->ready() ? "Ready" : "Refused", e->reason, e->nativePath);
    return e->ready() ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--avext") return installReal(argv[2], argv[3]);
    g_work = AVER_AVEXT_TEST_WORKDIR;
    std::error_code ec;
    fsys::remove_all(fsys::path(g_work), ec);
    createDirectories(g_work);

    testDigests();
    testNames();
    testReadValid();
    testHostile();
    testInspect();
    testInstall();
    testUninstall();

    if (g_failures) {
        AVER_ERROR("AvextTest: {} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("AvextTest: all checks passed");
    return 0;
}
