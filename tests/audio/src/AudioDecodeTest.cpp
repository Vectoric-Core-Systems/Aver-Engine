// Headless tests for the Ogg Vorbis / FLAC / MP3 decoders (modules/formats AudioDecode*): no device, no
// audio file in the repo, no encoder. The FLAC streams are built here -- STREAMINFO, verbatim, constant
// and fixed-predictor subframes, stereo decorrelation, CRC-8 and CRC-16 -- so the decoded samples are
// checked exactly. The MP3 streams are silent MPEG-1 Layer III frames with a hand-built Xing/LAME
// tag, which pins the frame count, the gapless trim and seeking. Vorbis cannot be built without an
// encoder, so it gets malformed and hostile input (and an optional out-of-repo file, see main).
#include "aver/audio/Stream.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/AudioDecode.hpp"
#include "aver/formats/OcAudio.hpp"

#if defined(AVER_TEST_HAS_ABI)
#  include "aver/audio/audio_abi.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

// A fixed-seed generator: the same bytes on every run and every platform.
struct Rng {
    u32 s = 0x2545F491u;
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    i32 range(i32 lo, i32 hi) { return lo + static_cast<i32>(next() % static_cast<u32>(hi - lo + 1)); }
};

// ---- bit writers -------------------------------------------------------------------------------

struct BitsMsb {       // FLAC order
    std::vector<u8> b;
    int used = 0;
    void bit(unsigned x) { if (used == 0) b.push_back(0); if (x) b.back() = static_cast<u8>(b.back() | (0x80 >> used)); used = (used + 1) & 7; }
    void put(u64 v, int n) { for (int i = n - 1; i >= 0; --i) bit(static_cast<unsigned>((v >> i) & 1)); }
    void unary(u64 q) { for (u64 i = 0; i < q; ++i) bit(0); bit(1); }
    void align() { used = 0; }
};

struct BitsLsb {       // Vorbis order
    std::vector<u8> b;
    int used = 0;
    void bit(unsigned x) { if (used == 0) b.push_back(0); if (x) b.back() = static_cast<u8>(b.back() | (1 << used)); used = (used + 1) & 7; }
    void put(u64 v, int n) { for (int i = 0; i < n; ++i) bit(static_cast<unsigned>((v >> i) & 1)); }
};

