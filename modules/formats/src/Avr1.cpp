// The AVR1 chunked container: header, chunk directory, string table, CRC32C and xxHash64.

#include "aver/formats/Avr1.hpp"

#include "aver/core/Log.hpp"

#include <cstring>
#include <fstream>

namespace aver::fmt {
namespace {

// The CRC32C (Castagnoli) lookup table, built once.
const u32* crcTable() {
    static const auto table = [] {
        static u32 t[256];
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return &t[0];
    }();
    return table;
}

// xxHash64 primes, from the reference definition.
constexpr u64 kP1 = 11400714785074694791ULL, kP2 = 14029467366897019727ULL;
constexpr u64 kP3 =  1609587929392839161ULL, kP4 =  9650029242287828579ULL;
constexpr u64 kP5 =  2870177450012600261ULL;

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
inline u64 round64(u64 acc, u64 v) { acc += v * kP2; acc = rotl64(acc, 31); return acc * kP1; }
inline u64 merge64(u64 acc, u64 v) { acc ^= round64(0, v); return acc * kP1 + kP4; }
inline u64 read64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline u32 read32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }

// Little-endian byte writer over a growing buffer.
struct Writer {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u16v(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (i * 8))); }
    void raw (const void* p, usize n) { const u8* s = static_cast<const u8*>(p); b.insert(b.end(), s, s + n); }
    void pad (usize to) { while (b.size() % to) b.push_back(0); }
};

