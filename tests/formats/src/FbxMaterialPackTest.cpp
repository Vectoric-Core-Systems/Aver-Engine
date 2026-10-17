// packSeparateMetalRough (ImportCook.cpp): the pack step that gives FBX's separate roughness and metalness maps the
// one packed slot .ocmat has. No GPU: it decodes PNGs the test writes, checks the packed texels, and checks that
// materials which do not need it come out untouched. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/ImportedMaterial.hpp"
#include "aver/platform/Image.hpp"

// ImportCook.cpp only DECLARES stb_image_write; this executable carries the one implementation (see the
// Aver.Formats.Material comment in modules/formats/CMakeLists.txt).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <functional>
#include <string>
#include <vector>

using namespace aver;

namespace aver::fmt {
// Defined in ImportCook.cpp (Aver.Formats.Material); declared in ImportCook.hpp once the shared patch lands.
u32 packSeparateMetalRough(std::vector<ImportedImage>& images, std::vector<ImportedMaterial>& materials);
}

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

using Pixel = std::function<void(u32 x, u32 y, u8 rgba[4])>;

// An RGBA PNG whose texels come from `fn`.
static fmt::ImportedImage makePng(u32 w, u32 h, const std::string& name, const Pixel& fn) {
    std::vector<u8> px(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) fn(x, y, &px[(usize(y) * w + x) * 4]);
    fmt::ImportedImage img;
    const auto sink = [](void* ctx, void* data, int len) {
        auto* v = static_cast<std::vector<u8>*>(ctx);
        const u8* p = static_cast<const u8*>(data);
        v->insert(v->end(), p, p + len);
    };
    stbi_write_png_to_func(sink, &img.bytes, int(w), int(h), 4, px.data(), int(w * 4));
    img.ext = ".png";
    img.suggestedName = name;
    img.ok = !img.bytes.empty();
    return img;
}

static bool decode(const fmt::ImportedImage& img, ImageData& out) {
    return decodeImage(img.bytes.data(), img.bytes.size(), out, nullptr);
}

static fmt::ImportedTexture tex(i32 index, char channel = 0) {
    fmt::ImportedTexture t;
    t.imageIndex = index;
    t.channel = channel;
    return t;
}