static u8 crc8(const u8* p, usize n) {
    u8 c = 0;
    for (usize i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = static_cast<u8>((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
    return c;
}
static u16 crc16(const u8* p, usize n) {
    u16 c = 0;
    for (usize i = 0; i < n; ++i) { c = static_cast<u16>(c ^ (p[i] << 8)); for (int k = 0; k < 8; ++k) c = static_cast<u16>((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1)); }
    return c;
}

// ---- a FLAC stream built in code ---------------------------------------------------------------

struct Sub {
    enum Kind { Constant, Verbatim, Fixed } kind = Verbatim;
    int order = 0, riceK = 0, bps = 16;
    std::vector<i32> x;
};

static void writeSub(BitsMsb& w, const Sub& s) {
    const u64 mask = (s.bps >= 64) ? ~0ull : ((1ull << s.bps) - 1);
    w.put(0, 1);
    if (s.kind == Sub::Constant) {
        w.put(0, 6); w.put(0, 1);
        w.put(static_cast<u64>(static_cast<i64>(s.x[0])) & mask, s.bps);
    } else if (s.kind == Sub::Verbatim) {
        w.put(1, 6); w.put(0, 1);
        for (i32 v : s.x) w.put(static_cast<u64>(static_cast<i64>(v)) & mask, s.bps);
    } else {
        w.put(static_cast<u64>(8 + s.order), 6); w.put(0, 1);
        for (int i = 0; i < s.order; ++i) w.put(static_cast<u64>(static_cast<i64>(s.x[i])) & mask, s.bps);
        w.put(0, 2);                 // Rice, 4-bit parameters
        w.put(0, 4);                 // one partition
        w.put(static_cast<u64>(s.riceK), 4);
        for (usize i = static_cast<usize>(s.order); i < s.x.size(); ++i) {
            i64 pred = 0;
            if (s.order == 1) pred = s.x[i - 1];
            else if (s.order == 2) pred = 2 * static_cast<i64>(s.x[i - 1]) - s.x[i - 2];
            const i64 r = static_cast<i64>(s.x[i]) - pred;
            const u64 u = r >= 0 ? static_cast<u64>(r) * 2 : static_cast<u64>(-r) * 2 - 1;
            w.unary(u >> s.riceK);
            w.put(u & ((1ull << s.riceK) - 1), s.riceK);
        }
    }
}

static int blockSizeCode(u32 bs) {
    switch (bs) { case 256: return 8; case 512: return 9; case 1024: return 10; case 2048: return 11; case 4096: return 12; default: return bs <= 256 ? 6 : 7; }
}

static std::vector<u8> flacFrame(u32 frameNo, u32 blockSize, u32 chAssign, const std::vector<Sub>& subs) {
    BitsMsb w;
    const int code = blockSizeCode(blockSize);
    w.put(0x3FFE, 14); w.put(0, 1); w.put(0, 1);
    w.put(static_cast<u64>(code), 4); w.put(0, 4); w.put(chAssign, 4); w.put(0, 3); w.put(0, 1);
    w.put(frameNo, 8);                                        // frame numbers here stay under 128
    if (code == 6) w.put(blockSize - 1, 8);
    if (code == 7) w.put(blockSize - 1, 16);
    w.put(crc8(w.b.data(), w.b.size()), 8);
    for (const Sub& s : subs) writeSub(w, s);
    w.align();
    const u16 c = crc16(w.b.data(), w.b.size());
    w.b.push_back(static_cast<u8>(c >> 8));
    w.b.push_back(static_cast<u8>(c & 0xFF));
    return w.b;
}

static std::vector<u8> flacHeader(u32 minBs, u32 maxBs, u32 rate, u32 channels, u32 bps, u64 total) {
    BitsMsb w;
    w.put(minBs, 16); w.put(maxBs, 16); w.put(0, 24); w.put(0, 24);
    w.put(rate, 20); w.put(channels - 1, 3); w.put(bps - 1, 5); w.put(total, 36);
    for (int i = 0; i < 16; ++i) w.put(0, 8);                 // MD5: unset
    std::vector<u8> out = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    out.insert(out.end(), w.b.begin(), w.b.end());
    return out;
}

struct TestFlac {
    std::vector<u8> bytes;
    std::vector<i32> L, R;      // what a decoder must give back, in 16-bit steps
    u32 rate = 44100;
};

// Five frames, every subframe kind and three channel assignments: 4 x 256 frames and a last block of 100.
static TestFlac makeTestFlac() {
    TestFlac t;
    constexpr u32 kBlocks[5] = {256, 256, 256, 256, 100};
    const u32 total = 4 * 256 + 100;
    Rng rng;
    for (u32 i = 0; i < total; ++i) { t.L.push_back(rng.range(-20000, 20000)); t.R.push_back(rng.range(-20000, 20000)); }
    for (u32 i = 3 * 256; i < 4 * 256; ++i) t.L[i] = 1234;    // frame 3: a constant left channel

    t.bytes = flacHeader(100, 256, t.rate, 2, 16, total);
    u32 at = 0;
    auto slice = [&](const std::vector<i32>& v, u32 n) { return std::vector<i32>(v.begin() + at, v.begin() + at + n); };
    for (u32 f = 0; f < 5; ++f) {
        const u32 n = kBlocks[f];
        Sub a, b;
        u32 assign = 1;                                       // two channels, independent
        a.x = slice(t.L, n); b.x = slice(t.R, n);
        if (f == 1) { a.kind = Sub::Fixed; a.order = 2; a.riceK = 10; b.kind = Sub::Fixed; b.order = 1; b.riceK = 9; }
        if (f == 2) {                                         // left / side
            assign = 8;
            for (u32 i = 0; i < n; ++i) b.x[i] = a.x[i] - b.x[i];
            b.bps = 17;
        }
        if (f == 3) { a.kind = Sub::Constant; b.kind = Sub::Fixed; b.order = 0; b.riceK = 13; }
        if (f == 4) {                                         // mid / side
            assign = 10;
            std::vector<i32> mid(n), side(n);
            for (u32 i = 0; i < n; ++i) { mid[i] = (a.x[i] + b.x[i]) >> 1; side[i] = a.x[i] - b.x[i]; }
            a.x = mid; b.x = side; b.bps = 17;
        }
        const std::vector<u8> fr = flacFrame(f, n, assign, {a, b});
        t.bytes.insert(t.bytes.end(), fr.begin(), fr.end());
        at += n;
    }
    return t;
}

// ---- helpers -----------------------------------------------------------------------------------

static std::vector<f32> readAll(audio::StreamSource& s, u32 chunk = 97, u64 capFrames = 1u << 24) {
    std::vector<f32> out;
    std::vector<f32> buf(static_cast<usize>(chunk) * s.channels());
    u64 frames = 0;
    for (;;) {
        const u32 n = s.read(buf.data(), chunk);
        if (n == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n) * s.channels());
        frames += n;
        if (frames >= capFrames) break;
    }
    return out;
}

static fmt::DecodedStream openMem(const std::vector<u8>& b, fmt::AudioCodec c, const fmt::DecodeOptions& o = {}, std::string* why = nullptr) {
    return fmt::openDecodedMemory(b, c, o, why);
}

static bool matchesStereo(const std::vector<f32>& got, const std::vector<i32>& L, const std::vector<i32>& R, usize from, usize count) {
    if (got.size() != count * 2) return false;
    for (usize i = 0; i < count; ++i) {
        if (got[i * 2] != static_cast<f32>(L[from + i]) / 32768.0f) return false;
        if (got[i * 2 + 1] != static_cast<f32>(R[from + i]) / 32768.0f) return false;
    }
    return true;
}

// ---- tests -------------------------------------------------------------------------------------

static void testDetection() {
    AVER_INFO("=== detection and tags ===");
    check(fmt::audioCodecForPath("a/b/c.OGG") == fmt::AudioCodec::Vorbis && fmt::audioCodecForPath("x.oga") == fmt::AudioCodec::Vorbis, ".ogg and .oga are Vorbis, case-insensitive");
    check(fmt::audioCodecForPath("x.flac") == fmt::AudioCodec::Flac && fmt::audioCodecForPath("x.mp3") == fmt::AudioCodec::Mp3, ".flac and .mp3");
    check(fmt::audioCodecForPath("x.wav") == fmt::AudioCodec::None && fmt::audioCodecForPath("dir.ogg/file") == fmt::AudioCodec::None, "anything else is not, and a dot in a directory is not an extension");
    const u8 ogg[] = {'O', 'g', 'g', 'S'}, flac[] = {'f', 'L', 'a', 'C'}, id3[] = {'I', 'D', '3', 3}, sync[] = {0xFF, 0xFB}, junk[] = {1, 2, 3, 4};
    check(fmt::sniffAudioCodec(ogg, 4) == fmt::AudioCodec::Vorbis && fmt::sniffAudioCodec(flac, 4) == fmt::AudioCodec::Flac, "the magic bytes name Ogg and FLAC");
    check(fmt::sniffAudioCodec(id3, 4) == fmt::AudioCodec::Mp3 && fmt::sniffAudioCodec(sync, 2) == fmt::AudioCodec::Mp3, "an ID3 tag or a frame sync names MP3");
    check(fmt::sniffAudioCodec(junk, 4) == fmt::AudioCodec::None && fmt::sniffAudioCodec(nullptr, 0) == fmt::AudioCodec::None, "junk and nothing name nothing");

    u32 b = 99, l = 99;
    const char* both[] = {"TITLE=x", "loopstart=44100", "LoopLength=88200"};
    check(fmt::parseLoopComments(both, 3, b, l) && b == 44100 && l == 88200, "LOOPSTART and LOOPLENGTH are read, any case");
    const char* endOnly[] = {"LOOPSTART=1000", "LOOPEND=5000"};
    check(fmt::parseLoopComments(endOnly, 2, b, l) && b == 1000 && l == 4000, "LOOPEND with LOOPSTART gives the length");
    const char* startOnly[] = {"LOOPSTART=300"};
    check(fmt::parseLoopComments(startOnly, 1, b, l) && b == 300 && l == 0, "LOOPSTART alone loops to the end");
    const char* bad[] = {"LOOPSTART=abc", "LOOPLENGTH=99999999999", "NOEQUALS", "LOOPSTART"};
    check(!fmt::parseLoopComments(bad, 4, b, l), "non-numbers, values past 32 bits and malformed lines are ignored");
    check(!fmt::parseLoopComments(nullptr, 0, b, l), "no comments, no loop");

    u64 lb = 100, le = 300;
    check(fmt::loopIntoWindow(40, 0, lb, le) && lb == 60 && le == 260, "a loop moves into a window that starts earlier");
    lb = 10; le = 300;
    check(fmt::loopIntoWindow(40, 0, lb, le) && lb == 0 && le == 260, "a loop starting before the window starts at its first frame");
    lb = 10; le = 30;
    check(!fmt::loopIntoWindow(40, 0, lb, le), "a loop wholly before the window is dropped");
    lb = 100; le = 0;
    check(!fmt::loopIntoWindow(0, 50, lb, le), "a loop that starts after the window ends is dropped");
    lb = 10; le = 500;
    check(fmt::loopIntoWindow(0, 50, lb, le) && le == 50, "a loop end past the window is clamped to it");
}

static void testFlacExact() {
    AVER_INFO("=== FLAC, built in code, exact samples ===");
    const TestFlac t = makeTestFlac();
    std::string why;
    fmt::DecodedStream ds = openMem(t.bytes, fmt::AudioCodec::Flac, {}, &why);
    check(ds.source != nullptr, "the stream opens (" + why + ")");
    if (!ds.source) return;
    check(ds.codec == fmt::AudioCodec::Flac && ds.source->channels() == 2 && ds.source->sampleRate() == 44100, "stereo, 44100 Hz, FLAC");
    check(ds.source->totalFrames() == t.L.size(), "STREAMINFO's frame count is reported");
    const std::vector<f32> all = readAll(*ds.source);
    check(matchesStereo(all, t.L, t.R, 0, t.L.size()),
          "every sample of verbatim, fixed-predictor, constant, left/side and mid/side subframes comes back exactly");

    check(ds.source->seek(300), "seek to a frame in the middle");
    std::vector<f32> part(10 * 2);
    check(ds.source->read(part.data(), 10) == 10 && matchesStereo(part, t.L, t.R, 300, 10), "and the samples after it are the right ones");
    check(ds.source->seek(0), "seek back to the start");
    const std::vector<f32> again = readAll(*ds.source);
    check(again == all, "and the whole stream reads identically a second time");
    check(!ds.source->seek(t.L.size() + 1000), "a seek past the end is refused");

    // The same bytes through a file, on the decoder-thread path.
    const fs::path dir = fs::temp_directory_path() / "aver-decode-test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path p = dir / "built.flac";
    { std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(t.bytes.data()), static_cast<std::streamsize>(t.bytes.size())); }
    audio::SoundData sd;
    fmt::AudioCodec codec = fmt::AudioCodec::None;
    const bool decoded = fmt::decodeAudioFile(p.string(), sd, &why, &codec);
    check(decoded && codec == fmt::AudioCodec::Flac, "a .flac file decodes whole for short clips (" + why + ")");
    check(sd.channels == 2 && sd.sampleRate == 44100 && sd.frames() == t.L.size() && matchesStereo(sd.samples, t.L, t.R, 0, t.L.size()), "to exactly those samples");
    audio::SoundData imp;
    const fmt::AudioImportResult r = fmt::audioImportFile(p.string(), imp);
    check(r.ok && r.decoder == "flac" && imp.samples == sd.samples, "the importer uses the vendored decoder and gets the same audio");
    fs::remove_all(dir, ec);
}

static void testFlacWindowAndLoop() {
    AVER_INFO("=== FLAC: start offset, duration, loop points, folding ===");
    const TestFlac t = makeTestFlac();
    {
        fmt::DecodeOptions o;
        o.startFrame = 100; o.durationFrames = 50;
        fmt::DecodedStream ds = openMem(t.bytes, fmt::AudioCodec::Flac, o);
        check(ds.source != nullptr && ds.source->totalFrames() == 50, "a window reports its own length");
        if (ds.source) {
            const std::vector<f32> w = readAll(*ds.source);
            check(matchesStereo(w, t.L, t.R, 100, 50), "it yields exactly the frames [100, 150)");
            check(ds.source->seek(10), "seek is relative to the window");
            std::vector<f32> p(2 * 5);
            check(ds.source->read(p.data(), 5) == 5 && matchesStereo(p, t.L, t.R, 110, 5), "frame 10 of the window is frame 110 of the file");
            check(!ds.source->seek(51), "past the window is refused");
        }
    }
    {
        fmt::DecodeOptions o;
        o.startFrame = 1100;                                 // the last 24 frames
        fmt::DecodedStream ds = openMem(t.bytes, fmt::AudioCodec::Flac, o);
        check(ds.source != nullptr && ds.source->totalFrames() == 24, "a start offset alone runs to the end of the file");
    }
    {
        fmt::DecodeOptions o;
        o.startFrame = 5; o.loopBegin = 300; o.loopLength = 500;
        fmt::DecodedStream ds = openMem(t.bytes, fmt::AudioCodec::Flac, o);
        check(ds.source != nullptr && ds.loopBegin == 295 && ds.loopEnd == 795, "loop points given in file frames move into the window");
    }

    // The loop through the real ring and decoder (no thread: pumped by hand), against the exact samples.
    {
        fmt::DecodeOptions o;
        o.loopBegin = 300; o.loopLength = 500;
        fmt::DecodedStream ds = openMem(t.bytes, fmt::AudioCodec::Flac, o);
        audio::StreamParams sp;
        sp.looping = true; sp.loopBegin = ds.loopBegin; sp.loopEnd = ds.loopEnd; sp.threaded = false; sp.ringFrames = 4096;
        audio::StreamDecoder dec(std::move(ds.source), sp);
        while (dec.pump(4096) > 0) {}
        check(dec.available() >= 2500, "the looping ring fills with the loop, not the end of the file");
        bool ok = true;
        for (u32 i = 0; i < 2500 && ok; ++i) {
            const u32 src = i < 800 ? i : 300 + ((i - 800) % 500);
            const f32* fr = dec.frameAt(i);
            ok = fr[0] == static_cast<f32>(t.L[src]) / 32768.0f && fr[1] == static_cast<f32>(t.R[src]) / 32768.0f;
        }
        check(ok, "frames 0..799, then 300..799 again and again, sample for sample");
    }

    // Six channels folded to stereo: constant channels, FLAC order L R C LFE BL BR.
    {
        constexpr i32 v[6] = {16384, 8192, 4096, 30000, 2048, 1024};
        std::vector<Sub> subs(6);
        for (int c = 0; c < 6; ++c) { subs[c].kind = Sub::Constant; subs[c].x = {v[c]}; }
        std::vector<u8> bytes = flacHeader(256, 256, 48000, 6, 16, 256);
        const std::vector<u8> fr = flacFrame(0, 256, 5, subs);
        bytes.insert(bytes.end(), fr.begin(), fr.end());
        fmt::DecodedStream ds = openMem(bytes, fmt::AudioCodec::Flac);
        check(ds.source != nullptr && ds.source->channels() == 2 && ds.source->sampleRate() == 48000, "5.1 opens as a stereo stream");
        if (ds.source) {
            const std::vector<f32> g = readAll(*ds.source);
            const f32 k = 0.70710678f, n = 1.0f / (1.0f + 2.0f * k);
            const f32 eL = (16384 + k * 4096 + k * 2048) / 32768.0f * n, eR = (8192 + k * 4096 + k * 1024) / 32768.0f * n;
            check(g.size() == 512 && near(g[0], eL) && near(g[1], eR) && near(g[510], eL) && near(g[511], eR),
                  "centre and surrounds at -3 dB into the right side, the LFE dropped");
        }
    }
}

static void testFlacHostile() {
    AVER_INFO("=== FLAC: hostile input ===");
    const TestFlac t = makeTestFlac();
    const usize bound = t.L.size() + 4096;      // nothing may decode to more than the stream holds

    // Every truncation of the stream.
    {
        bool sane = true;
        int opened = 0;
        for (usize len = 0; len <= t.bytes.size(); len += 3) {
            std::vector<u8> cut(t.bytes.begin(), t.bytes.begin() + static_cast<std::ptrdiff_t>(len));
            fmt::DecodedStream ds = openMem(cut, fmt::AudioCodec::Flac);
            if (!ds.source) continue;
            ++opened;
            const std::vector<f32> all = readAll(*ds.source, 64, 1u << 20);
            if (all.size() / 2 > bound) sane = false;
        }
        check(sane && opened > 0, "truncated at every third byte: no crash, nothing decoded beyond what the bytes hold (" + std::to_string(opened) + " opened)");
    }
    // Flipped bytes everywhere: the CRCs reject frames, the decoder resynchronises or stops.
    {
        Rng rng;
        bool sane = true;
        for (int i = 0; i < 400; ++i) {
            std::vector<u8> bad = t.bytes;
            const int flips = 1 + static_cast<int>(rng.next() % 6);
            for (int f = 0; f < flips; ++f) bad[rng.next() % bad.size()] = static_cast<u8>(rng.next());
            fmt::DecodedStream ds = openMem(bad, fmt::AudioCodec::Flac);
            if (!ds.source) continue;
            const std::vector<f32> all = readAll(*ds.source, 128, 1u << 20);
            if (all.size() / ds.source->channels() > (1u << 20)) sane = false;
            ds.source->seek(rng.next() % 2000);
            readAll(*ds.source, 128, 1u << 16);
        }
        check(sane, "400 streams with random bytes overwritten, decoded and seeked: no crash, bounded");
    }
    // Pure garbage, with and without the magic.
    {
        Rng rng;
        int opened = 0;
        for (int i = 0; i < 300; ++i) {
            std::vector<u8> junk(rng.next() % 3000);
            for (u8& b : junk) b = static_cast<u8>(rng.next());
            if (i & 1 && junk.size() > 4) std::memcpy(junk.data(), "fLaC", 4);
            fmt::DecodedStream ds = openMem(junk, fmt::AudioCodec::Flac);
            if (ds.source) { ++opened; readAll(*ds.source, 128, 1u << 16); }
        }
        check(true, "300 garbage buffers (half starting fLaC): no crash (" + std::to_string(opened) + " opened)");
    }
    // Huge declared sizes: a metadata block of 16 MB in a 60-byte file, a 36-bit sample count, 8 channels of 65535-frame blocks.
    {
        std::vector<u8> a = flacHeader(16, 65535, 44100, 8, 32, (1ull << 36) - 1);
        a[4] = 0x00;                                        // STREAMINFO is not the last block
        const u8 bigBlock[] = {0x86, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4};   // PICTURE claiming 0xFFFFFF bytes
        a.insert(a.end(), bigBlock, bigBlock + sizeof(bigBlock));
        fmt::DecodedStream ds = openMem(a, fmt::AudioCodec::Flac);
        check(ds.source == nullptr || readAll(*ds.source, 4096, 1u << 16).size() < (1u << 20), "a 16 MB metadata block in a tiny file does not allocate or decode anything");

        std::vector<u8> b = flacHeader(16, 65535, 44100, 8, 32, (1ull << 36) - 1);
        std::vector<Sub> subs(8);
        for (auto& s : subs) { s.kind = Sub::Constant; s.bps = 32; s.x = {123456}; }
        const std::vector<u8> fr = flacFrame(0, 4096, 7, subs);
        b.insert(b.end(), fr.begin(), fr.end());
        fmt::DecodedStream d2 = openMem(b, fmt::AudioCodec::Flac);
        check(d2.source != nullptr && d2.source->channels() == 2, "an 8-channel 32-bit stream declaring 2^36 frames opens (folded to stereo) with its small header");
        if (d2.source) check(readAll(*d2.source, 4096, 1u << 20).size() <= 2u * 4096u, "and reads only the one frame the file holds");
    }
    // The resident decode refuses to grow past its limit, whatever the stream expands to.
    {
        std::vector<u8> b = flacHeader(4096, 4096, 44100, 1, 16, 4096 * 50);
        Sub s; s.kind = Sub::Constant; s.x = {1000};
        for (u32 f = 0; f < 50; ++f) { const std::vector<u8> fr = flacFrame(f, 4096, 0, {s}); b.insert(b.end(), fr.begin(), fr.end()); }
        audio::SoundData sd;
        std::string why;
        check(!fmt::decodeAudioMemory(b, fmt::AudioCodec::Flac, sd, &why, 100000) && sd.samples.empty() && why.find("too long") != std::string::npos,
              "50 constant frames (204800 samples in ~600 bytes) are refused at a 100000-sample limit, and nothing is kept");
        check(fmt::decodeAudioMemory(b, fmt::AudioCodec::Flac, sd, &why) && sd.frames() == 4096u * 50, "and decode in full under the default limit");
    }
    // Wrong codec for the bytes: a FLAC stream handed to the MP3 and Vorbis decoders.
    {
        fmt::DecodedStream m = openMem(t.bytes, fmt::AudioCodec::Mp3), v = openMem(t.bytes, fmt::AudioCodec::Vorbis);
        if (m.source) readAll(*m.source, 128, 1u << 14);
        check(v.source == nullptr, "FLAC bytes are not Ogg Vorbis, and the MP3 decoder survives them");
    }
}

// ---- MP3 ---------------------------------------------------------------------------------------

constexpr u32 kMp3Frame = 417;       // MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, mono, no padding

static std::vector<u8> mp3AudioFrame() {
    std::vector<u8> f(kMp3Frame, 0);
    f[0] = 0xFF; f[1] = 0xFB; f[2] = 0x90; f[3] = 0xC0;
    return f;
}

// An Info frame that states `declaredFrames` and (when lame) the encoder delay and padding.
static std::vector<u8> mp3InfoFrame(u32 declaredFrames, bool lame, u32 delay, u32 padding) {
    std::vector<u8> f = mp3AudioFrame();
    std::memcpy(&f[21], "Info", 4);
    f[28] = 0x01;                                                      // flags: a frame count follows
    f[29] = static_cast<u8>(declaredFrames >> 24); f[30] = static_cast<u8>(declaredFrames >> 16);
    f[31] = static_cast<u8>(declaredFrames >> 8);  f[32] = static_cast<u8>(declaredFrames);
    if (lame) {
        std::memcpy(&f[33], "LAME3.100", 9);
        f[54] = static_cast<u8>(delay >> 4);
        f[55] = static_cast<u8>(((delay & 0xF) << 4) | ((padding >> 8) & 0xF));
        f[56] = static_cast<u8>(padding & 0xFF);
    }
    return f;
}

static std::vector<u8> makeMp3(u32 audioFrames, bool info, bool lame = true, u32 delay = 576, u32 padding = 1105, u32 declared = 0) {
    std::vector<u8> out;
    if (info) { const auto i = mp3InfoFrame(declared ? declared : audioFrames, lame, delay, padding); out.insert(out.end(), i.begin(), i.end()); }
    const auto a = mp3AudioFrame();
    for (u32 i = 0; i < audioFrames; ++i) out.insert(out.end(), a.begin(), a.end());
    return out;
}

static void testMp3() {
    AVER_INFO("=== MP3: silent frames with a hand-built Xing/LAME tag ===");
    {
        std::string why;
        fmt::DecodedStream ds = openMem(makeMp3(10, false), fmt::AudioCodec::Mp3, {}, &why);
        check(ds.source != nullptr, "frames with no tag open (" + why + ")");
        if (ds.source) {
            check(ds.source->channels() == 1 && ds.source->sampleRate() == 44100, "mono, 44100 Hz");
            const usize n = readAll(*ds.source, 500).size();
            check(n == 10 * 1152 || n == 9 * 1152, "and decode as whole frames, untrimmed (" + std::to_string(n) + " frames)");
        }
    }
    {
        // Encoder delay 576 and padding 1105: the output is the audio frames' length less both.
        std::string why;
        fmt::DecodedStream ds = openMem(makeMp3(10, true, true, 576, 1105), fmt::AudioCodec::Mp3, {}, &why);
        check(ds.source != nullptr, "a stream with an Info/LAME tag opens (" + why + ")");
        if (ds.source) {
            const u64 expected = 10ull * 1152 - 576 - 1105;
            check(ds.source->totalFrames() == expected, "the reported length is trimmed by the encoder delay and padding (" + std::to_string(ds.source->totalFrames()) + " vs " + std::to_string(expected) + ")");
            const usize got = readAll(*ds.source, 333).size();
            check(got == expected, "and the decoded length is exactly that (" + std::to_string(got) + ")");
            check(ds.source->seek(0) && readAll(*ds.source, 700).size() == expected, "a seek to the start repeats it exactly: the gapless trim is applied each time");
            check(ds.source->seek(5000), "seek into the middle");
            check(readAll(*ds.source, 411).size() == expected - 5000, "and the rest of the stream follows from there");
            check(!ds.source->seek(expected + 2000), "past the end is refused");
        }
        fmt::DecodeOptions o;
        o.startFrame = 1000; o.durationFrames = 2500;
        fmt::DecodedStream w = openMem(makeMp3(10, true, true, 576, 1105), fmt::AudioCodec::Mp3, o);
        check(w.source != nullptr && readAll(*w.source, 300).size() == 2500, "a start offset and duration window the trimmed stream");
    }
    {
        // The tag with no LAME block states a length but no trim.
        fmt::DecodedStream ds = openMem(makeMp3(6, true, false), fmt::AudioCodec::Mp3);
        check(ds.source != nullptr && readAll(*ds.source, 500).size() == 6 * 1152, "an Info tag without a LAME block: whole frames, nothing trimmed");
    }
    {
        // Resident decode agrees with the streamed one.
        audio::SoundData sd;
        std::string why;
        const bool decoded = fmt::decodeAudioMemory(makeMp3(10, true), fmt::AudioCodec::Mp3, sd, &why);
        check(decoded && sd.frames() == 10u * 1152 - 576 - 1105, "load-time decode gives the same trimmed length (" + why + ")");
    }
}

static void testMp3Hostile() {
    AVER_INFO("=== MP3: hostile input ===");
    const std::vector<u8> good = makeMp3(12, true);
    {
        bool sane = true;
        int opened = 0;
        for (usize len = 0; len <= good.size(); len += 7) {
            std::vector<u8> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
            fmt::DecodedStream ds = openMem(cut, fmt::AudioCodec::Mp3);
            if (!ds.source) continue;
            ++opened;
            if (readAll(*ds.source, 256, 1u << 20).size() > 13u * 1152) sane = false;
        }
        check(sane && opened > 0, "truncated at every 7th byte: no crash, nothing past what the frames hold (" + std::to_string(opened) + " opened)");
    }
    {
        Rng rng;
        bool sane = true;
        for (int i = 0; i < 400; ++i) {
            std::vector<u8> bad = good;
            const int flips = 1 + static_cast<int>(rng.next() % 8);
            for (int f = 0; f < flips; ++f) bad[rng.next() % bad.size()] = static_cast<u8>(rng.next());
            fmt::DecodedStream ds = openMem(bad, fmt::AudioCodec::Mp3);
            if (!ds.source) continue;
            if (readAll(*ds.source, 300, 1u << 20).size() > (1u << 20) * 2) sane = false;
            ds.source->seek(rng.next() % 20000);
            readAll(*ds.source, 300, 1u << 16);
        }
        check(sane, "400 streams with random bytes overwritten, decoded and seeked: no crash, bounded");
    }
    {
        Rng rng;
        int opened = 0;
        for (int i = 0; i < 300; ++i) {
            std::vector<u8> junk(rng.next() % 4000);
            for (u8& b : junk) b = static_cast<u8>(rng.next());
            if ((i & 1) && junk.size() > 3) { junk[0] = 0xFF; junk[1] = 0xFB; junk[2] = 0x90; junk[3] = 0xC0; }
            if ((i & 3) == 2 && junk.size() > 10) std::memcpy(junk.data(), "ID3\x03\x00\x00\x7F\x7F\x7F\x7F", 10);   // an ID3 tag claiming 256 MB
            fmt::DecodedStream ds = openMem(junk, fmt::AudioCodec::Mp3);
            if (ds.source) { ++opened; readAll(*ds.source, 300, 1u << 16); }
        }
        check(true, "300 garbage buffers (some with a frame sync, some with an ID3 tag of 256 MB): no crash (" + std::to_string(opened) + " opened)");
    }
    {
        // A tag that claims 4 billion frames and the largest trim in a file of a few kilobytes.
        fmt::DecodedStream ds = openMem(makeMp3(5, true, true, 4095, 4095, 0xFFFFFFF0u), fmt::AudioCodec::Mp3);
        check(ds.source != nullptr, "an Info tag declaring 4 billion frames still opens");
        if (ds.source) {
            check(ds.source->totalFrames() <= 5ull * 1152 + 2304, "the length it reports is capped by what the file could hold");
            check(readAll(*ds.source, 500, 1u << 20).size() <= 5u * 1152, "and the decode ends with the real frames");
        }
        audio::SoundData sd;
        std::string why;
        check(!fmt::decodeAudioMemory(makeMp3(40, true), fmt::AudioCodec::Mp3, sd, &why, 5000) && sd.samples.empty(),
              "the resident decode stops at its sample limit and keeps nothing");
    }
}

// ---- Vorbis ------------------------------------------------------------------------------------

static void oggPage(std::vector<u8>& out, u8 type, u32 seq, const std::vector<u8>& packet) {
    std::vector<u8> seg;
    usize left = packet.size();
    while (left >= 255) { seg.push_back(255); left -= 255; }
    seg.push_back(static_cast<u8>(left));
    const u8 head[] = {'O', 'g', 'g', 'S', 0, type, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                       static_cast<u8>(seq), 0, 0, 0, 0, 0, 0, 0, static_cast<u8>(seg.size())};
    out.insert(out.end(), head, head + sizeof(head));
    out.insert(out.end(), seg.begin(), seg.end());
    out.insert(out.end(), packet.begin(), packet.end());
}

static void le32(std::vector<u8>& v, u32 x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i))); }