// Bounds-checked little-endian byte reader. `ok` goes false on the first short read.
struct Reader {
    const u8* p; const u8* end; bool ok = true;
    bool need(usize n) { if (usize(end - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u16 u16v() { if (!need(2)) return 0; u16 v = u16(p[0]) | u16(p[1] << 8); p += 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v = read32(p); p += 4; return v; }
    u64 u64v() { if (!need(8)) return 0; u64 v = read64(p); p += 8; return v; }
    void raw(void* d, usize n) { if (!need(n)) return; std::memcpy(d, p, n); p += n; }
    void skip(usize n) { if (need(n)) p += n; }
};

// Sets `why` and returns false.
bool fail(std::string* why, std::string msg) { if (why) *why = std::move(msg); return false; }

} // namespace

// CRC32C (Castagnoli) over a byte range.
u32 avrCrc32c(const void* data, usize size) {
    const u8* p = static_cast<const u8*>(data);
    const u32* t = crcTable();
    u32 c = 0xFFFFFFFFu;
    for (usize i = 0; i < size; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// xxHash64 over a byte range, seed 0.
u64 avrHash64(const void* data, usize size) {
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h;
    if (size >= 32) {
        u64 v1 = kP1 + kP2, v2 = kP2, v3 = 0, v4 = 0ULL - kP1;
        const u8* limit = end - 32;
        do {
            v1 = round64(v1, read64(p)); p += 8;
            v2 = round64(v2, read64(p)); p += 8;
            v3 = round64(v3, read64(p)); p += 8;
            v4 = round64(v4, read64(p)); p += 8;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge64(h, v1); h = merge64(h, v2); h = merge64(h, v3); h = merge64(h, v4);
    } else {
        h = kP5;
    }
    h += static_cast<u64>(size);
    while (p + 8 <= end) { h ^= round64(0, read64(p)); h = rotl64(h, 27) * kP1 + kP4; p += 8; }
    if (p + 4 <= end)    { h ^= static_cast<u64>(read32(p)) * kP1; h = rotl64(h, 23) * kP2 + kP3; p += 4; }
    while (p < end)      { h ^= static_cast<u64>(*p++) * kP5; h = rotl64(h, 11) * kP1; }
    h ^= h >> 33; h *= kP2; h ^= h >> 29; h *= kP3; h ^= h >> 32;
    return h;
}

// The first chunk with this id, or nullptr.
const AvrChunk* Avr1File::find(u32 chunkId) const {
    for (const AvrChunk& c : chunks) if (c.id == chunkId) return &c;
    return nullptr;
}

// Appends a chunk and returns a reference to it.
AvrChunk& Avr1File::add(u32 chunkId, std::vector<u8> payload, u8 chunkFlags, u16 chunkVersion) {
    chunks.push_back(AvrChunk{chunkId, chunkVersion, 0, chunkFlags, std::move(payload)});
    return chunks.back();
}

// Starts the blob with the empty string, so offset 0 always means "".
AvrStringTable::AvrStringTable() {
    blob_.push_back(0); blob_.push_back(0);
}

// Interns a string and returns its offset. An identical entry is reused.
u32 AvrStringTable::add(std::string_view s) {
    usize off = 0;
    while (off + 2 <= blob_.size()) {
        const u16 len = u16(blob_[off]) | u16(u16(blob_[off + 1]) << 8);
        if (off + 2 + len > blob_.size()) break;
        if (std::string_view(reinterpret_cast<const char*>(blob_.data() + off + 2), len) == s)
            return static_cast<u32>(off);
        off += 2 + len;
    }
    const u32 ref = static_cast<u32>(blob_.size());
    const u16 len = static_cast<u16>(s.size() > 0xFFFF ? 0xFFFF : s.size());
    blob_.push_back(u8(len)); blob_.push_back(u8(len >> 8));
    blob_.insert(blob_.end(), s.begin(), s.begin() + len);
    return ref;
}

// The string at `ref`, or empty when the reference is null or out of range.
std::string_view AvrStringTable::get(u32 ref) const {
    if (ref == kAvrStringNull || ref + 2 > blob_.size()) return {};
    const u16 len = u16(blob_[ref]) | u16(u16(blob_[ref + 1]) << 8);
    if (ref + 2 + len > blob_.size()) return {};
    return std::string_view(reinterpret_cast<const char*>(blob_.data() + ref + 2), len);
}

// Serialises a container into `out`. Returns false with `why` set.
bool writeAvr1(const Avr1File& in, std::vector<u8>& out, std::string* why) {
    if (in.subtype == 0) return fail(why, "AVR1: subtype is 0");
    const usize align = usize(1) << (in.alignLog2 ? in.alignLog2 : 4);

    out.clear();
    Writer w{out};

    // Header and directory reserved first, patched once the payload offsets are known.
    out.resize(64, 0);
    const usize dirOffset = 64;
    out.resize(dirOffset + in.chunks.size() * 40, 0);

    // Payloads in directory order. GpuUploadable chunks align to 256 (§3.2).
    std::vector<u64> offsets(in.chunks.size()), sizes(in.chunks.size()), hashes(in.chunks.size());
    for (usize i = 0; i < in.chunks.size(); ++i) {
        const AvrChunk& c = in.chunks[i];
        const usize a = (c.flags & kAvrChunkGpuUploadable) ? 256u : align;
        while (out.size() % a) out.push_back(0);
        offsets[i] = out.size();
        sizes[i]   = c.data.size();
        hashes[i]  = avrHash64(c.data.data(), c.data.size());
        out.insert(out.end(), c.data.begin(), c.data.end());
    }

    // The directory.
    {
        std::vector<u8> dir;
        Writer dw{dir};
        for (usize i = 0; i < in.chunks.size(); ++i) {
            const AvrChunk& c = in.chunks[i];
            dw.u32v(c.id);
            dw.u16v(c.version);
            dw.u8v(0);            // Compression: v1 writes uncompressed only, see Avr1.hpp
            dw.u8v(c.flags);
            dw.u64v(offsets[i]);
            dw.u64v(sizes[i]);    // SizeOnDisk
            dw.u64v(sizes[i]);    // SizeUncompressed == SizeOnDisk while uncompressed
            dw.u64v(hashes[i]);
        }
        std::memcpy(out.data() + dirOffset, dir.data(), dir.size());
    }

    // The header, patched in place.
    {
        std::vector<u8> hdr;
        Writer hw{hdr};
        hw.u32v(kAvr1Magic);
        hw.u32v(in.subtype);
        hw.u16v(kAvrContainerVersion);
        hw.u16v(in.contentVersion);
        hw.u16v(in.minReaderVersion);
        hw.u8v(0);                                   // Endianness: LE, the only v1 value
        hw.u8v(in.alignLog2 ? in.alignLog2 : 4);
        hw.u32v(64);                                 // HeaderSize
        hw.u32v(static_cast<u32>(in.chunks.size()));
        hw.u64v(dirOffset);
        hw.u64v(static_cast<u64>(out.size()));       // FileSize
        hw.raw(in.guid, 16);
        hw.u32v(in.flags);
        // HeaderCrc covers 0x00..0x3B, which is exactly what has been written so far.
        hw.u32v(avrCrc32c(hdr.data(), hdr.size()));
        std::memcpy(out.data(), hdr.data(), 64);
    }
    return true;
}

// Parses a container, verifying the header CRC and every chunk hash. Returns false with `why` set.
bool parseAvr1(const u8* bytes, usize size, Avr1File& out, std::string* why) {
    if (!bytes || size < 64) return fail(why, "AVR1: shorter than a header");
    Reader r{bytes, bytes + size};

    const u32 magic = r.u32v();
    if (magic != kAvr1Magic) return fail(why, "AVR1: bad magic (not an AVR1 container)");

    out.subtype           = r.u32v();
    const u16 containerV  = r.u16v();
    out.contentVersion    = r.u16v();
    out.minReaderVersion  = r.u16v();
    const u8 endian       = r.u8v();
    out.alignLog2         = r.u8v();
    const u32 headerSize  = r.u32v();
    const u32 chunkCount  = r.u32v();
    const u64 dirOffset   = r.u64v();
    const u64 fileSize    = r.u64v();
    r.raw(out.guid, 16);
    out.flags             = r.u32v();
    const u32 storedCrc   = r.u32v();
    if (!r.ok) return fail(why, "AVR1: truncated header");

    if (endian != 0) return fail(why, "AVR1: big-endian files are not defined in v1");
    // Before any other header field is trusted.
    if (avrCrc32c(bytes, 60) != storedCrc) return fail(why, "AVR1: header CRC mismatch (file corrupt)");
    if (out.minReaderVersion > kAvrContainerVersion)
        return fail(why, "AVR1: file needs container version " + std::to_string(out.minReaderVersion) +
                         ", this reader is " + std::to_string(kAvrContainerVersion));
    (void)containerV;
    if (headerSize < 64) return fail(why, "AVR1: HeaderSize below 64");
    if (fileSize != size)
        return fail(why, "AVR1: FileSize says " + std::to_string(fileSize) + " but the file is " +
                         std::to_string(size));
    if (dirOffset + u64(chunkCount) * 40 > size) return fail(why, "AVR1: chunk directory runs past the end");

    out.chunks.clear();
    out.chunks.reserve(chunkCount);
    for (u32 i = 0; i < chunkCount; ++i) {
        Reader d{bytes + dirOffset + usize(i) * 40, bytes + size};
        AvrChunk c;
        c.id            = d.u32v();
        c.version       = d.u16v();
        c.compression   = d.u8v();
        c.flags         = d.u8v();
        const u64 off   = d.u64v();
        const u64 onDisk= d.u64v();
        const u64 raw   = d.u64v();
        const u64 hash  = d.u64v();
        if (!d.ok) return fail(why, "AVR1: truncated chunk directory entry");
        if (off + onDisk > size) return fail(why, "AVR1: chunk payload runs past the end");
        if (c.compression != 0)
            return fail(why, "AVR1: chunk is compressed and no decompressor is built in");
        if (onDisk != raw)
            return fail(why, "AVR1: uncompressed chunk with mismatched sizes");

        c.data.assign(bytes + off, bytes + off + onDisk);
        if (avrHash64(c.data.data(), c.data.size()) != hash)
            return fail(why, "AVR1: chunk payload hash mismatch (file corrupt)");
        out.chunks.push_back(std::move(c));
    }
    return true;
}

// Reads and parses a container file. Returns false with `why` set.
bool loadAvr1(const std::string& path, Avr1File& out, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "AVR1: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "AVR1: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "AVR1: short read on " + path);
    return parseAvr1(bytes.data(), bytes.size(), out, why);
}

// Serialises a container and writes it to a file. Returns false with `why` set.
bool saveAvr1(const std::string& path, const Avr1File& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeAvr1(in, bytes, why)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(why, "AVR1: cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(why, "AVR1: write failed on " + path);
    return true;
}

} // namespace aver::fmt
