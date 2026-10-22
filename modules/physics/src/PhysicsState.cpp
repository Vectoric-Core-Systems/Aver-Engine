// PhysicsState.cpp -- snapshots of the whole simulation, exact stepping, the state hash and the hash log
// (physics_state_abi.h; wave 11 WORLD_STATE_DEBUG phase D). PhysicsStateEngine.cpp holds the manifest and the engine section;
// PhysicsStateRecorder.hpp the byte buffers.
//
// THE BLOB. A 64-byte header, then the payload:
//
//   header   "APST", state layout version, sizeof(JPH::Real), Jolt version triple, the determinism-build flags, the parts held,
//            topology epoch, constraint epoch, fixed steps at the capture, payload size, FNV-1a 64 of the payload
//   payload  manifest (what must still exist), then three length-prefixed sections: (1) Jolt's PhysicsSystem::SaveState,
//            (2) one CharacterVirtual::SaveState per manifest character, (3) the engine section
//
// The hash is over the PAYLOAD only, so it does not move with the capture time or the step counter: two runs, two machines
// and two thread counts compare by it. A single-precision blob has a different Real size and a different Jolt body layout and
// is refused by a double-precision build (BUILD_MISMATCH).
//
// RESTORE IS ALL-OR-NOTHING: the manifest is checked against the live world and the engine section is parsed in full BEFORE
// Jolt is asked to restore anything, because Jolt's own restore stops halfway at a missing body and restores constraints by
// array index. An imported blob is also checked against its checksum, so a flipped byte is CORRUPT before it reaches Jolt.
#include "PhysicsConfig.hpp"
#include "PhysicsStateRecorder.hpp"
#include "SoftBodyAero.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"

#include <Jolt/Physics/StateRecorder.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr size_t kHeaderBytes = 64;
constexpr char kMagic[4] = {'A', 'P', 'S', 'T'};
constexpr uint32_t kKnownParts = AVER_PHYS_SNAP_ALL | AVER_PHYS_SNAP_STATICS;

// The hash of the payload alone (the bytes after the header).
uint64_t payloadHash(const uint8_t* blob, size_t size) { return fnv1a64(blob + kHeaderBytes, size - kHeaderBytes); }

// Only the build property that changes the arithmetic belongs in the header: the other optional features (profiler, debug
// renderer, stats) leave the state bytes alone.
uint32_t determinismBuildFlags() { return aver_phys_build_flags() & AVER_PHYS_BUILD_CROSS_PLATFORM; }

struct Header {
    uint32_t layout = 0, realSize = 0, major = 0, minor = 0, patch = 0, buildFlags = 0, snapFlags = 0;
    uint32_t topologyEpoch = 0, constraintEpoch = 0, payloadSize = 0;
    int64_t steps = 0;
    uint64_t hash = 0;
};

template <class T> T readAt(const uint8_t* p, size_t off) { T v; std::memcpy(&v, p + off, sizeof v); return v; }
template <class T> void writeAt(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof v); }

// A malformed header is BAD_HEADER, a well-formed one from another build is BUILD_MISMATCH.
int32_t parseHeader(const uint8_t* d, size_t n, Header& h) {
    if (n < kHeaderBytes || std::memcmp(d, kMagic, sizeof kMagic) != 0) return AVER_PHYS_RESTORE_BAD_HEADER;
    h.layout = readAt<uint32_t>(d, 4);
    h.realSize = readAt<uint32_t>(d, 8);
    h.major = readAt<uint32_t>(d, 12);
    h.minor = readAt<uint32_t>(d, 16);
    h.patch = readAt<uint32_t>(d, 20);
    h.buildFlags = readAt<uint32_t>(d, 24);
    h.snapFlags = readAt<uint32_t>(d, 28);
    h.topologyEpoch = readAt<uint32_t>(d, 32);
    h.constraintEpoch = readAt<uint32_t>(d, 36);
    h.steps = readAt<int64_t>(d, 40);
    h.payloadSize = readAt<uint32_t>(d, 48);
    h.hash = readAt<uint64_t>(d, 56);
    if (h.payloadSize != n - kHeaderBytes) return AVER_PHYS_RESTORE_BAD_HEADER;   // truncated, or bytes appended
    if (h.snapFlags == 0 || (h.snapFlags & ~kKnownParts)) return AVER_PHYS_RESTORE_BAD_HEADER;
    if (h.layout != static_cast<uint32_t>(kStateLayoutVersion) || h.realSize != sizeof(JPH::Real) ||
        h.major != static_cast<uint32_t>(JPH_VERSION_MAJOR) || h.minor != static_cast<uint32_t>(JPH_VERSION_MINOR) ||
        h.patch != static_cast<uint32_t>(JPH_VERSION_PATCH) ||
        h.buildFlags != determinismBuildFlags())
        return AVER_PHYS_RESTORE_BUILD_MISMATCH;
    return AVER_PHYS_RESTORE_OK;
}

