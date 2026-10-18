// The world effects C ABI (framework_worldfx_abi.h) end to end through the Aver.Framework DLL: every environment key
// sets, reads back and clears; bad keys and values are refused; post layers are created, valued, weighted and removed;
// the blend advances with aver_fw_worldfx_tick; authored-volume scales start at 1 and fade; and every bad argument
// answers 0 and writes nothing. The store is process-wide, so each section ends by clearing what it made. Exits 0 or 1.
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/framework/framework_worldfx_abi.h"

#include <cmath>
#include <limits>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static int64_t keyHash(const char* key) { return static_cast<int64_t>(fnv1a64(std::string_view(key))); }

static void clearAll() {
    aver_fw_env_clear(0);
    aver_fw_post_clear(0);
    for (int64_t h = 1; h <= 40; ++h) aver_fw_post_volume_weight(h, -1.0f, 0.0f);
}

static void testVersionAndKeys() {
    AVER_INFO("=== version and key numbers ===");
    check(aver_fw_worldfx_abi_version() == 1 && AVER_FW_WORLDFX_ABI_VERSION == 1, "the world effects ABI is version 1");
    check(AVER_FW_ENV_SUN_DIR == 1 && AVER_FW_ENV_SUN_LUX == 2 && AVER_FW_ENV_SUN_COLOR == 3 && AVER_FW_ENV_SUN_TEMP == 4 &&
              AVER_FW_ENV_FOG_DENSITY == 5 && AVER_FW_ENV_FOG_COLOR == 6 && AVER_FW_ENV_FOG_FALLOFF == 7 &&
              AVER_FW_ENV_FOG_HEIGHT == 8 && AVER_FW_ENV_FOG_START == 9 && AVER_FW_ENV_FOG_MAXOPAC == 10 && AVER_FW_ENV_SKY_LIGHT == 11,
          "the environment keys are pinned");
}

