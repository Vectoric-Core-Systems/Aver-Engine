// Runtime-texture store behind framework_rtex_abi.h (E8). One file-static registry, single game thread; the hosts read it every frame (Runtime GameApp, the
// editor's Play path) and upload the textures it reports changed. Pure CPU, so it is tested without a device.

#include "aver/framework/framework_rtex_abi.h"

#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxKey = 120;

struct Tex {
    std::string key;
    int32_t width = 0, height = 0, format = 0, mips = 0;
    std::vector<uint8_t> pixels;
    int32_t generation = 0;
    bool dirty = false;
    uint64_t gpu = 0;
};

struct Store {
    std::map<int32_t, Tex> byId;
    std::map<std::string, int32_t> byKey;
    int32_t next = 1;
    int32_t maxTextures = AVER_FW_RTEX_DEFAULT_MAX_TEXTURES;
    size_t totalBytes = 0;
    std::vector<int32_t> removed;
};

Store& store() {
    static Store s;
    return s;
}

int32_t bytesPerPixel(int32_t format) {
    switch (format) {
        case AVER_FW_RTEX_FORMAT_RGBA8: case AVER_FW_RTEX_FORMAT_RGBA8_SRGB: return 4;
        case AVER_FW_RTEX_FORMAT_R8: return 1;
        case AVER_FW_RTEX_FORMAT_RGBA16F: return 8;
        default: return 0;
    }
}

// The size in bytes of a width x height image, or -1 when a side is out of range or the product leaves the per-texture limit. 64-bit throughout: a hostile
// 8192 x 8192 x 8 is 512 MB, past what an int32_t holds.
int64_t imageBytes(int32_t w, int32_t h, int32_t format) {
    const int32_t bpp = bytesPerPixel(format);
    if (bpp == 0 || w < 1 || h < 1 || w > AVER_FW_RTEX_MAX_DIMENSION || h > AVER_FW_RTEX_MAX_DIMENSION) return -1;
    const int64_t n = static_cast<int64_t>(w) * h * bpp;
    return n > AVER_FW_RTEX_MAX_BYTES ? -1 : n;
}

Tex* find(int32_t id) {
    Store& s = store();
    const auto it = s.byId.find(id);
    return it == s.byId.end() ? nullptr : &it->second;
}

bool validKey(const char* key) {
    if (!key) return false;
    const size_t n = std::strlen(key);
    return n >= 1 && n <= kMaxKey;
}

int32_t copyOut(const std::string& s, char* out, int32_t cap) {
    if (!out || cap < 1) return static_cast<int32_t>(s.size());
    const size_t n = std::min(s.size(), static_cast<size_t>(cap - 1));
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
    return static_cast<int32_t>(s.size());
}

void touch(Tex& t) {
    t.dirty = true;
    ++t.generation;
}

}  // namespace

extern "C" void aver_fw_rtex_reset(void);

namespace {
// A play session that has ended takes its textures with it: the state fell back to the editor, the same rule the runtime meshes follow (aver_fw_rtmesh_pump).
// Checked where the host reads the registry, so no extra call is needed.
int32_t g_lastPlayState = AVER_FW_PLAY_EDITOR;
void followPlaySession() {
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && g_lastPlayState != AVER_FW_PLAY_EDITOR) aver_fw_rtex_reset();
    g_lastPlayState = state;
}
}  // namespace

