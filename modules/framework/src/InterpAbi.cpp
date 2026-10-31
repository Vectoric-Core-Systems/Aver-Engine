// Pose interpolation store behind framework_interp_abi.h. One file-static table, single game thread; Runtime's BodyPoseSync reads it each fixed
// step and each frame (docs/PHYSICS_INTERPOLATION.md).

#include "aver/framework/framework_interp_abi.h"

#include <cstring>
#include <map>

namespace {

struct Entry {
    int32_t mode = AVER_FW_INTERP_INHERIT;
    int32_t resetSerial = 0;
    bool hasPose = false;
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1};
};

struct Store {
    int32_t enabled = -1;
    std::map<int32_t, Entry> entries;   // ordered, so the host walks entities in the same order every run
};

Store& store() {
    static Store s;
    return s;
}

// An entry that says nothing is not kept.
void dropIfEmpty(std::map<int32_t, Entry>::iterator it) {
    if (it->second.mode == AVER_FW_INTERP_INHERIT && it->second.resetSerial == 0 && !it->second.hasPose) store().entries.erase(it);
}

}  // namespace

extern "C" {

int32_t aver_fw_interp_abi_version(void) { return AVER_FW_INTERP_ABI_VERSION; }

void aver_fw_interp_set_enabled(int32_t enabled) { store().enabled = enabled < 0 ? -1 : (enabled != 0 ? 1 : 0); }

int32_t aver_fw_interp_enabled(void) { return store().enabled; }

void aver_fw_interp_set_mode(int32_t entity, int32_t mode) {
    if (entity <= 0 || mode < AVER_FW_INTERP_INHERIT || mode > AVER_FW_INTERP_OFF) return;
    auto& m = store().entries;
    auto it = m.find(entity);
    if (it == m.end()) {
        if (mode == AVER_FW_INTERP_INHERIT) return;
        it = m.emplace(entity, Entry{}).first;
    }
    it->second.mode = mode;
    dropIfEmpty(it);
}

int32_t aver_fw_interp_mode(int32_t entity) {
    const auto& m = store().entries;
    const auto it = m.find(entity);
    return it == m.end() ? AVER_FW_INTERP_INHERIT : it->second.mode;
}

void aver_fw_interp_reset(int32_t entity) {
    if (entity <= 0) return;
    Entry& e = store().entries[entity];
    e.resetSerial = e.resetSerial == 0x7fffffff ? 1 : e.resetSerial + 1;
}

int32_t aver_fw_interp_pose(int32_t entity, float* outPosition3, float* outRotation4) {
    const auto& m = store().entries;
    const auto it = m.find(entity);
    if (it == m.end() || !it->second.hasPose) return 0;
    if (outPosition3) std::memcpy(outPosition3, it->second.pos, sizeof it->second.pos);
    if (outRotation4) std::memcpy(outRotation4, it->second.rot, sizeof it->second.rot);
    return 1;
}

int32_t aver_fw_interp_entries(int32_t* outEntity, int32_t* outMode, int32_t* outResetSerial, int32_t cap) {
    int32_t n = 0;
    for (const auto& [entity, e] : store().entries) {
        if (e.mode == AVER_FW_INTERP_INHERIT) continue;
        if (n < cap) {
            if (outEntity) outEntity[n] = entity;
            if (outMode) outMode[n] = e.mode;
            if (outResetSerial) outResetSerial[n] = e.resetSerial;
        }
        ++n;
    }
    return n;
}

int32_t aver_fw_interp_reset_serial(int32_t entity) {
    const auto& m = store().entries;
    const auto it = m.find(entity);
    return it == m.end() ? 0 : it->second.resetSerial;
}

void aver_fw_interp_publish(int32_t entity, const float* position3, const float* rotation4) {
    if (entity <= 0) return;
    auto& m = store().entries;
    auto it = m.find(entity);
    if (!position3) {
        if (it != m.end()) { it->second.hasPose = false; dropIfEmpty(it); }
        return;
    }
    if (it == m.end()) it = m.emplace(entity, Entry{}).first;
    Entry& e = it->second;
    e.hasPose = true;
    std::memcpy(e.pos, position3, sizeof e.pos);
    if (rotation4) std::memcpy(e.rot, rotation4, sizeof e.rot);
}

void aver_fw_interp_clear(void) {
    Store& s = store();
    s.entries.clear();
    s.enabled = -1;
}

}  // extern "C"