int main() {
    AVER_INFO("FbxMaterialPackTest");

    AVER_INFO("=== a roughness map and a smaller metalness map ===");
    {
        std::vector<fmt::ImportedImage> images;
        images.push_back(makePng(4, 4, "rough", [](u32, u32, u8 p[4]) { p[0] = p[1] = p[2] = 100; p[3] = 255; }));
        images.push_back(makePng(2, 2, "metal", [](u32 x, u32 y, u8 p[4]) { p[0] = u8(10 + 10 * (y * 2 + x)); p[1] = p[2] = 0; p[3] = 255; }));
        std::vector<fmt::ImportedMaterial> mats(1);
        mats[0].name = "Body";
        mats[0].metalRoughTex = tex(0, 'r');
        mats[0].metallicTex = tex(1, 'r');
        mats[0].roughnessFactor = 1.0f;
        mats[0].metallicFactor = 1.0f;
        check(fmt::packSeparateMetalRough(images, mats) == 1, "one material was packed");
        check(images.size() == 3 && images[2].ok && images[2].ext == ".png", "a new PNG image was appended");
        check(images[2].suggestedName == "Body_mr", "named after the material");
        check(mats[0].metallicTex.empty(), "metallicTex is cleared");
        check(mats[0].metalRoughTex.imageIndex == 2 && mats[0].metalRoughTex.channel == 0, "metalRoughTex points at the packed image");
        check(mats[0].roughnessFactor == 1.0f && mats[0].metallicFactor == 1.0f, "both factors are 1");
        ImageData out;
        check(decode(images[2], out) && out.width == 4 && out.height == 4, "it is the larger size");
        bool texels = out.valid();
        for (u32 y = 0; y < 4 && texels; ++y)
            for (u32 x = 0; x < 4; ++x) {
                const u8* p = &out.pixels[(usize(y) * 4 + x) * 4];
                const u8 wantMetal = u8(10 + 10 * ((y / 2) * 2 + (x / 2)));      // nearest-texel upsample of the 2x2
                if (p[0] != 255 || p[1] != 100 || p[2] != wantMetal || p[3] != 255) { texels = false; break; }
            }
        check(texels, "R = 255, G = roughness, B = the metalness map resampled by nearest texel, A = 255");
    }

    AVER_INFO("=== channels ===");
    {
        std::vector<fmt::ImportedImage> images;
        images.push_back(makePng(2, 2, "rough", [](u32, u32, u8 p[4]) { p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 4; }));
        images.push_back(makePng(2, 2, "metal", [](u32, u32, u8 p[4]) { p[0] = 50; p[1] = 60; p[2] = 70; p[3] = 80; }));
        std::vector<fmt::ImportedMaterial> mats(1);
        mats[0].name = "Chan";
        mats[0].metalRoughTex = tex(0, 'b');       // roughness lives in the blue channel of its file
        mats[0].metallicTex = tex(1, 'g');
        fmt::packSeparateMetalRough(images, mats);
        ImageData out;
        check(images.size() == 3 && decode(images[2], out) && out.valid() &&
              out.pixels[1] == 3 && out.pixels[2] == 60, "the channel each source named is the one read");
    }

    AVER_INFO("=== no roughness map: the factor becomes a constant plane ===");
    {
        std::vector<fmt::ImportedImage> images;
        images.push_back(makePng(2, 2, "metal", [](u32, u32, u8 p[4]) { p[0] = 200; p[1] = p[2] = 0; p[3] = 255; }));
        std::vector<fmt::ImportedMaterial> mats(1);
        mats[0].name = "Plain";
        mats[0].metallicTex = tex(0);
        mats[0].roughnessFactor = 0.5f;
        mats[0].metallicFactor = 1.0f;
        fmt::packSeparateMetalRough(images, mats);
        ImageData out;
        check(images.size() == 2 && decode(images[1], out) && out.width == 2 && out.valid(), "a packed image the size of the metalness map");
        check(out.pixels[1] == 128 && out.pixels[2] == 200, "G is 0.5 * 255 rounded, B the metalness");
        check(mats[0].roughnessFactor == 1.0f && mats[0].metallicFactor == 1.0f, "the factors reset to 1");
    }

    AVER_INFO("=== materials that need no packing are untouched ===");
    {
        std::vector<fmt::ImportedImage> images;
        images.push_back(makePng(2, 2, "orm", [](u32, u32, u8 p[4]) { p[0] = 255; p[1] = 90; p[2] = 40; p[3] = 255; }));
        std::vector<fmt::ImportedMaterial> mats(2);
        // Already the packed layout: the same image, G and B.
        mats[0].name = "Packed";
        mats[0].metalRoughTex = tex(0, 'g');
        mats[0].metallicTex = tex(0, 'b');
        mats[0].roughnessFactor = 0.7f;
        // An old importer's material: no metallicTex at all.
        mats[1].name = "Old";
        mats[1].metalRoughTex = tex(0);
        mats[1].roughnessFactor = 0.3f;
        mats[1].metallicFactor = 0.2f;
        const std::vector<u8> before = images[0].bytes;
        check(fmt::packSeparateMetalRough(images, mats) == 0, "nothing was packed");
        check(images.size() == 1 && images[0].bytes == before, "no image was added or changed");
        check(mats[0].metallicTex.empty() && mats[0].metalRoughTex.imageIndex == 0 && mats[0].roughnessFactor == 0.7f,
              "the already-packed pair keeps its map and factor, and only metallicTex is cleared");
        check(mats[1].metalRoughTex.imageIndex == 0 && mats[1].roughnessFactor == 0.3f && mats[1].metallicFactor == 0.2f,
              "a material with no metallicTex is exactly as it was");
    }

    AVER_INFO("=== an unreadable metalness map is dropped, not fatal ===");
    {
        std::vector<fmt::ImportedImage> images;
        images.push_back(makePng(2, 2, "rough", [](u32, u32, u8 p[4]) { p[0] = p[1] = p[2] = 10; p[3] = 255; }));
        fmt::ImportedImage junk;
        junk.bytes = {1, 2, 3, 4, 5};
        junk.ext = ".png";
        junk.suggestedName = "junk";
        junk.ok = true;
        images.push_back(junk);
        std::vector<fmt::ImportedMaterial> mats(1);
        mats[0].name = "Bad";
        mats[0].metalRoughTex = tex(0);
        mats[0].metallicTex = tex(1);
        check(fmt::packSeparateMetalRough(images, mats) == 0, "nothing was packed");
        check(images.size() == 2 && mats[0].metallicTex.empty() && mats[0].metalRoughTex.imageIndex == 0,
              "metallicTex is cleared and the roughness map is left bound");
    }

    AVER_INFO(g_failures == 0 ? "FbxMaterialPackTest: PASS" : "FbxMaterialPackTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