extern "C" {

int32_t aver_fw_rtex_abi_version(void) { return AVER_FW_RTEX_ABI_VERSION; }

int32_t aver_fw_rtex_create(const char* key, int32_t width, int32_t height, int32_t format, int32_t mips) {
    if (!validKey(key)) return 0;
    Store& s = store();
    if (const auto it = s.byKey.find(key); it != s.byKey.end()) return it->second;
    if (static_cast<int32_t>(s.byId.size()) >= s.maxTextures) {
        AVER_WARN("[RuntimeTexture] '{}' refused: {} textures are live (the budget)", key, s.byId.size());
        return 0;
    }
    const int64_t bytes = imageBytes(width, height, format);
    if (bytes < 0) return 0;
    if (s.totalBytes + static_cast<size_t>(bytes) > static_cast<size_t>(AVER_FW_RTEX_MAX_TOTAL_BYTES)) {
        AVER_WARN("[RuntimeTexture] '{}' refused: the textures would pass {} MB together", key, AVER_FW_RTEX_MAX_TOTAL_BYTES >> 20);
        return 0;
    }
    const int32_t id = s.next++;
    Tex& t = s.byId[id];
    t.key = key;
    t.width = width; t.height = height; t.format = format;
    t.mips = (mips != 0 && format != AVER_FW_RTEX_FORMAT_RGBA16F) ? 1 : 0;
    t.pixels.assign(static_cast<size_t>(bytes), 0);
    s.byKey[t.key] = id;
    s.totalBytes += static_cast<size_t>(bytes);
    touch(t);
    return id;
}

int32_t aver_fw_rtex_destroy(int32_t id) {
    Store& s = store();
    const auto it = s.byId.find(id);
    if (it == s.byId.end()) return 0;
    s.totalBytes -= std::min(s.totalBytes, it->second.pixels.size());
    s.byKey.erase(it->second.key);
    s.byId.erase(it);
    s.removed.push_back(id);
    return 1;
}

int32_t aver_fw_rtex_find(const char* key) {
    if (!key) return 0;
    const Store& s = store();
    const auto it = s.byKey.find(key);
    return it == s.byKey.end() ? 0 : it->second;
}

int32_t aver_fw_rtex_set_pixels(int32_t id, int32_t width, int32_t height, int32_t format, const uint8_t* data, int32_t bytes) {
    Tex* t = find(id);
    if (!t || !data) return 0;
    const int64_t want = imageBytes(width, height, format);
    if (want < 0 || bytes < 0 || static_cast<int64_t>(bytes) != want) return 0;
    Store& s = store();
    const size_t others = s.totalBytes - std::min(s.totalBytes, t->pixels.size());
    if (others + static_cast<size_t>(want) > static_cast<size_t>(AVER_FW_RTEX_MAX_TOTAL_BYTES)) {
        AVER_WARN("[RuntimeTexture] '{}' not updated: the textures would pass {} MB together", t->key, AVER_FW_RTEX_MAX_TOTAL_BYTES >> 20);
        return 0;
    }
    t->pixels.assign(data, data + want);
    s.totalBytes = others + static_cast<size_t>(want);
    t->width = width; t->height = height;
    if (t->format != format) {
        t->format = format;
        if (format == AVER_FW_RTEX_FORMAT_RGBA16F) t->mips = 0;
    }
    touch(*t);
    return 1;
}

int32_t aver_fw_rtex_set_region(int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* data, int32_t bytes) {
    Tex* t = find(id);
    if (!t || !data || w < 1 || h < 1 || x < 0 || y < 0) return 0;
    if (static_cast<int64_t>(x) + w > t->width || static_cast<int64_t>(y) + h > t->height) return 0;
    const int32_t bpp = bytesPerPixel(t->format);
    const int64_t want = static_cast<int64_t>(w) * h * bpp;
    if (bytes < 0 || static_cast<int64_t>(bytes) != want) return 0;
    const size_t rowBytes = static_cast<size_t>(w) * bpp;
    for (int32_t row = 0; row < h; ++row) {
        std::memcpy(t->pixels.data() + (static_cast<size_t>(y + row) * t->width + x) * bpp, data + static_cast<size_t>(row) * rowBytes, rowBytes);
    }
    touch(*t);
    return 1;
}

int32_t aver_fw_rtex_get_pixels(int32_t id, uint8_t* out, int32_t cap) {
    const Tex* t = find(id);
    if (!t) return 0;
    const int32_t n = static_cast<int32_t>(t->pixels.size());
    if (out && cap >= n) std::memcpy(out, t->pixels.data(), t->pixels.size());
    return n;
}

int32_t aver_fw_rtex_info(int32_t id, int32_t* out6) {
    const Tex* t = find(id);
    if (!t || !out6) return 0;
    out6[0] = t->width; out6[1] = t->height; out6[2] = t->format; out6[3] = t->mips; out6[4] = t->generation;
    out6[5] = static_cast<int32_t>(t->pixels.size());
    return 1;
}

int32_t aver_fw_rtex_key(int32_t id, char* out, int32_t cap) {
    const Tex* t = find(id);
    return t ? copyOut(t->key, out, cap) : 0;
}

int32_t aver_fw_rtex_material_path(int32_t id, char* out, int32_t cap) {
    const Tex* t = find(id);
    return t ? copyOut(std::string(AVER_FW_RTEX_MATERIAL_PREFIX) + t->key, out, cap) : 0;
}

uint64_t aver_fw_rtex_gpu_handle(int32_t id) {
    const Tex* t = find(id);
    return t ? t->gpu : 0;
}

int32_t aver_fw_image_decode(const char* path, int32_t* out2, uint8_t* out, int32_t cap) {
    if (!path || !*path) return 0;
    aver::ImageData img;
    std::string err;
    if (!aver::decodeImage(path, img, &err) || !img.valid()) return 0;
    if (img.width > static_cast<aver::u32>(AVER_FW_RTEX_MAX_DIMENSION) || img.height > static_cast<aver::u32>(AVER_FW_RTEX_MAX_DIMENSION)) return 0;
    if (!out) {
        if (!out2) return 0;
        out2[0] = static_cast<int32_t>(img.width);
        out2[1] = static_cast<int32_t>(img.height);
        return 1;
    }
    const int64_t need = static_cast<int64_t>(img.width) * img.height * 4;
    if (need > AVER_FW_RTEX_MAX_BYTES || cap < 0 || static_cast<int64_t>(cap) < need) return 0;
    std::memcpy(out, img.pixels.data(), static_cast<size_t>(need));
    if (out2) { out2[0] = static_cast<int32_t>(img.width); out2[1] = static_cast<int32_t>(img.height); }
    return 1;
}

void aver_fw_rtex_configure(int32_t maxTextures) {
    if (maxTextures >= 1) store().maxTextures = maxTextures;
}

int32_t aver_fw_rtex_take_dirty(int32_t* outIds, int32_t max) {
    followPlaySession();
    if (!outIds || max < 1) return 0;
    int32_t n = 0;
    for (auto& [id, t] : store().byId) {
        if (!t.dirty) continue;
        if (n >= max) break;
        t.dirty = false;
        outIds[n++] = id;
    }
    return n;
}

int32_t aver_fw_rtex_take_removed(int32_t* outIds, int32_t max) {
    followPlaySession();
    Store& s = store();
    if (!outIds || max < 1 || s.removed.empty()) return 0;
    const int32_t n = std::min(max, static_cast<int32_t>(s.removed.size()));
    for (int32_t i = 0; i < n; ++i) outIds[i] = s.removed[static_cast<size_t>(i)];
    s.removed.erase(s.removed.begin(), s.removed.begin() + n);
    return n;
}

int32_t aver_fw_rtex_view(int32_t id, AverFwRtexView* out) {
    const Tex* t = find(id);
    if (!t || !out) return 0;
    out->width = t->width; out->height = t->height; out->format = t->format; out->mips = t->mips;
    out->bytes = static_cast<int32_t>(t->pixels.size());
    out->pixels = t->pixels.data();
    out->key = t->key.c_str();
    return 1;
}

void aver_fw_rtex_host_set_gpu_handle(int32_t id, uint64_t handle) {
    if (Tex* t = find(id)) t->gpu = handle;
}

int32_t aver_fw_rtex_count(void) { return static_cast<int32_t>(store().byId.size()); }

void aver_fw_rtex_reset(void) {
    Store& s = store();
    for (const auto& [id, t] : s.byId) { (void)t; s.removed.push_back(id); }
    s.byId.clear();
    s.byKey.clear();
    s.totalBytes = 0;
}

}  // extern "C"