static void testEnvironment() {
    AVER_INFO("=== environment ===");
    float out[3] = {-9, -9, -9};

    // Nothing is overridden at first, and a get leaves its output alone.
    for (int k = AVER_FW_ENV_SUN_DIR; k <= AVER_FW_ENV_SKY_LIGHT; ++k)
        check(aver_fw_env_get(k, out) == 0 && out[0] == -9 && out[1] == -9 && out[2] == -9, "key " + std::to_string(k) + " starts unset");

    // Every key stores and reads back (values already inside every clamp).
    struct Case { int key; float v[3]; float want[3]; };
    const Case cases[] = {
        {AVER_FW_ENV_SUN_DIR,     {0, 0, 2},         {0, 0, 1}},           // normalised
        {AVER_FW_ENV_SUN_LUX,     {50000, 7, 7},     {50000, 0, 0}},       // unused components stored as 0
        {AVER_FW_ENV_SUN_COLOR,   {1.0f, 0.5f, 0.25f}, {1.0f, 0.5f, 0.25f}},
        {AVER_FW_ENV_SUN_TEMP,    {5600, 0, 0},      {5600, 0, 0}},
        {AVER_FW_ENV_FOG_DENSITY, {0.0001f, 0, 0},   {0.0001f, 0, 0}},
        {AVER_FW_ENV_FOG_COLOR,   {0.2f, 0.3f, 0.4f}, {0.2f, 0.3f, 0.4f}},
        {AVER_FW_ENV_FOG_FALLOFF, {0.002f, 0, 0},    {0.002f, 0, 0}},
        {AVER_FW_ENV_FOG_HEIGHT,  {-250, 0, 0},      {-250, 0, 0}},         // height may be negative
        {AVER_FW_ENV_FOG_START,   {300, 0, 0},       {300, 0, 0}},
        {AVER_FW_ENV_FOG_MAXOPAC, {0.7f, 0, 0},      {0.7f, 0, 0}},
        {AVER_FW_ENV_SKY_LIGHT,   {2.5f, 0, 0},      {2.5f, 0, 0}},
    };
    for (const Case& c : cases) {
        const std::string n = "key " + std::to_string(c.key);
        check(aver_fw_env_set(c.key, c.v[0], c.v[1], c.v[2]) == 1, n + " sets");
        out[0] = out[1] = out[2] = -9;
        const bool got = aver_fw_env_get(c.key, out) == 1;
        check(got && approx(out[0], c.want[0]) && approx(out[1], c.want[1]) && approx(out[2], c.want[2]), n + " reads back");
        check(aver_fw_env_get(c.key, nullptr) == 1, n + " answers 1 with no output pointer");
    }

    // Clearing one key leaves the others.
    check(aver_fw_env_clear(AVER_FW_ENV_SUN_LUX) == 1 && aver_fw_env_get(AVER_FW_ENV_SUN_LUX, out) == 0, "clearing one key releases it");
    check(aver_fw_env_get(AVER_FW_ENV_SUN_DIR, out) == 1, "and leaves the others");
    check(aver_fw_env_clear(AVER_FW_ENV_SUN_LUX) == 1, "clearing a key that is already clear still answers 1");
    check(aver_fw_env_clear(0) == 1, "key 0 clears all");
    bool any = false;
    for (int k = AVER_FW_ENV_SUN_DIR; k <= AVER_FW_ENV_SKY_LIGHT; ++k) any = any || aver_fw_env_get(k, nullptr) == 1;
    check(!any, "nothing stays overridden after clear-all");

    // Sanitising.
    aver_fw_env_set(AVER_FW_ENV_SUN_DIR, 3, 0, 4);
    aver_fw_env_get(AVER_FW_ENV_SUN_DIR, out);
    check(approx(out[0], 0.6f) && approx(out[1], 0.0f) && approx(out[2], 0.8f), "a sun direction is stored normalised");
    check(aver_fw_env_set(AVER_FW_ENV_SUN_DIR, 0, 0, 0) == 0, "a zero sun direction is refused");
    aver_fw_env_get(AVER_FW_ENV_SUN_DIR, out);
    check(approx(out[0], 0.6f) && approx(out[2], 0.8f), "and the previous direction stands");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    check(aver_fw_env_set(AVER_FW_ENV_FOG_DENSITY, nan, 0, 0) == 0 && aver_fw_env_get(AVER_FW_ENV_FOG_DENSITY, nullptr) == 0, "NaN is refused");
    check(aver_fw_env_set(AVER_FW_ENV_FOG_DENSITY, 1, inf, 0) == 0, "an infinite component is refused even when the key ignores it");
    aver_fw_env_set(AVER_FW_ENV_SUN_LUX, -5, 0, 0);
    aver_fw_env_get(AVER_FW_ENV_SUN_LUX, out);
    check(out[0] == 0.0f, "negative lux clamps to 0");
    aver_fw_env_set(AVER_FW_ENV_FOG_MAXOPAC, 2, 0, 0);
    aver_fw_env_get(AVER_FW_ENV_FOG_MAXOPAC, out);
    check(out[0] == 1.0f, "max opacity clamps to 1");
    aver_fw_env_set(AVER_FW_ENV_SUN_TEMP, 90000, 0, 0);
    aver_fw_env_get(AVER_FW_ENV_SUN_TEMP, out);
    check(out[0] == 40000.0f, "temperature clamps to 40000 K");
    aver_fw_env_set(AVER_FW_ENV_SUN_COLOR, -1, 0.5f, 2);
    aver_fw_env_get(AVER_FW_ENV_SUN_COLOR, out);
    check(out[0] == 0.0f && approx(out[1], 0.5f) && out[2] == 2.0f, "a colour clamps below at 0 and not above");

    // Unknown keys.
    out[0] = out[1] = out[2] = -9;
    check(aver_fw_env_set(0, 1, 1, 1) == 0 && aver_fw_env_set(12, 1, 1, 1) == 0 && aver_fw_env_set(-1, 1, 1, 1) == 0, "unknown keys are refused by set");
    check(aver_fw_env_clear(12) == 0 && aver_fw_env_clear(-1) == 0, "and by clear");
    check(aver_fw_env_get(0, out) == 0 && aver_fw_env_get(12, out) == 0 && out[0] == -9, "and by get, which leaves its output alone");
    clearAll();
}