// What goes into Jolt's stream: dynamic and kinematic bodies unless STATICS, and the contacts that touch at least one of them
// (a dynamic-on-ground contact must survive; static-on-static never exists).
class SaveFilter final : public JPH::StateRecorderFilter {
public:
    bool ShouldSaveBody(const JPH::Body& body) const override { return !body.IsStatic(); }
    bool ShouldSaveContact(const JPH::BodyID& a, const JPH::BodyID& b) const override {
        const JPH::BodyLockInterfaceNoLock& locks = g_world->system.GetBodyLockInterfaceNoLock();
        const JPH::Body* ba = locks.TryGetBody(a);
        const JPH::Body* bb = locks.TryGetBody(b);
        return (ba && !ba->IsStatic()) || (bb && !bb->IsStatic());
    }
};

// ---- the snapshot table ------------------------------------------------------------------------------------------------

struct Snap {
    std::vector<uint8_t> blob;
    uint64_t worldId = 0;
    uint32_t flags = 0, topologyEpoch = 0, constraintEpoch = 0;
    int64_t steps = 0;
    uint32_t payloadSize = 0;
    uint64_t hash = 0;
    uint32_t bodies = 0, characters = 0, vehicles = 0, joints = 0, ragdolls = 0;
};

std::unordered_map<int32_t, Snap> g_snaps;
int32_t g_nextSnap = 1;
int32_t g_lastResult = AVER_PHYS_RESTORE_OK;

// The hash log: one "step hash" line per fixed step while open.
std::ofstream g_hashLog;

void purgeStale() {
    for (auto it = g_snaps.begin(); it != g_snaps.end();) {
        if (!g_world || it->second.worldId != g_world->id) it = g_snaps.erase(it);
        else ++it;
    }
}

Snap* findSnap(int32_t h) {
    if (!g_world) return nullptr;
    const auto it = g_snaps.find(h);
    if (it == g_snaps.end() || it->second.worldId != g_world->id) return nullptr;
    return &it->second;
}

void fillCounts(Snap& s, const StateManifest& m) {
    s.bodies = static_cast<uint32_t>(m.bodyIds.size());
    s.characters = static_cast<uint32_t>(m.characters.size());
    s.vehicles = static_cast<uint32_t>(m.vehicles.size());
    s.joints = static_cast<uint32_t>(m.joints.size());
    s.ragdolls = static_cast<uint32_t>(m.ragdolls.size());
}

void abiErrorForResult(int32_t rc) {
    if (rc == AVER_PHYS_RESTORE_OK) setAbiError(AbiError::Ok);
    else if (rc == AVER_PHYS_RESTORE_BAD_HANDLE) setAbiError(AbiError::BadHandle);
    else if (rc == AVER_PHYS_RESTORE_BUILD_MISMATCH) setAbiError(AbiError::Unsupported);
    else setAbiError(AbiError::InvalidArgument);
}

// ---- capture ---------------------------------------------------------------------------------------------------------

