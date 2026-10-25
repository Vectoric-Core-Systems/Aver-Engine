// PhysicsStateRecorder.hpp -- the byte buffers behind snapshots (physics_state_abi.h). SRC-LOCAL like PhysicsInternal.hpp: it
// names JPH:: types.
//
// ByteStateRecorder is Jolt's StateRecorder over a std::vector<uint8_t> instead of a std::stringstream (StateRecorderImpl): no
// per-field allocation, and capture into a reused vector keeps its capacity, so a ring of snapshots allocates once. The same
// class reads: a pointer and a size, with the usual "attempt to read past the end" EOF and failure flags. In validating mode
// (JPH::StateRecorder::SetValidating) every read compares the stream with the live bytes it is about to overwrite and counts the
// differences, instead of Jolt's breakpoint.
//
// ByteWriter / ByteReader are the same thing for the parts Jolt does not write (the header, the manifest, the engine section).
// Everything is native little-endian: every platform this engine builds for is little-endian, and a blob from another build is
// refused by the header's build check long before an endian question could matter.
#pragma once
#include "PhysicsInternal.hpp"
#include "aver/physics/physics_state_abi.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/StateRecorder.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace aver::physics::detail {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}
    size_t size() const { return out_.size(); }
    void bytes(const void* p, size_t n) {
        if (n == 0) return;
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    template <class T> void put(const T& v) { bytes(&v, sizeof(T)); }
    void u32(uint32_t v) { put(v); }
    void i32(int32_t v) { put(v); }
    void u64(uint64_t v) { put(v); }
    void f32(float v) { put(v); }
    // Overwrites four bytes already written (a length written before its content is known).
    void patchU32(size_t at, uint32_t v) { std::memcpy(out_.data() + at, &v, sizeof v); }

private:
    std::vector<uint8_t>& out_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : p_(data), n_(size) {}
    bool ok() const { return ok_; }
    size_t left() const { return n_ - pos_; }
    bool bytes(void* out, size_t n) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; return false; }
        if (n) std::memcpy(out, p_ + pos_, n);
        pos_ += n;
        return true;
    }
    template <class T> T get() { T v{}; bytes(&v, sizeof(T)); return v; }
    uint32_t u32() { return get<uint32_t>(); }
    int32_t i32() { return get<int32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    float f32() { return get<float>(); }
    // A sub-range of `n` bytes (a length-prefixed section); false when it runs past the end.
    bool take(size_t n, const uint8_t*& at) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; return false; }
        at = p_ + pos_;
        pos_ += n;
        return true;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class ByteStateRecorder final : public JPH::StateRecorder {
public:
    // Write mode: appends to `out`.
    explicit ByteStateRecorder(std::vector<uint8_t>& out) : out_(&out) {}
    // Read mode over a byte range.
    ByteStateRecorder(const uint8_t* data, size_t size) : in_(data), inSize_(size) {}

    void WriteBytes(const void* data, size_t numBytes) override {
        if (!out_) { failed_ = true; return; }
        if (numBytes == 0) return;
        const uint8_t* b = static_cast<const uint8_t*>(data);
        out_->insert(out_->end(), b, b + numBytes);
    }

    void ReadBytes(void* data, size_t numBytes) override {
        if (!in_ || numBytes > inSize_ - pos_) {
            // Past the end: Jolt's convention is EOF and failed, and the destination is left as it was.
            eof_ = true;
            failed_ = true;
            return;
        }
        if (IsValidating()) {
            if (std::memcmp(in_ + pos_, data, numBytes) != 0) {
                if (mismatches_ == 0) firstMismatch_ = pos_;
                ++mismatches_;
            }
        }
        std::memcpy(data, in_ + pos_, numBytes);
        pos_ += numBytes;
    }

    bool IsEOF() const override { return eof_; }
    bool IsFailed() const override { return failed_; }

    // Validating mode: how many reads differed from the live bytes, and the stream offset of the first.
    size_t mismatches() const { return mismatches_; }
    size_t firstMismatch() const { return firstMismatch_; }

private:
    std::vector<uint8_t>* out_ = nullptr;
    const uint8_t* in_ = nullptr;
    size_t inSize_ = 0;
    size_t pos_ = 0;
    bool eof_ = false;
    bool failed_ = false;
    size_t mismatches_ = 0, firstMismatch_ = 0;
};