static void testPostLayers() {
    AVER_INFO("=== post layers ===");
    const int64_t kSat = keyHash("saturation"), kLift = keyHash("lift"), kBad = keyHash("notakey");
    const int64_t L1 = 101, L2 = 202, L3 = 303;
    int64_t id = 0;
    float w = -1, v[3] = {-9, -9, -9};
    int32_t n = -1;
    int64_t kh = 0;

    check(aver_fw_post_layer_count() == 0, "no layers at first");
    check(aver_fw_post_set(0, kSat, 0, 0, 0) == 0, "layer 0 is refused");
    check(aver_fw_post_set(L1, kBad, 0, 0, 0) == 0 && aver_fw_post_layer_count() == 0, "an unknown key is refused and makes no layer");
    check(aver_fw_post_set(L1, 0, 0, 0, 0) == 0, "key hash 0 is refused");
    check(aver_fw_post_set(L1, kSat, std::numeric_limits<float>::quiet_NaN(), 0, 0) == 0 && aver_fw_post_layer_count() == 0, "a NaN value is refused");

    // A layer made by a value alone engages by itself over AVER_FW_POST_AUTO_BLEND.
    check(aver_fw_post_set(L1, kSat, 0.25f, 0, 0) == 1 && aver_fw_post_layer_count() == 1, "a value makes the layer");
    check(aver_fw_post_layer_info(0, &id, &w, &n) == 1 && id == L1 && w == 0.0f && n == 1, "info: the id, weight 0 before any tick, one value");
    aver_fw_worldfx_tick(AVER_FW_POST_AUTO_BLEND * 0.5f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(approx(w, 0.5f), "half the auto blend later it is at half weight");
    aver_fw_worldfx_tick(1.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 1.0f, "and it settles at 1");
    check(aver_fw_post_layer_value(0, 0, &kh, v) == 1 && kh == kSat && approx(v[0], 0.25f), "the value reads back under its key hash");

    // Setting the same key replaces; another key appends in order.
    check(aver_fw_post_set(L1, kSat, 0.75f, 0, 0) == 1, "the same key sets again");
    aver_fw_post_layer_info(0, nullptr, nullptr, &n);
    aver_fw_post_layer_value(0, 0, nullptr, v);
    check(n == 1 && approx(v[0], 0.75f), "it replaced the value and added none");
    check(aver_fw_post_set(L1, kLift, 0.1f, 0.2f, 0.3f) == 1, "a second key appends");
    aver_fw_post_layer_info(0, nullptr, nullptr, &n);
    check(n == 2 && aver_fw_post_layer_value(0, 1, &kh, v) == 1 && kh == kLift && approx(v[0], 0.1f) && approx(v[1], 0.2f) && approx(v[2], 0.3f), "three components survive");
    check(aver_fw_post_layer_value(0, 2, &kh, v) == 0 && aver_fw_post_layer_value(0, -1, &kh, v) == 0 && aver_fw_post_layer_value(1, 0, &kh, v) == 0, "an index out of range answers 0");
    check(aver_fw_post_layer_info(1, &id, &w, &n) == 0 && aver_fw_post_layer_info(-1, &id, &w, &n) == 0, "so does a layer index out of range");

    // An explicit weight blends from where the layer is now.
    check(aver_fw_post_set_weight(L1, 0.0f, 2.0f) == 1, "a weight with a blend is accepted");
    aver_fw_worldfx_tick(1.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(approx(w, 0.5f), "halfway through a 2 s fade to 0 the weight is 0.5");
    aver_fw_worldfx_tick(1.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 0.0f, "the fade ends exactly at 0");
    aver_fw_worldfx_tick(5.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 0.0f, "and stays there");
    check(aver_fw_post_set_weight(L1, 1.0f, 0.0f) == 1, "a zero blend is accepted");
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 1.0f, "a zero blend applies at once, with no tick");
    aver_fw_post_set_weight(L1, 5.0f, 0.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 1.0f, "a weight above 1 clamps");
    aver_fw_post_set_weight(L1, -3.0f, 0.0f);
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 0.0f, "and one below 0 clamps");
    check(aver_fw_post_set_weight(L1, std::numeric_limits<float>::quiet_NaN(), 0.0f) == 0 &&
              aver_fw_post_set_weight(L1, 1.0f, std::numeric_limits<float>::infinity()) == 0 && aver_fw_post_set_weight(0, 1.0f, 0.0f) == 0,
          "a NaN weight, an infinite blend and layer 0 are refused");

    // A weight set first makes the layer explicit: a value added later does not auto-engage it.
    check(aver_fw_post_set_weight(L2, 0.0f, 0.0f) == 1 && aver_fw_post_layer_count() == 2, "a weight alone makes a layer");
    aver_fw_post_set(L2, kSat, 0.0f, 0, 0);
    aver_fw_worldfx_tick(3.0f);
    aver_fw_post_layer_info(1, &id, &w, &n);
    check(id == L2 && w == 0.0f && n == 1, "a layer given a weight of 0 stays at 0 when a value arrives");

    // Removal keeps the order of the rest.
    aver_fw_post_set(L3, kSat, 1.0f, 0, 0);
    check(aver_fw_post_layer_count() == 3, "three layers");
    check(aver_fw_post_clear(L2) == 1 && aver_fw_post_layer_count() == 2, "clearing a layer removes it");
    aver_fw_post_layer_info(0, &id, nullptr, nullptr);
    int64_t id1 = 0;
    aver_fw_post_layer_info(1, &id1, nullptr, nullptr);
    check(id == L1 && id1 == L3, "the others keep their order");
    check(aver_fw_post_clear(L2) == 0, "clearing it again answers 0");
    check(aver_fw_post_clear(0) == 1 && aver_fw_post_layer_count() == 0, "layer 0 clears every layer");

    // The bound.
    bool allOk = true;
    for (int i = 1; i <= AVER_FW_POST_MAX_LAYERS; ++i) allOk = allOk && aver_fw_post_set(1000 + i, kSat, 0, 0, 0) == 1;
    check(allOk && aver_fw_post_layer_count() == AVER_FW_POST_MAX_LAYERS, "the layer table fills to its bound");
    check(aver_fw_post_set(5000, kSat, 0, 0, 0) == 0 && aver_fw_post_set_weight(5001, 1, 0) == 0 && aver_fw_post_layer_count() == AVER_FW_POST_MAX_LAYERS,
          "one more is refused");
    check(aver_fw_post_set(1001, kLift, 0, 0, 0) == 1, "an existing layer still takes values when the table is full");
    aver_fw_post_clear(0);

    // A bad dt moves nothing.
    aver_fw_post_set(L1, kSat, 0.5f, 0, 0);
    aver_fw_worldfx_tick(0.0f);
    aver_fw_worldfx_tick(-1.0f);
    aver_fw_worldfx_tick(std::numeric_limits<float>::quiet_NaN());
    aver_fw_post_layer_info(0, nullptr, &w, nullptr);
    check(w == 0.0f, "a zero, negative or NaN dt leaves the blend alone");
    clearAll();
}