// Writes header + payload for the live world into `blob` (cleared first, capacity kept). False if Jolt's stream failed.
bool capturePayload(std::vector<uint8_t>& blob, uint32_t flags, StateManifest& m) {
    World& w = *g_world;
    blob.clear();
    blob.resize(kHeaderBytes, 0);
    ByteWriter bw(blob);

    stateBuildManifest(flags, m);
    stateWriteManifest(bw, m);

    // (1) Jolt: global state (gravity, previous step), bodies, the contact cache, constraints.
    {
        const size_t lenAt = bw.size();
        bw.u32(0);
        const size_t start = bw.size();
        if (flags & (AVER_PHYS_SNAP_BODIES | AVER_PHYS_SNAP_CONTACTS | AVER_PHYS_SNAP_CONSTRAINTS)) {
            JPH::EStateRecorderState state = JPH::EStateRecorderState::Global;
            if (flags & AVER_PHYS_SNAP_BODIES) state |= JPH::EStateRecorderState::Bodies;
            if (flags & AVER_PHYS_SNAP_CONTACTS) state |= JPH::EStateRecorderState::Contacts;
            if (flags & AVER_PHYS_SNAP_CONSTRAINTS) state |= JPH::EStateRecorderState::Constraints;
            ByteStateRecorder rec(blob);
            SaveFilter filter;
            w.system.SaveState(rec, state, (flags & AVER_PHYS_SNAP_STATICS) ? nullptr : &filter);
            if (rec.IsFailed()) return false;
        }
        bw.patchU32(lenAt, static_cast<uint32_t>(bw.size() - start));
    }

    // (2) CharacterVirtual, which PhysicsSystem::SaveState does not include.
    if (flags & AVER_PHYS_SNAP_CHARACTERS) {
        for (const StateManifest::Char& mc : m.characters) {
            const size_t lenAt = bw.size();
            bw.u32(0);
            const size_t start = bw.size();
            ByteStateRecorder rec(blob);
            w.characters.at(mc.handle)->SaveState(rec);
            if (rec.IsFailed()) return false;
            bw.patchU32(lenAt, static_cast<uint32_t>(bw.size() - start));
        }
    }

    // (3) what Jolt cannot see.
    {
        const size_t lenAt = bw.size();
        bw.u32(0);
        const size_t start = bw.size();
        if (flags & AVER_PHYS_SNAP_ENGINE) stateWriteEngine(bw, m);
        bw.patchU32(lenAt, static_cast<uint32_t>(bw.size() - start));
    }

    uint8_t* p = blob.data();
    std::memcpy(p, kMagic, sizeof kMagic);
    writeAt<uint32_t>(p, 4, static_cast<uint32_t>(kStateLayoutVersion));
    writeAt<uint32_t>(p, 8, static_cast<uint32_t>(sizeof(JPH::Real)));
    writeAt<uint32_t>(p, 12, JPH_VERSION_MAJOR);
    writeAt<uint32_t>(p, 16, JPH_VERSION_MINOR);
    writeAt<uint32_t>(p, 20, JPH_VERSION_PATCH);
    writeAt<uint32_t>(p, 24, determinismBuildFlags());
    writeAt<uint32_t>(p, 28, flags);
    writeAt<uint32_t>(p, 32, w.topologyEpoch);
    writeAt<uint32_t>(p, 36, w.constraintEpoch);
    writeAt<int64_t>(p, 40, w.stepsTotal);
    writeAt<uint32_t>(p, 48, static_cast<uint32_t>(blob.size() - kHeaderBytes));
    writeAt<uint32_t>(p, 52, 0);
    writeAt<uint64_t>(p, 56, payloadHash(blob.data(), blob.size()));
    return true;
}

// ---- restore ---------------------------------------------------------------------------------------------------------

void clearEventQueues() {
    World& w = *g_world;
    {
        std::lock_guard<std::mutex> lock(w.eventMutex);
        w.contacts.clear();
        w.overlaps.clear();
    }
    vehicleBeginCall();         // vehicle events and raw impacts
    softbody::beginCall();      // soft-body impact events
    clearJointBreakEvents();    // the resimulated steps will produce them again
}

