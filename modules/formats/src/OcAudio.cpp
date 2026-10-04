// The .ocaudio container, plus importing source audio through the platform's media stack.

#include "aver/formats/OcAudio.hpp"
#include "aver/formats/Wav.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#  include <mferror.h>
#endif

namespace aver::fmt {
namespace {

// Appends one little-endian u32.
void putU32(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x)); v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x >> 16)); v.push_back(static_cast<u8>(x >> 24));
}
// Reads one little-endian u32 at `p` and advances it. Returns false past the end.
bool getU32(const std::vector<u8>& v, usize& p, u32& out) {
    if (p + 4 > v.size()) return false;
    out = static_cast<u32>(v[p]) | (static_cast<u32>(v[p + 1]) << 8) |
          (static_cast<u32>(v[p + 2]) << 16) | (static_cast<u32>(v[p + 3]) << 24);
    p += 4;
    return true;
}

// Sets `why` and returns false.
bool fail(std::string* why, const char* msg) { if (why) *why = msg; return false; }

// The file extension including the dot, lowercased. Empty when there is none.
std::string lowerExtension(const std::string& path) {
    const usize dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return e;
}

#if defined(_WIN32)

// Starts Media Foundation once per process. Never shut down.
bool mfStart() {
    static const bool started = [] {
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }();
    return started;
}

// Releases a COM pointer and nulls it.
template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Decodes a file to interleaved 32-bit float PCM through Media Foundation.
AudioImportResult mfDecode(const std::string& path, audio::SoundData& out) {
    AudioImportResult r; r.decoder = "media foundation";
    if (!mfStart()) { r.error = "Media Foundation would not start"; return r; }

    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide <= 0) { r.error = "the path is not valid UTF-8"; return r; }
    std::wstring wpath(static_cast<usize>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);

    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader)) || !reader) {
        r.error = "no decoder for this file";
        return r;
    }

    IMFMediaType* want = nullptr;
    if (FAILED(MFCreateMediaType(&want))) { safeRelease(reader); r.error = "out of memory"; return r; }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    const HRESULT setType = reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want);
    safeRelease(want);
    if (FAILED(setType)) {
        safeRelease(reader);
        r.error = "the file has no audio stream this machine can decode";
        return r;
    }

    // The negotiated rate and channel count, which the float request does not pin.
    IMFMediaType* got = nullptr;
    UINT32 channels = 0, rate = 0;
    if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got)) && got) {
        got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        safeRelease(got);
    }
    if (channels == 0 || rate == 0) {
        safeRelease(reader);
        r.error = "the decoder reported no channel count or sample rate";
        return r;
    }

    out = audio::SoundData{};
    out.channels = channels;
    out.sampleRate = rate;

    for (;;) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                      0, nullptr, &flags, nullptr, &sample))) {
            safeRelease(reader);
            r.error = "the decoder failed part way through";
            return r;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { safeRelease(sample); break; }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            safeRelease(sample); safeRelease(reader);
            r.error = "the stream changes format part way through";
            return r;
        }
        if (!sample) continue;   // a gap, which is legal and carries no data

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* data = nullptr; DWORD len = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)) && data) {
                const usize count = len / sizeof(f32);
                const usize base = out.samples.size();
                out.samples.resize(base + count);
                std::memcpy(out.samples.data() + base, data, count * sizeof(f32));
                buffer->Unlock();
            }
            safeRelease(buffer);
        }
        safeRelease(sample);
    }

    safeRelease(reader);
    if (out.samples.empty()) { r.error = "the file decoded to no audio at all"; return r; }
    // Drop a trailing partial frame.
    out.samples.resize((out.samples.size() / channels) * channels);
    r.ok = true;
    return r;
}

#endif // _WIN32

} // namespace

// ---------------------------------------------------------------- the container

// Encodes sound data into an .ocaudio container. Returns false with `why` set.
bool writeOcAudio(const audio::SoundData& in, const std::string& sourceName, std::vector<u8>& out,
                  std::string* why) {
    if (!in.valid()) return fail(why, "the sound data is empty or malformed");

    Avr1File f;
    f.subtype = kAvrSubtypeAudio;
    f.contentVersion = 1;

    std::vector<u8> header;
    putU32(header, in.channels);
    putU32(header, in.sampleRate);
    putU32(header, in.frames());
    putU32(header, in.loopBegin);
    putU32(header, in.loopEnd);
    putU32(header, static_cast<u32>(sourceName.size()));
    header.insert(header.end(), sourceName.begin(), sourceName.end());
    f.add(kOcAudioChunkHeader, std::move(header), kAvrChunkRequired);

    std::vector<u8> pcm(in.samples.size() * sizeof(f32));
    std::memcpy(pcm.data(), in.samples.data(), pcm.size());
    f.add(kOcAudioChunkSamples, std::move(pcm), kAvrChunkRequired);

    return writeAvr1(f, out, why);
}