static void testVolumeScales() {
    AVER_INFO("=== authored volume scales ===");
    const int64_t V1 = 11, V2 = 22;
    float w = -1;
    check(aver_fw_post_volume_scale(V1, &w) == 0 && w == -1.0f, "no scale at first, and the output is untouched");
    check(aver_fw_post_volume_weight(0, 0.5f, 0.0f) == 0, "hash 0 is refused");
    check(aver_fw_post_volume_weight(V1, std::numeric_limits<float>::quiet_NaN(), 0.0f) == 0, "a NaN weight is refused");

    check(aver_fw_post_volume_weight(V1, 0.25f, 0.0f) == 1 && aver_fw_post_volume_scale(V1, &w) == 1 && approx(w, 0.25f), "a zero blend scales at once");
    check(aver_fw_post_volume_scale(V1, nullptr) == 1, "a read with no output pointer answers 1");

    // A new scale fades from 1 (the volume's own full weight).
    check(aver_fw_post_volume_weight(V2, 0.0f, 2.0f) == 1, "a fade is accepted");
    aver_fw_post_volume_scale(V2, &w);
    check(w == 1.0f, "the scale starts at 1");
    aver_fw_worldfx_tick(1.0f);
    aver_fw_post_volume_scale(V2, &w);
    check(approx(w, 0.5f), "and is at half after half the blend");
    aver_fw_worldfx_tick(1.0f);
    aver_fw_post_volume_scale(V2, &w);
    check(w == 0.0f, "and 0 at the end");

    aver_fw_post_volume_weight(V1, 3.0f, 0.0f);
    aver_fw_post_volume_scale(V1, &w);
    check(w == 1.0f, "a scale above 1 clamps to 1");

    check(aver_fw_post_volume_weight(V1, -1.0f, 0.0f) == 1 && aver_fw_post_volume_scale(V1, &w) == 0, "a negative weight removes the scale");
    check(aver_fw_post_volume_weight(V1, -1.0f, 0.0f) == 0, "removing it again answers 0");
    check(aver_fw_post_volume_scale(V2, nullptr) == 1, "the other scale is untouched");

    // The bound.
    clearAll();
    bool allOk = true;
    for (int i = 1; i <= AVER_FW_POST_MAX_SCALES; ++i) allOk = allOk && aver_fw_post_volume_weight(i, 0.5f, 0.0f) == 1;
    check(allOk, "the scale table fills to its bound");
    check(aver_fw_post_volume_weight(AVER_FW_POST_MAX_SCALES + 1, 0.5f, 0.0f) == 0, "one more is refused");
    check(aver_fw_post_volume_weight(1, 0.75f, 0.0f) == 1, "an existing scale still changes when the table is full");
    clearAll();
}

static void testKeyTableAgreement() {
    AVER_INFO("=== every POSTVOLUME key is accepted ===");
    bool all = true;
    for (const fmt::OcPostKeyInfo& k : fmt::kOcPostKeys) all = all && aver_fw_post_set(77, keyHash(k.key), 0, 0, 0) == 1;
    check(all, "the hash of every key in kOcPostKeys is accepted");
    int32_t n = 0;
    aver_fw_post_layer_info(0, nullptr, nullptr, &n);
    check(n == static_cast<int32_t>(sizeof(fmt::kOcPostKeys) / sizeof(fmt::kOcPostKeys[0])), "and each made its own value");
    check(aver_fw_post_set(77, keyHash("Saturation"), 0, 0, 0) == 0, "a key must be lower-case: the hash of the capitalised name is refused");
    clearAll();
}

int main() {
    AVER_INFO("FrameworkWorldFxAbiTest");
    testVersionAndKeys();
    testEnvironment();
    testPostLayers();
    testVolumeScales();
    testKeyTableAgreement();
    if (g_failures) {
        AVER_ERROR("FrameworkWorldFxAbiTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FrameworkWorldFxAbiTest: ok");
    return 0;
}