// Returns an AVER_PHYS_RESTORE_* result. Nothing is applied unless it validates; once Jolt has been asked, only CORRUPT can follow.
int32_t doRestore(const Snap& s, uint32_t options) {
    World& w = *g_world;
    const uint8_t* payload = s.blob.data() + kHeaderBytes;
    ByteReader r(payload, s.blob.size() - kHeaderBytes);

    StateManifest m;
    if (!stateReadManifest(r, m)) return AVER_PHYS_RESTORE_CORRUPT;
    const uint32_t flags = s.flags;

    const uint8_t* joltData = nullptr;
    uint32_t joltSize = r.u32();
    if (!r.take(joltSize, joltData)) return AVER_PHYS_RESTORE_CORRUPT;
    struct Slice { const uint8_t* data = nullptr; uint32_t size = 0; };
    std::vector<Slice> charData;
    if (flags & AVER_PHYS_SNAP_CHARACTERS) {
        charData.resize(m.characters.size());
        for (Slice& c : charData) {
            c.size = r.u32();
            if (!r.take(c.size, c.data)) return AVER_PHYS_RESTORE_CORRUPT;
        }
    }
    const uint8_t* engData = nullptr;
    const uint32_t engSize = r.u32();
    if (!r.take(engSize, engData)) return AVER_PHYS_RESTORE_CORRUPT;

    // 1. The live world must still be the captured one.
    if (const int32_t rc = stateCheckManifest(m, flags, options, s.constraintEpoch); rc != AVER_PHYS_RESTORE_OK) return rc;
    // 2. The engine section parses in full and matches the live vehicles.
    EngineStage stage;
    if ((flags & AVER_PHYS_SNAP_ENGINE) && !stateStageEngine(engData, engSize, m, stage)) return AVER_PHYS_RESTORE_TOPOLOGY;

    // 3. Apply. Jolt first (bodies, contacts, constraints), then the characters, then the engine's own state.
    if (joltSize > 0) {
        ByteStateRecorder rec(joltData, joltSize);
        if (!w.system.RestoreState(rec, nullptr) || rec.IsFailed()) {
            AVER_ERROR("[Physics] snapshot restore: Jolt refused the stream; the world may be partly restored");
            return AVER_PHYS_RESTORE_CORRUPT;
        }
    }
    if (flags & AVER_PHYS_SNAP_CHARACTERS) {
        for (size_t i = 0; i < m.characters.size(); ++i) {
            ByteStateRecorder rec(charData[i].data, charData[i].size);
            w.characters.at(m.characters[i].handle)->RestoreState(rec);
            if (rec.IsFailed()) {
                AVER_ERROR("[Physics] snapshot restore: a character's stream is short; the world may be partly restored");
                return AVER_PHYS_RESTORE_CORRUPT;
            }
        }
    }
    if (flags & AVER_PHYS_SNAP_ENGINE) stateApplyEngine(stage);

    w.stepsTotal = s.steps;
    clearEventQueues();
    return AVER_PHYS_RESTORE_OK;
}

} // namespace