// Writes sound data to an .ocaudio file. Returns false with `why` set.
bool saveOcAudio(const std::string& path, const audio::SoundData& in, const std::string& sourceName,
                 std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcAudio(in, sourceName, bytes, why)) return false;
    Avr1File f;
    if (!parseAvr1(bytes.data(), bytes.size(), f, why)) return false;
    return saveAvr1(path, f, why);
}

// Decodes an .ocaudio container into `out`. Returns false with `why` set on a malformed file.
bool parseOcAudio(const u8* bytes, usize size, audio::SoundData& out, std::string* why) {
    out = audio::SoundData{};
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeAudio) return fail(why, "not an .ocaudio container");

    const AvrChunk* h = f.find(kOcAudioChunkHeader);
    const AvrChunk* s = f.find(kOcAudioChunkSamples);
    if (!h) return fail(why, "no AHDR chunk");
    if (!s) return fail(why, "no APCM chunk");

    usize p = 0;
    u32 channels = 0, rate = 0, frames = 0, loopBegin = 0, loopEnd = 0, nameLen = 0;
    if (!getU32(h->data, p, channels) || !getU32(h->data, p, rate) || !getU32(h->data, p, frames) ||
        !getU32(h->data, p, loopBegin) || !getU32(h->data, p, loopEnd) || !getU32(h->data, p, nameLen))
        return fail(why, "the AHDR chunk is truncated");
    if (channels == 0 || rate == 0) return fail(why, "the AHDR chunk has no channels or no rate");

    const usize expected = static_cast<usize>(frames) * channels * sizeof(f32);
    if (s->data.size() != expected) return fail(why, "the sample payload does not match the header");

    out.channels = channels;
    out.sampleRate = rate;
    out.loopBegin = loopBegin;
    out.loopEnd = loopEnd;
    out.samples.resize(static_cast<usize>(frames) * channels);
    if (!out.samples.empty()) std::memcpy(out.samples.data(), s->data.data(), expected);
    return true;
}

// Reads an .ocaudio file from disk. Returns false with `why` set.
bool loadOcAudio(const std::string& path, audio::SoundData& out, std::string* why) {
    Avr1File f;
    if (!loadAvr1(path, f, why)) return false;
    std::vector<u8> bytes;
    if (!writeAvr1(f, bytes, why)) return false;
    return parseOcAudio(bytes.data(), bytes.size(), out, why);
}

// ---------------------------------------------------------------- import

// True for an extension the importer will attempt. Only .wav is guaranteed; the rest need a codec.
bool isImportableAudio(const std::string& path) {
    const std::string e = lowerExtension(path);
    return e == ".wav" || e == ".mp3" || e == ".m4a" || e == ".aac" ||
           e == ".wma" || e == ".flac" || e == ".ogg";
}

// The .ocaudio path a source file compiles to.
std::string ocAudioPathFor(const std::string& sourcePath) {
    const usize dot = sourcePath.find_last_of('.');
    const usize slash = sourcePath.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return sourcePath + ".ocaudio";
    return sourcePath.substr(0, dot) + ".ocaudio";
}

// Decodes a source audio file: this module's WAV reader first, then the platform decoder.
AudioImportResult audioImportFile(const std::string& path, audio::SoundData& out) {
    const std::string e = lowerExtension(path);

    if (e == ".wav") {
        AudioImportResult r; r.decoder = "wav";
        const WavResult w = wavReadFile(path, out);
        r.ok = w.ok;
        r.error = w.error;
        if (r.ok) return r;
#if !defined(_WIN32)
        return r;
#else
        AudioImportResult mf = mfDecode(path, out);
        if (mf.ok) return mf;
        r.error += "; and the platform decoder also declined: " + mf.error;
        return r;
#endif
    }

#if defined(_WIN32)
    return mfDecode(path, out);
#else
    AudioImportResult r;
    r.decoder = "none";
    r.error = "only .wav can be imported on this platform";
    return r;
#endif
}

} // namespace aver::fmt
