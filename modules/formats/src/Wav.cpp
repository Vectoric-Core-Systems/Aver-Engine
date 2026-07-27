#include "aver/formats/Wav.hpp"

#include <cstring>
#include <cstdio>

namespace aver::formats {
namespace {

constexpr u16 kFormatPcm   = 1;
constexpr u16 kFormatFloat = 3;
constexpr u16 kFormatExtensible = 0xFFFE;

u32 readU32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
u16 readU16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }

WavResult fail(const char* why) { return WavResult{false, why}; }

} // namespace

WavResult wavRead(const std::vector<u8>& bytes, audio::SoundData& out) {
    out = audio::SoundData{};
    if (bytes.size() < 44) return fail("shorter than the smallest possible WAV header");
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0) return fail("not a RIFF file");
    if (std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return fail("RIFF, but not WAVE");

    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    usize dataBytes = 0;

    // Walked as a chunk list rather than assumed to be fmt-then-data. Real files carry LIST, fact
    // and smpl chunks in between, and a reader that assumes the canonical 44-byte layout reads a
    // metadata block as samples -- which is, again, full-scale noise.
    usize p = 12;
    while (p + 8 <= bytes.size()) {
        const u8* id = bytes.data() + p;
        const u32 size = readU32(bytes.data() + p + 4);
        const usize body = p + 8;
        if (body + size > bytes.size()) break;   // truncated: use what has been found so far

        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            format   = readU16(bytes.data() + body + 0);
            channels = readU16(bytes.data() + body + 2);
            rate     = readU32(bytes.data() + body + 4);
            bits     = readU16(bytes.data() + body + 14);
            // WAVE_FORMAT_EXTENSIBLE carries the real format in a GUID whose first two bytes are the
            // tag it is standing in for, which is the only part of that GUID anything needs.
            if (format == kFormatExtensible && size >= 40)
                format = readU16(bytes.data() + body + 24);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = bytes.data() + body;
            dataBytes = size;
        }
        // Chunks are word-aligned: an odd length is followed by a pad byte that is not counted in it.
        p = body + size + (size & 1);
    }

    if (!data || dataBytes == 0) return fail("no data chunk");
    if (channels == 0 || rate == 0) return fail("no usable fmt chunk");
    if (format != kFormatPcm && format != kFormatFloat)
        return fail("compressed WAV: only integer PCM and 32-bit float are read");
    if (format == kFormatFloat && bits != 32) return fail("float WAV that is not 32-bit");
    if (format == kFormatPcm && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return fail("PCM WAV with an unsupported bit depth");

    const u32 bytesPerSample = bits / 8u;
    const usize total = dataBytes / bytesPerSample;

    out.channels   = channels;
    out.sampleRate = rate;
    out.samples.resize(total);

    for (usize i = 0; i < total; ++i) {
        const u8* s = data + i * bytesPerSample;
        if (format == kFormatFloat) {
            f32 v; std::memcpy(&v, s, 4);
            out.samples[i] = v;
        } else if (bits == 8) {
            // 8-bit WAV is UNSIGNED, alone among the depths. Read as signed it is a DC offset of half
            // full scale plus an inverted waveform -- loud, and not obviously the wrong sign.
            out.samples[i] = (static_cast<f32>(s[0]) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            i16 v; std::memcpy(&v, s, 2);
            out.samples[i] = static_cast<f32>(v) / 32768.0f;
        } else if (bits == 24) {
            // Sign-extended from 24 bits by hand: there is no 24-bit integer type to memcpy into.
            const i32 v = static_cast<i32>((static_cast<u32>(s[0])) | (static_cast<u32>(s[1]) << 8) |
                                           (static_cast<u32>(s[2]) << 16) |
                                           ((s[2] & 0x80) ? 0xFF000000u : 0u));
            out.samples[i] = static_cast<f32>(v) / 8388608.0f;
        } else {
            i32 v; std::memcpy(&v, s, 4);
            out.samples[i] = static_cast<f32>(v) / 2147483648.0f;
        }
    }

    return WavResult{true, {}};
}

WavResult wavReadFile(const std::string& path, audio::SoundData& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("could not open the file");
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return fail("empty file"); }
    std::vector<u8> bytes(static_cast<usize>(n));
    const usize got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return fail("short read");
    return wavRead(bytes, out);
}

} // namespace aver::formats