namespace aver::physics::detail {

uint64_t nextWorldId() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

void stateAfterFixedStep() {
    if (!g_hashLog.is_open() || !g_world) return;
    static std::vector<uint8_t> scratch;
    static StateManifest manifest;
    if (!capturePayload(scratch, AVER_PHYS_SNAP_ALL, manifest)) return;
    char line[48];
    std::snprintf(line, sizeof line, "%lld %016llx\n", static_cast<long long>(g_world->stepsTotal),
                  static_cast<unsigned long long>(payloadHash(scratch.data(), scratch.size())));
    g_hashLog << line;
    g_hashLog.flush();
}

void stateOnShutdown() {
    diagOnWorldShutdown();   // closes Jolt's determinism log in the diagnostics preset; nothing elsewhere
    g_snaps.clear();
    if (g_hashLog.is_open()) g_hashLog.close();
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_state_abi_version(void) { return AVER_PHYS_STATE_ABI_VERSION; }

int32_t aver_phys_snapshot_capture(int32_t reuse, uint32_t flags) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); g_lastResult = AVER_PHYS_RESTORE_BAD_HANDLE; return 0; }
    if (flags == 0) flags = AVER_PHYS_SNAP_ALL;
    if (flags & ~kKnownParts) { setAbiError(AbiError::InvalidArgument); return 0; }
    purgeStale();

    Snap* s = reuse != 0 ? findSnap(reuse) : nullptr;
    int32_t handle = reuse;
    if (!s) {
        handle = g_nextSnap++;
        s = &g_snaps[handle];
        s->worldId = g_world->id;
    }
    StateManifest m;
    if (!capturePayload(s->blob, flags, m)) {
        g_snaps.erase(handle);
        g_lastResult = AVER_PHYS_RESTORE_CORRUPT;
        setAbiError(AbiError::AllocationFailed);
        return 0;
    }
    s->flags = flags;
    s->topologyEpoch = g_world->topologyEpoch;
    s->constraintEpoch = g_world->constraintEpoch;
    s->steps = g_world->stepsTotal;
    s->payloadSize = static_cast<uint32_t>(s->blob.size() - kHeaderBytes);
    s->hash = readAt<uint64_t>(s->blob.data(), 56);
    fillCounts(*s, m);
    g_lastResult = AVER_PHYS_RESTORE_OK;
    setAbiError(AbiError::Ok);
    return handle;
}

int32_t aver_phys_snapshot_restore(int32_t snapshot, uint32_t options) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); g_lastResult = AVER_PHYS_RESTORE_BAD_HANDLE; return 0; }
    if (options & ~static_cast<uint32_t>(AVER_PHYS_RESTORE_STRICT)) { setAbiError(AbiError::InvalidArgument); return 0; }
    const Snap* s = findSnap(snapshot);
    if (!s) { setAbiError(AbiError::BadHandle); g_lastResult = AVER_PHYS_RESTORE_BAD_HANDLE; return 0; }
    const int32_t rc = doRestore(*s, options);
    g_lastResult = rc;
    abiErrorForResult(rc);
    if (rc != AVER_PHYS_RESTORE_OK)
        AVER_WARN("[Physics] snapshot {} was not restored (result {}); nothing was changed unless the result is -5", snapshot, rc);
    return rc == AVER_PHYS_RESTORE_OK ? 1 : 0;
}

int32_t aver_phys_snapshot_last_result(void) { return g_lastResult; }

