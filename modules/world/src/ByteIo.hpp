#pragma once
// Little-endian byte packing for the region format. Internal to modules/world.
//
// BYTE AT A TIME IN BOTH DIRECTIONS, deliberately, and not memcpy into a native integer. AVR1's
// reader does the latter -- Reader::u32v/u64v go through a memcpy (Avr1.cpp:37-38, :57-58) -- so
// although the format is little-endian by explicit check, the READER silently assumes a
// little-endian host. That is invisible on every machine this has ever run on and would be a
// nightmare on the first one where it is not. The cost here is a few shifts on a path that runs once
// per chunk load, so there is no reason to inherit the same hazard.
#include "aver/core/Types.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace aver::world {

struct ByteWriter {
    std::vector<u8> bytes;

    void u8v(u8 v)  { bytes.push_back(v); }
    void u16v(u16 v) { for (int i = 0; i < 2; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i))); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    // Through the bit pattern, not a reinterpret of the object: this is the one place a float's
    // representation is pinned, and it must be the same 4 bytes on both sides.
    void f32v(f32 v) { u32 b; std::memcpy(&b, &v, 4); u32v(b); }
    void str(const std::string& s) {
        const u16 n = static_cast<u16>(s.size() > 0xFFFF ? 0xFFFF : s.size());
        u16v(n);
        bytes.insert(bytes.end(), s.begin(), s.begin() + n);
    }
    void pad(usize to) { while (bytes.size() % to) bytes.push_back(0); }
};

struct ByteReader {
    const u8* p = nullptr;
    usize left = 0;
    bool bad = false;   // sticky: one short read poisons every later one

    ByteReader(const u8* data, usize size) : p(data), left(size) {}

    bool need(usize n) { if (left < n) { bad = true; return false; } return true; }

    u8  u8v()  { if (!need(1)) return 0; const u8 v = *p++; --left; return v; }
    u16 u16v() { if (!need(2)) return 0; u16 v = 0; for (int i = 0; i < 2; ++i) v |= static_cast<u16>(static_cast<u16>(p[i]) << (8 * i)); p += 2; left -= 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i); p += 4; left -= 4; return v; }
    u64 u64v() { if (!need(8)) return 0; u64 v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i); p += 8; left -= 8; return v; }
    i32 i32v() { return static_cast<i32>(u32v()); }
    f32 f32v() { const u32 b = u32v(); f32 v = 0.0f; std::memcpy(&v, &b, 4); return v; }
    std::string str() {
        const u16 n = u16v();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n; left -= n;
        return s;
    }
};

} // namespace aver::world