static std::vector<u8> vorbisIdent(u8 channels, u32 rate, u8 blocks) {
    std::vector<u8> p = {1, 'v', 'o', 'r', 'b', 'i', 's'};
    le32(p, 0); p.push_back(channels); le32(p, rate); le32(p, 0); le32(p, 128000); le32(p, 0);
    p.push_back(blocks); p.push_back(1);
    return p;
}

static std::vector<u8> vorbisComment(u32 vendorClaim, const std::string& vendor, u32 countClaim) {
    std::vector<u8> p = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    le32(p, vendorClaim);
    p.insert(p.end(), vendor.begin(), vendor.end());
    le32(p, countClaim);
    p.push_back(1);
    return p;
}

static void testVorbis() {
    AVER_INFO("=== Ogg Vorbis: malformed and hostile input ===");
    auto opens = [](const std::vector<u8>& b, std::string* why = nullptr) { return openMem(b, fmt::AudioCodec::Vorbis, {}, why).source != nullptr; };

    std::string why;
    check(!opens({}, &why) && !opens({'O', 'g', 'g', 'S'}) && !opens(std::vector<u8>(100, 0)), "nothing, a bare capture pattern and zeros are refused");

    std::vector<u8> ident;
    oggPage(ident, 2, 0, vorbisIdent(2, 44100, 0xB8));
    bool accepted = opens(ident, &why);
    check(!accepted, "an identification header with no comment or setup header is refused (" + why + ")");

    std::vector<u8> onlyHeaders = ident;
    oggPage(onlyHeaders, 0, 1, vorbisComment(6, "aver-t", 0));
    accepted = opens(onlyHeaders, &why);
    check(!accepted, "identification and comment headers with no setup header are refused (" + why + ")");

    std::vector<u8> badIdent;
    oggPage(badIdent, 2, 0, vorbisIdent(255, 0xFFFFFFFFu, 0x00));
    oggPage(badIdent, 0, 1, vorbisComment(0xFFFFFFF0u, "x", 0xFFFFFFFFu));
    check(!opens(badIdent), "255 channels, an impossible rate and blocksizes, and a 4 GB vendor string are refused");

    // A setup header whose first codebook declares 16 million entries: the arena turns it into a bounded refusal.
    {
        std::vector<u8> setup = {5, 'v', 'o', 'r', 'b', 'i', 's'};
        BitsLsb bits;
        bits.put(0, 8);                      // one codebook
        bits.put(0x564342, 24);              // sync "BCV"
        bits.put(1, 16);                     // dimensions
        bits.put(0xFFFFFF, 24);              // entries
        bits.put(0, 1); bits.put(0, 1);      // not ordered, not sparse; the lengths never come
        setup.insert(setup.end(), bits.b.begin(), bits.b.end());
        std::vector<u8> hostile = ident;
        oggPage(hostile, 0, 1, vorbisComment(6, "aver-t", 0));
        oggPage(hostile, 0, 2, setup);
        const auto t0 = std::chrono::steady_clock::now();
        accepted = opens(hostile, &why);
        check(!accepted, "a codebook of 16 million entries in a 200-byte file is refused (" + why + ")");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(ms < 3000, "without grinding (" + std::to_string(ms) + " ms)");
    }

    // Truncations, bit flips and garbage over the header bytes, with and without the capture pattern.
    {
        std::vector<u8> base = onlyHeaders;
        Rng rng;
        int opened = 0;   // how many of them the decoder accepted
        for (usize len = 0; len <= base.size(); ++len) opened += opens(std::vector<u8>(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(len)));
        for (int i = 0; i < 400; ++i) {
            std::vector<u8> bad = base;
            for (int f = 0; f < 1 + static_cast<int>(rng.next() % 5); ++f) bad[rng.next() % bad.size()] = static_cast<u8>(rng.next());
            opened += opens(bad);
        }
        for (int i = 0; i < 300; ++i) {
            std::vector<u8> junk(rng.next() % 3000);
            for (u8& b : junk) b = static_cast<u8>(rng.next());
            if ((i & 1) && junk.size() > 4) std::memcpy(junk.data(), "OggS", 4);
            opened += opens(junk);
        }
        check(true, "every truncation, 400 corrupted header sets and 300 garbage buffers: no crash (" + std::to_string(opened) + " opened)");
    }
    {
        audio::SoundData sd;
        std::string w;
        check(!fmt::decodeAudioMemory(onlyHeaders, fmt::AudioCodec::Vorbis, sd, &w) && sd.samples.empty(), "a refused stream decodes to nothing");
    }
}