int32_t aver_phys_snapshot_release(int32_t snapshot) {
    if (!findSnap(snapshot)) { setAbiError(AbiError::BadHandle); return 0; }
    g_snaps.erase(snapshot);
    setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_snapshot_release_all(void) {
    purgeStale();
    const int32_t n = static_cast<int32_t>(g_snaps.size());
    g_snaps.clear();
    setAbiError(AbiError::Ok);
    return n;
}

int32_t aver_phys_snapshot_count(void) {
    purgeStale();
    return static_cast<int32_t>(g_snaps.size());
}

int32_t aver_phys_snapshot_bytes(int32_t snapshot) {
    const Snap* s = findSnap(snapshot);
    return s ? static_cast<int32_t>(s->blob.size()) : 0;
}

int32_t aver_phys_snapshot_read(int32_t snapshot, uint8_t* outData, int32_t cap) {
    const Snap* s = findSnap(snapshot);
    if (!s) { setAbiError(AbiError::BadHandle); return 0; }
    const int32_t need = static_cast<int32_t>(s->blob.size());
    if (!outData) return need;
    if (cap < need) { setAbiError(AbiError::OutOfRange); return 0; }
    std::memcpy(outData, s->blob.data(), s->blob.size());
    setAbiError(AbiError::Ok);
    return need;
}

int32_t aver_phys_snapshot_import(const uint8_t* data, int32_t size) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); g_lastResult = AVER_PHYS_RESTORE_BAD_HANDLE; return 0; }
    if (!data || size <= 0) { setAbiError(AbiError::NullPointer); g_lastResult = AVER_PHYS_RESTORE_BAD_HEADER; return 0; }
    Header h;
    int32_t rc = parseHeader(data, static_cast<size_t>(size), h);
    if (rc == AVER_PHYS_RESTORE_OK && fnv1a64(data + kHeaderBytes, static_cast<size_t>(size) - kHeaderBytes) != h.hash)
        rc = AVER_PHYS_RESTORE_CORRUPT;
    StateManifest m;
    if (rc == AVER_PHYS_RESTORE_OK) {
        ByteReader r(data + kHeaderBytes, static_cast<size_t>(size) - kHeaderBytes);
        if (!stateReadManifest(r, m)) rc = AVER_PHYS_RESTORE_CORRUPT;
    }
    g_lastResult = rc;
    if (rc != AVER_PHYS_RESTORE_OK) { abiErrorForResult(rc); return 0; }

    purgeStale();
    const int32_t handle = g_nextSnap++;
    Snap& s = g_snaps[handle];
    s.worldId = g_world->id;
    s.blob.assign(data, data + size);
    s.flags = h.snapFlags;
    s.topologyEpoch = h.topologyEpoch;
    s.constraintEpoch = h.constraintEpoch;
    s.steps = h.steps;
    s.payloadSize = h.payloadSize;
    s.hash = h.hash;
    fillCounts(s, m);
    setAbiError(AbiError::Ok);
    return handle;
}

int32_t aver_phys_snapshot_info(int32_t snapshot, int32_t* outValues, int32_t n) {
    const Snap* s = findSnap(snapshot);
    if (!s) { setAbiError(AbiError::BadHandle); return 0; }
    if (!outValues || n <= 0) { setAbiError(AbiError::NullPointer); return 0; }
    int32_t v[AVER_PHYS_SI_COUNT] = {};
    v[AVER_PHYS_SI_FLAGS] = static_cast<int32_t>(s->flags);
    v[AVER_PHYS_SI_BODIES] = static_cast<int32_t>(s->bodies);
    v[AVER_PHYS_SI_CHARACTERS] = static_cast<int32_t>(s->characters);
    v[AVER_PHYS_SI_VEHICLES] = static_cast<int32_t>(s->vehicles);
    v[AVER_PHYS_SI_JOINTS] = static_cast<int32_t>(s->joints);
    v[AVER_PHYS_SI_RAGDOLLS] = static_cast<int32_t>(s->ragdolls);
    v[AVER_PHYS_SI_TOPOLOGY_EPOCH] = static_cast<int32_t>(s->topologyEpoch);
    v[AVER_PHYS_SI_CONSTRAINT_EPOCH] = static_cast<int32_t>(s->constraintEpoch);
    v[AVER_PHYS_SI_PRECISION] = aver_phys_precision();
    v[AVER_PHYS_SI_STEPS_LOW] = static_cast<int32_t>(static_cast<uint64_t>(s->steps) & 0xFFFFFFFFull);
    v[AVER_PHYS_SI_STEPS_HIGH] = static_cast<int32_t>(static_cast<uint64_t>(s->steps) >> 32);
    v[AVER_PHYS_SI_BYTES] = static_cast<int32_t>(s->blob.size());
    const int32_t w = n < AVER_PHYS_SI_COUNT ? n : AVER_PHYS_SI_COUNT;
    for (int32_t i = 0; i < w; ++i) outValues[i] = v[i];
    setAbiError(AbiError::Ok);
    return w;
}

uint64_t aver_phys_snapshot_hash(int32_t snapshot) {
    const Snap* s = findSnap(snapshot);
    if (!s) { setAbiError(AbiError::BadHandle); return 0; }
    return s->hash;
}

