// FrameworkRtexAbiTest -- E8: the runtime-texture C ABI (framework_rtex_abi.h) through the Aver.Framework DLL: creation by key and the budgets, whole-image and
// region writes, what reads back, the dirty and removed queues the host drains, the GPU handle slot, the mip request, the image-file decoder, and the refusals
// (hostile sizes and byte counts included). The store is process-wide, so each section starts with aver_fw_rtex_reset. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_rtex_abi.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void resetAll() {
    aver_fw_rtex_reset();
    aver_fw_rtex_configure(AVER_FW_RTEX_DEFAULT_MAX_TEXTURES);
    int32_t ids[64];
    while (aver_fw_rtex_take_removed(ids, 64) > 0) {}
    while (aver_fw_rtex_take_dirty(ids, 64) > 0) {}
}

static std::vector<uint8_t> ramp(int32_t n) {
    std::vector<uint8_t> v(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) v[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

static void testCreateAndRefusals() {
    AVER_INFO("=== create and refusals ===");
    resetAll();
    check(aver_fw_rtex_abi_version() == 1 && AVER_FW_RTEX_ABI_VERSION == 1, "the runtime-texture ABI is version 1");
    check(aver_fw_rtex_create(nullptr, 4, 4, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0 && aver_fw_rtex_create("", 4, 4, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0, "a null or empty key is refused");
    const std::string longKey(121, 'k');
    check(aver_fw_rtex_create(longKey.c_str(), 4, 4, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0, "a key over 120 bytes is refused");
    check(aver_fw_rtex_create("a", 0, 4, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0 && aver_fw_rtex_create("a", 4, -1, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0, "a side below 1 is refused");
    check(aver_fw_rtex_create("a", AVER_FW_RTEX_MAX_DIMENSION + 1, 1, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0 && aver_fw_rtex_create("a", 2147483647, 2147483647, AVER_FW_RTEX_FORMAT_RGBA8, 0) == 0,
          "a side past 8192, however large, is refused");
    check(aver_fw_rtex_create("a", 4, 4, 99, 0) == 0 && aver_fw_rtex_create("a", 4, 4, -1, 0) == 0, "an unknown format is refused");
    check(aver_fw_rtex_create("a", AVER_FW_RTEX_MAX_DIMENSION, AVER_FW_RTEX_MAX_DIMENSION, AVER_FW_RTEX_FORMAT_RGBA16F, 0) == 0, "8192 x 8192 half floats (512 MB) is past the per-texture cap");
    check(aver_fw_rtex_count() == 0, "nothing was made by any of that");

    const int32_t a = aver_fw_rtex_create("Main/Paint", 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, 1);
    check(a > 0 && aver_fw_rtex_count() == 1, "a texture is made");
    check(aver_fw_rtex_create("Main/Paint", 64, 64, AVER_FW_RTEX_FORMAT_R8, 0) == a, "the same key returns the same texture, size and format kept");
    check(aver_fw_rtex_find("Main/Paint") == a && aver_fw_rtex_find("Nope") == 0 && aver_fw_rtex_find(nullptr) == 0, "find answers by key");
    int32_t info[6] = {};
    check(aver_fw_rtex_info(a, info) == 1 && info[0] == 4 && info[1] == 2 && info[2] == AVER_FW_RTEX_FORMAT_RGBA8 && info[3] == 1 && info[5] == 32, "info: 4x2 RGBA8, mips asked, 32 bytes");
    check(aver_fw_rtex_info(999, info) == 0 && aver_fw_rtex_info(a, nullptr) == 0, "info on an unknown id or with no output is refused");
    const int32_t half = aver_fw_rtex_create("Main/Half", 2, 2, AVER_FW_RTEX_FORMAT_RGBA16F, 1);
    check(half > 0 && aver_fw_rtex_info(half, info) == 1 && info[3] == 0 && info[5] == 32, "half floats take no mip chain (mips reads back 0); 2x2 is 32 bytes");
    char key[64] = {};
    check(aver_fw_rtex_key(a, key, sizeof key) == 10 && std::string(key) == "Main/Paint" && aver_fw_rtex_key(999, key, sizeof key) == 0, "the key reads back");
    char path[96] = {};
    check(aver_fw_rtex_material_path(a, path, sizeof path) == 19 && std::string(path) == "@rt:rtex.Main/Paint" && std::string(path).rfind(AVER_FW_RTEX_MATERIAL_PREFIX, 0) == 0,
          "the material slot path is '@rt:rtex.<key>'");
    char tiny[4] = {};
    check(aver_fw_rtex_key(a, tiny, sizeof tiny) == 10 && std::string(tiny) == "Mai", "a short output buffer is truncated and terminated, the full length returned");
    resetAll();
}

static void testPixels() {
    AVER_INFO("=== whole-image and region writes, reads ===");
    resetAll();
    const int32_t id = aver_fw_rtex_create("px", 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, 0);
    std::vector<uint8_t> out(32, 0xEE);
    check(aver_fw_rtex_get_pixels(id, out.data(), 32) == 32 && out[0] == 0 && out[31] == 0, "a new texture reads back zero (transparent black)");
    const std::vector<uint8_t> img = ramp(32);
    check(aver_fw_rtex_set_pixels(id, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), 32) == 1, "set_pixels takes exactly width x height x 4 bytes");
    check(aver_fw_rtex_get_pixels(id, out.data(), 32) == 32 && out == img, "and reads back byte for byte");
    std::vector<uint8_t> small(8, 0xEE);
    check(aver_fw_rtex_get_pixels(id, small.data(), 8) == 32 && small[0] == 0xEE, "a buffer that is too small gets the size and no bytes");
    check(aver_fw_rtex_get_pixels(id, nullptr, 0) == 32, "a null buffer asks the size");
    check(aver_fw_rtex_get_pixels(999, out.data(), 32) == 0, "an unknown id reads 0 bytes");

    check(aver_fw_rtex_set_pixels(id, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), 31) == 0 && aver_fw_rtex_set_pixels(id, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), 33) == 0 &&
          aver_fw_rtex_set_pixels(id, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), -32) == 0, "a wrong or negative byte count is refused");
    check(aver_fw_rtex_set_pixels(id, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, nullptr, 32) == 0 && aver_fw_rtex_set_pixels(999, 4, 2, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), 32) == 0,
          "a null pointer or an unknown id is refused");
    check(aver_fw_rtex_set_pixels(id, 65536, 65536, AVER_FW_RTEX_FORMAT_RGBA8, img.data(), 0) == 0 && aver_fw_rtex_set_pixels(id, 2147483647, 2, AVER_FW_RTEX_FORMAT_R8, img.data(), 32) == 0,
          "sizes whose product would overflow 32 bits are refused");
    check(aver_fw_rtex_get_pixels(id, out.data(), 32) == 32 && out == img, "none of the refusals touched the pixels");

    // Region: a 2x1 block at (1,1) of the 4x2 image.
    const uint8_t block[8] = {200, 201, 202, 203, 210, 211, 212, 213};
    check(aver_fw_rtex_set_region(id, 1, 1, 2, 1, block, 8) == 1, "a region inside the image is written");
    aver_fw_rtex_get_pixels(id, out.data(), 32);
    check(out[16 + 4] == 200 && out[16 + 11] == 213 && out[16] == img[16] && out[16 + 12] == img[16 + 12] && out[0] == img[0], "...only that rectangle changed");
    check(aver_fw_rtex_set_region(id, 3, 1, 2, 1, block, 8) == 0 && aver_fw_rtex_set_region(id, 0, 2, 1, 1, block, 4) == 0 && aver_fw_rtex_set_region(id, -1, 0, 1, 1, block, 4) == 0 &&
          aver_fw_rtex_set_region(id, 2147483647, 0, 2147483647, 1, block, 8) == 0, "a region that leaves the image is refused, however its sum overflows");
    check(aver_fw_rtex_set_region(id, 0, 0, 2, 1, block, 4) == 0 && aver_fw_rtex_set_region(id, 0, 0, 0, 1, block, 0) == 0 && aver_fw_rtex_set_region(id, 0, 0, 2, 1, nullptr, 8) == 0,
          "a wrong byte count, an empty rectangle or a null pointer is refused");

    // A write can change the size and the format.
    const std::vector<uint8_t> grey = ramp(6);
    check(aver_fw_rtex_set_pixels(id, 3, 2, AVER_FW_RTEX_FORMAT_R8, grey.data(), 6) == 1, "set_pixels may change size and format");
    int32_t info[6] = {};
    check(aver_fw_rtex_info(id, info) == 1 && info[0] == 3 && info[1] == 2 && info[2] == AVER_FW_RTEX_FORMAT_R8 && info[5] == 6, "info follows");
    std::vector<uint8_t> g(6);
    check(aver_fw_rtex_get_pixels(id, g.data(), 6) == 6 && g == grey, "the R8 image reads back");
    check(aver_fw_rtex_set_region(id, 0, 0, 1, 1, grey.data(), 4) == 0 && aver_fw_rtex_set_region(id, 2, 1, 1, 1, grey.data(), 1) == 1, "a region counts bytes in the texture's own format");
    resetAll();
}

static void testQueues() {
    AVER_INFO("=== dirty and removed queues, GPU handle, view ===");
    resetAll();
    const int32_t a = aver_fw_rtex_create("qa", 2, 2, AVER_FW_RTEX_FORMAT_RGBA8, 0);
    const int32_t b = aver_fw_rtex_create("qb", 2, 2, AVER_FW_RTEX_FORMAT_R8, 1);
    int32_t ids[8] = {};
    check(aver_fw_rtex_take_dirty(ids, 8) == 2 && ((ids[0] == a && ids[1] == b) || (ids[0] == b && ids[1] == a)), "both new textures are dirty");
    check(aver_fw_rtex_take_dirty(ids, 8) == 0, "taking them clears the list");
    const std::vector<uint8_t> px = ramp(16);
    aver_fw_rtex_set_pixels(a, 2, 2, AVER_FW_RTEX_FORMAT_RGBA8, px.data(), 16);
    check(aver_fw_rtex_take_dirty(ids, 8) == 1 && ids[0] == a, "a write dirties exactly that texture");
    aver_fw_rtex_set_region(b, 0, 0, 1, 1, px.data(), 1);
    check(aver_fw_rtex_take_dirty(ids, 1) == 1 && ids[0] == b && aver_fw_rtex_take_dirty(ids, 8) == 0, "a region write does too; a cap of 1 hands over one");
    check(aver_fw_rtex_take_dirty(nullptr, 8) == 0 && aver_fw_rtex_take_dirty(ids, 0) == 0, "no output or a cap of 0 takes nothing");

    int32_t before[6] = {}, after[6] = {};
    aver_fw_rtex_info(a, before);
    aver_fw_rtex_set_pixels(a, 2, 2, AVER_FW_RTEX_FORMAT_RGBA8, px.data(), 16);
    aver_fw_rtex_info(a, after);
    check(after[4] == before[4] + 1, "the generation counts each change");
    while (aver_fw_rtex_take_dirty(ids, 8) > 0) {}

    AverFwRtexView v{};
    check(aver_fw_rtex_view(a, &v) == 1 && v.width == 2 && v.height == 2 && v.format == AVER_FW_RTEX_FORMAT_RGBA8 && v.mips == 0 && v.bytes == 16 && v.pixels != nullptr &&
          v.pixels[0] == px[0] && v.pixels[15] == px[15] && std::string(v.key) == "qa", "the host's view shows size, format, pixels and key");
    AverFwRtexView vb{};
    check(aver_fw_rtex_view(b, &vb) == 1 && vb.mips == 1 && aver_fw_rtex_view(999, &v) == 0 && aver_fw_rtex_view(a, nullptr) == 0, "mips are reported; an unknown id or no output is refused");

    check(aver_fw_rtex_gpu_handle(a) == 0, "no GPU handle before the host uploads");
    aver_fw_rtex_host_set_gpu_handle(a, 0x1234567890ABCDEFull);
    check(aver_fw_rtex_gpu_handle(a) == 0x1234567890ABCDEFull && aver_fw_rtex_gpu_handle(b) == 0 && aver_fw_rtex_gpu_handle(999) == 0, "the host publishes the handle; it reads back for that texture only");
    aver_fw_rtex_host_set_gpu_handle(999, 5);   // unknown id: nothing happens

    check(aver_fw_rtex_destroy(a) == 1 && aver_fw_rtex_destroy(a) == 0 && aver_fw_rtex_count() == 1, "destroy removes once");
    check(aver_fw_rtex_find("qa") == 0 && aver_fw_rtex_info(a, after) == 0 && aver_fw_rtex_gpu_handle(a) == 0, "and the key and id are gone");
    check(aver_fw_rtex_take_removed(ids, 8) == 1 && ids[0] == a && aver_fw_rtex_take_removed(ids, 8) == 0, "the host is told once which one to retire");
    const int32_t again = aver_fw_rtex_create("qa", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0);
    check(again > 0 && again != a, "a key can be made again; it gets a new id");

    aver_fw_rtex_reset();
    check(aver_fw_rtex_count() == 0 && aver_fw_rtex_take_removed(ids, 8) == 2, "reset forgets everything and queues every removal");
    resetAll();
}

static void testBudgets() {
    AVER_INFO("=== texture count budget ===");
    resetAll();
    aver_fw_rtex_configure(2);
    check(aver_fw_rtex_create("b1", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0) > 0 && aver_fw_rtex_create("b2", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0) > 0 && aver_fw_rtex_create("b3", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0) == 0,
          "a third texture is refused at a budget of 2");
    check(aver_fw_rtex_create("b1", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0) > 0, "asking for an existing key still answers when full");
    aver_fw_rtex_configure(0);
    check(aver_fw_rtex_create("b3", 1, 1, AVER_FW_RTEX_FORMAT_R8, 0) == 0, "a budget below 1 keeps the previous one");
    resetAll();
}

static void testDecode() {
    AVER_INFO("=== image file decode ===");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aver_rtex_abi_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // A 2x1 uncompressed 24-bit TGA (bottom-left origin): pixels (255,0,0) and (0,0,255), stored BGR.
    const uint8_t tga[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 24, 0,
                           0, 0, 255,      // red
                           255, 0, 0};     // blue
    const fs::path file = dir / "two.tga";
    {
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(tga), sizeof tga);
    }
    const std::string path = file.string();
    int32_t size[2] = {};
    check(aver_fw_image_decode(path.c_str(), size, nullptr, 0) == 1 && size[0] == 2 && size[1] == 1, "the first call learns the size");
    std::vector<uint8_t> px(8, 0xEE);
    check(aver_fw_image_decode(path.c_str(), size, px.data(), 8) == 1, "the second reads the pixels");
    check(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255 && px[4] == 0 && px[5] == 0 && px[6] == 255 && px[7] == 255, "as RGBA8: red, then blue, both opaque");
    check(aver_fw_image_decode(path.c_str(), size, px.data(), 7) == 0, "a buffer one byte short is refused");
    check(aver_fw_image_decode(path.c_str(), size, px.data(), -1) == 0, "a negative capacity is refused");
    check(aver_fw_image_decode(path.c_str(), nullptr, nullptr, 0) == 0, "asking the size with nowhere to put it is refused");
    check(aver_fw_image_decode((dir / "missing.png").string().c_str(), size, nullptr, 0) == 0 && aver_fw_image_decode(nullptr, size, nullptr, 0) == 0 && aver_fw_image_decode("", size, nullptr, 0) == 0,
          "a missing file, a null path and an empty path are refused");
    {
        const fs::path junk = dir / "junk.png";
        std::ofstream f(junk, std::ios::binary);
        const char noise[] = "this is not an image at all, not even close";
        f.write(noise, sizeof noise);
        f.close();
        check(aver_fw_image_decode(junk.string().c_str(), size, nullptr, 0) == 0, "a file that is not an image is refused");
        // A truncated TGA: the header promises 2x1 and the pixels stop short.
        const fs::path cut = dir / "cut.tga";
        std::ofstream g(cut, std::ios::binary);
        g.write(reinterpret_cast<const char*>(tga), 20);
        g.close();
        check(aver_fw_image_decode(cut.string().c_str(), size, nullptr, 0) == 0 || aver_fw_image_decode(cut.string().c_str(), size, px.data(), 8) == 0,
              "a truncated file does not hand back a full image");
    }
    fs::remove_all(dir, ec);
}

int main() {
    testCreateAndRefusals();
    testPixels();
    testQueues();
    testBudgets();
    testDecode();
    aver_fw_rtex_reset();
    AVER_INFO("FrameworkRtexAbiTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