// ---- what a snapshot expects to find again, and the engine's own section (PhysicsStateEngine.cpp) ------------------

// The manifest is written at the head of every payload. A restore checks it against the live world BEFORE it lets Jolt touch
// anything (physics_state_abi.h: all-or-nothing), because Jolt's non-validating restore looks each stored body up by id, stops
// halfway when one is gone, and restores constraints by array index.
struct StateManifest {
    std::vector<uint32_t> bodyIds;                     // raw Jolt ids of every body the Jolt section holds, ascending
    struct Char { int32_t handle = 0; uint32_t inner = 0; };   // inner: the raw id of the character's inner body (0xFFFFFFFF: none)
    std::vector<Char> characters;                      // handle order
    struct Veh { int32_t handle = 0, body = 0; uint32_t bodyId = 0, wheels = 0; };
    std::vector<Veh> vehicles;                         // finished vehicles, handle order
    std::vector<int32_t> joints, ragdolls;             // ascending
    uint32_t constraintCount = 0;                      // JPH constraints in the system
};

// Fills `m` from the live world for a capture with `flags` (AVER_PHYS_SNAP_*).
void stateBuildManifest(uint32_t flags, StateManifest& m);
void stateWriteManifest(ByteWriter& w, const StateManifest& m);
bool stateReadManifest(ByteReader& r, StateManifest& m);
// AVER_PHYS_RESTORE_OK, or AVER_PHYS_RESTORE_TOPOLOGY when the live world no longer matches `m` for the parts in `flags`
// (and, under AVER_PHYS_RESTORE_STRICT in `options`, when it holds MORE than `m` does). Changes nothing.
int32_t stateCheckManifest(const StateManifest& m, uint32_t flags, uint32_t options, uint32_t constraintEpoch);

// The engine section, staged: parsed and checked in full before anything is applied.
struct EngineStage {
    float accumulator = 0.0f;
    // f / i are the wheel's telemetry of the LAST step: the next step's traction control and ABS read the slip and the contact
    // flag from them (stepBeginOne), so a restore that left them at their pre-restore values would resimulate differently.
    struct Wheel {
        float absFactor = 1.0f, airborne = 0.0f, skidPeak = 0.0f;
        bool hadContact = false, skidding = false;
        float f[AVER_PHYS_VWS_COUNT] = {};
        int32_t i[AVER_PHYS_VWSI_COUNT] = {};
    };
    struct Veh {
        int32_t handle = 0;
        float req[4] = {}, app[4] = {};
        float tcsFactor = 1.0f;
        bool absActive = false, tcsActive = false, flippedSent = false;
        int32_t gear = 0, lastGear = 0;
        float shiftLeft = 0.0f, clutchRelease = 0.0f, lastRpm = 0.0f, lastVel[3] = {};
        float longAccelG = 0.0f, latAccelG = 0.0f, airborneAll = 0.0f, sideSeconds = 0.0f;
        float vs[AVER_PHYS_VS_COUNT] = {};         // the telemetry a reader sees right after the restore (derived, but cheap)
        int32_t vsi[AVER_PHYS_VSI_COUNT] = {};
        std::vector<Wheel> wheels;
    };
    std::vector<Veh> vehicles;
    struct Carry { int32_t handle = 0; bool present = false; float v[3] = {}; };
    std::vector<Carry> carry;
    struct Part { uint64_t nameHash = 0; std::vector<uint8_t> data; };
    std::vector<Part> participants;
};

// Appends the engine section's content (no length prefix) for the world as it is now.
void stateWriteEngine(ByteWriter& w, const StateManifest& m);
// Parses a section written by stateWriteEngine against the manifest; false when it is malformed or a vehicle's wheel count
// no longer matches the live vehicle.
bool stateStageEngine(const uint8_t* data, size_t size, const StateManifest& m, EngineStage& out);
// Writes a staged section into the live world. Cannot fail on its own; a participant that refuses is logged.
void stateApplyEngine(const EngineStage& s);

} // namespace aver::physics::detail