int32_t aver_phys_snapshot_diff(int32_t a, int32_t b) {
    const Snap* sa = findSnap(a);
    const Snap* sb = findSnap(b);
    if (!sa || !sb) { setAbiError(AbiError::BadHandle); return -2; }
    const size_t na = sa->blob.size() - kHeaderBytes, nb = sb->blob.size() - kHeaderBytes;
    const size_t common = na < nb ? na : nb;
    const uint8_t* pa = sa->blob.data() + kHeaderBytes;
    const uint8_t* pb = sb->blob.data() + kHeaderBytes;
    for (size_t i = 0; i < common; ++i)
        if (pa[i] != pb[i]) return static_cast<int32_t>(i);
    return na == nb ? -1 : static_cast<int32_t>(common);
}

uint64_t aver_phys_state_hash(uint32_t flags) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return 0; }
    if (flags == 0) flags = AVER_PHYS_SNAP_ALL;
    if (flags & ~kKnownParts) { setAbiError(AbiError::InvalidArgument); return 0; }
    static std::vector<uint8_t> scratch;   // reused: a hash every step costs no allocation once warm
    static StateManifest manifest;
    if (!capturePayload(scratch, flags, manifest)) { setAbiError(AbiError::AllocationFailed); return 0; }
    setAbiError(AbiError::Ok);
    return payloadHash(scratch.data(), scratch.size());
}

int32_t aver_phys_hash_log(const char* pathOrNull) {
    if (!pathOrNull) {
        if (g_hashLog.is_open()) g_hashLog.close();
        setAbiError(AbiError::Ok);
        return 1;
    }
    if (!g_world) { setAbiError(AbiError::NotInitialised); return 0; }
    if (g_hashLog.is_open()) g_hashLog.close();
    g_hashLog.clear();
    g_hashLog.open(pathOrNull, std::ios::binary | std::ios::trunc);
    if (!g_hashLog.is_open()) {
        AVER_WARN("[Physics] hash log: cannot open '{}'", pathOrNull);
        setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    setAbiError(AbiError::Ok);
    return 1;
}

// The replay primitive. aver_phys_step runs whole fixed steps from its accumulator, and its eight-step clamp bounds only what ONE
// CALL ADDS (dt), not what the accumulator already holds, so a single call runs any count: this sets the accumulator to n whole
// steps and half a step to spare (float rounding over 600 subtractions is a few 1e-4 of a step, so it can neither lose nor add
// one), adds a sliver so the call is legal, and puts the real accumulator back at the end. One call means one prologue: the event
// queues are cleared once and describe all n steps, exactly as they do for a frame that ran n steps. The loop below only
// repeats if a call came up short, which the margin makes impossible; it is there so the contract does not rest on that argument.
int32_t aver_phys_step_exact(int32_t steps) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return 0; }
    if (steps < 1 || steps > 600) { setAbiError(AbiError::InvalidArgument); return 0; }
    const float saved = g_world->accumulator;
    const float fixed = g_world->fixedStep;
    int32_t done = 0;
    while (done < steps) {
        g_world->accumulator = (static_cast<float>(steps - done) + 0.5f) * fixed;
        const int32_t ran = aver_phys_step(fixed * 1.0e-3f);
        if (ran <= 0) break;
        done += ran;
    }
    g_world->accumulator = saved;
    setAbiError(AbiError::Ok);
    return done;
}

int64_t aver_phys_step_total(void) { return g_world ? g_world->stepsTotal : 0; }

int32_t aver_phys_accumulator_set(float seconds) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return 0; }
    if (!(seconds == seconds)) { setAbiError(AbiError::InvalidArgument); return 0; }   // NaN
    const float top = g_world->fixedStep * 8.0f;
    g_world->accumulator = seconds < 0.0f ? 0.0f : (seconds > top ? top : seconds);
    setAbiError(AbiError::Ok);
    return 1;
}

float aver_phys_accumulator_get(void) { return g_world ? g_world->accumulator : 0.0f; }

} // extern "C"