// ---- the ABI: a streamed FLAC renders what the same audio as a WAV does ----------------------------

#if defined(AVER_TEST_HAS_ABI)

static void writeWav16(const fs::path& p, const std::vector<i32>& L, const std::vector<i32>& R, u32 rate) {
    std::vector<u8> b = {'R', 'I', 'F', 'F'};
    const u32 data = static_cast<u32>(L.size()) * 4;
    le32(b, 36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(b, 16); b.push_back(1); b.push_back(0); b.push_back(2); b.push_back(0);
    le32(b, rate); le32(b, rate * 4); b.push_back(4); b.push_back(0); b.push_back(16); b.push_back(0);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le32(b, data);
    for (usize i = 0; i < L.size(); ++i) {
        const i16 l = static_cast<i16>(L[i]), r = static_cast<i16>(R[i]);
        b.push_back(static_cast<u8>(l & 0xFF)); b.push_back(static_cast<u8>((l >> 8) & 0xFF));
        b.push_back(static_cast<u8>(r & 0xFF)); b.push_back(static_cast<u8>((r >> 8) & 0xFF));
    }
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// Streams `path` through the C seam into a fresh offline mixer and returns what the master bus mixed
// from the first audible block on. The decoder thread runs for real; the output is compared, not timed.
static std::vector<float> streamThroughMixer(const std::string& path, usize wantFrames, int loop = 0) {
    std::vector<float> all;
    if (aver_audio_offline_init(48000, 2) != 1) return all;
    const int voice = aver_audio_stream_play(path.c_str(), 1.0f, 1.0f, loop, AVER_AUDIO_BUS_SFX, 0.0f);
    if (voice == 0) { aver_audio_shutdown(); return all; }
    std::vector<float> block(480 * 2);
    bool started = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (all.size() < wantFrames * 2 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        aver_audio_offline_mix(block.data(), 480);
        bool loud = false;
        for (float v : block) if (v != 0.0f) { loud = true; break; }
        if (!started && !loud) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        started = true;
        all.insert(all.end(), block.begin(), block.end());
        if (!loud && all.size() > 2000) break;
    }
    aver_audio_stop(voice);
    aver_audio_collect();
    aver_audio_shutdown();
    return all;
}

static void testThroughAbi() {
    AVER_INFO("=== through the audio ABI (offline mixer, real decoder thread) ===");
    // 0.25 s of two sine tones at 48000 Hz, as 16-bit samples.
    std::vector<i32> L, R;
    for (u32 i = 0; i < 12000; ++i) {
        L.push_back(static_cast<i32>(std::lround(14000.0 * std::sin(0.031 * i))));
        R.push_back(static_cast<i32>(std::lround(9000.0 * std::sin(0.047 * i + 1.0))));
    }
    const fs::path dir = fs::temp_directory_path() / "aver-decode-abi";
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::vector<u8> flac = flacHeader(4096, 4096, 48000, 2, 16, L.size());
    for (u32 f = 0, at = 0; at < L.size(); ++f, at += 4096) {
        const u32 n = std::min<u32>(4096, static_cast<u32>(L.size()) - at);
        Sub a, b;
        a.x.assign(L.begin() + at, L.begin() + at + n);
        b.x.assign(R.begin() + at, R.begin() + at + n);
        const std::vector<u8> fr = flacFrame(f, n, 1, {a, b});
        flac.insert(flac.end(), fr.begin(), fr.end());
    }
    { std::ofstream o(dir / "tones.flac", std::ios::binary); o.write(reinterpret_cast<const char*>(flac.data()), static_cast<std::streamsize>(flac.size())); }
    writeWav16(dir / "tones.wav", L, R, 48000);

    const std::vector<float> viaWav = streamThroughMixer((dir / "tones.wav").string(), L.size());
    const std::vector<float> viaFlac = streamThroughMixer((dir / "tones.flac").string(), L.size());
    check(viaWav.size() >= L.size() * 2 && viaFlac.size() >= L.size() * 2, "both streams reach the mixer's output");
    bool same = viaWav.size() == viaFlac.size() && !viaWav.empty();
    for (usize i = 0; same && i < std::min(viaWav.size(), viaFlac.size()); ++i) same = viaWav[i] == viaFlac[i];
    check(same, "the FLAC stream and the WAV stream of the same audio mix to identical samples, bit for bit");
    bool audible = false;
    for (float v : viaFlac) if (std::fabs(v) > 0.05f) { audible = true; break; }
    check(audible, "and it is not silence");

    // Short clips load whole, through the same decoders.
    check(aver_audio_offline_init(48000, 2) == 1, "offline mixer for the resident path");
    const int snd = aver_audio_load((dir / "tones.flac").string().c_str());
    check(snd > 0 && near(static_cast<float>(aver_audio_sound_length(snd)), 0.25f, 1.0e-4f), "aver_audio_load decodes a .flac at load time");
    const int bad = aver_audio_load((dir / "missing.flac").string().c_str());
    check(bad == 0, "and a missing file is a clean 0");
    {
        std::ofstream o(dir / "junk.ogg", std::ios::binary);
        for (int i = 0; i < 5000; ++i) o.put(static_cast<char>(i * 31));
    }
    check(aver_audio_load((dir / "junk.ogg").string().c_str()) == 0 && aver_audio_stream_play((dir / "junk.ogg").string().c_str(), 1, 1, 0, AVER_AUDIO_BUS_SFX, 0) == 0,
          "a garbage .ogg is refused by both the loader and the streamer");
    aver_audio_shutdown();
    fs::remove_all(dir, ec);
}

#endif

// ---- an optional file from outside the repo ------------------------------------------------------

static void smokeFile(const std::string& path, int damaged) {
    AVER_INFO("=== out-of-repo file: {} ===", path);
    std::string why;
    fmt::DecodedStream ds = fmt::openDecodedFile(path, {}, &why);
    if (!ds.source) { AVER_ERROR("  could not open: {}", why); ++g_failures; return; }
    const u32 ch = ds.source->channels();
    AVER_INFO("  codec {}  channels {}  rate {}  declared frames {}  loop {}..{}", fmt::audioCodecName(ds.codec), ch, ds.source->sampleRate(),
              ds.source->totalFrames(), ds.loopBegin, ds.loopEnd);
    const std::vector<f32> all = readAll(*ds.source, 4096, 1u << 26);
    f32 peak = 0.0f;
    bool finite = true;
    for (f32 v : all) { if (!std::isfinite(v)) finite = false; peak = std::max(peak, std::fabs(v)); }
    const u64 frames = all.size() / ch;
    AVER_INFO("  decoded frames {}  peak {:.4f}  all finite {}", frames, peak, finite);
    check(frames > 0 && finite && peak > 0.0f && peak <= 1.5f, "it decodes to finite, non-silent audio");
    if (ds.source->totalFrames() != 0) check(frames == ds.source->totalFrames(), "and the decoded length matches the declared one");

    // A seek must land on the audio the stream has there: compare against the full decode.
    bool seekOk = true;
    for (u64 at : {u64(0), frames / 3, frames / 2, frames - 1500}) {
        if (!ds.source->seek(at)) { seekOk = false; AVER_ERROR("  seek({}) refused", at); continue; }
        std::vector<f32> buf(1000 * ch);
        const u32 got = ds.source->read(buf.data(), 1000);
        f32 worst = 0.0f;
        for (u32 i = 0; i < got * ch; ++i) worst = std::max(worst, std::fabs(buf[i] - all[at * ch + i]));
        AVER_INFO("  seek({}) -> read {} frames, worst difference from the straight decode {:.2e}", at, got, worst);
        if (got < 1000 || worst > 1.0e-5f) seekOk = false;
    }
    check(seekOk, "seeking round-trips: the frames after a seek equal the same frames read straight through");

    // The file's own bytes, damaged: truncated and bit-flipped copies decode without a crash.
    std::vector<u8> bytes;
    { std::ifstream f(path, std::ios::binary); bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    Rng rng;
    int opened = 0;
    for (int i = 0; i < damaged && !bytes.empty(); ++i) {
        std::vector<u8> bad(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(rng.next() % bytes.size() + 1));
        for (int k = 0; k < (i % 7); ++k) bad[rng.next() % bad.size()] ^= static_cast<u8>(1u << (rng.next() % 8));
        fmt::DecodedStream d = fmt::openDecodedMemory(bad, ds.codec);
        if (!d.source) continue;
        ++opened;
        readAll(*d.source, 4096, 1u << 24);
        d.source->seek(rng.next() % (frames + 1));
        readAll(*d.source, 4096, 1u << 16);
    }
    check(true, std::to_string(damaged) + " truncated and bit-flipped copies of the file: no crash (" + std::to_string(opened) + " opened)");
}

int main(int argc, char** argv) {
    testDetection();
    testFlacExact();
    testFlacWindowAndLoop();
    testFlacHostile();
    testMp3();
    testMp3Hostile();
    testVorbis();
#if defined(AVER_TEST_HAS_ABI)
    testThroughAbi();
#endif
    // `AudioDecodeTest --file <path>` decodes one file from anywhere and reports it; ctest passes no arguments.
    if (argc >= 3 && std::strcmp(argv[1], "--file") == 0) smokeFile(argv[2], argc >= 4 ? std::atoi(argv[3]) : 150);

    if (g_failures == 0) AVER_INFO("=== all decoder tests passed ===");
    else AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
