// Decodes an image file and creates the GPU texture for it.
#include "aver/assets/TextureUpload.hpp"

#include "aver/formats/Texture.hpp"

#include <cmath>

namespace aver::assets {

// Decodes `path` and uploads it. Returns 0 on failure, with the reason in `err`.
rhi::TextureHandle uploadTexture(rhi::IResourceFactory& res, const std::string& path,
                                 TextureUsage usage, std::string* err, TextureUploadInfo* info) {
    if (path.empty()) {
        if (err) *err = "empty texture path";
        return 0;
    }

    fmt::TextureLoadOptions opt;
    opt.srgb        = usage == TextureUsage::Colour;
    opt.normalMap   = usage == TextureUsage::NormalMap;
    opt.generateMips = true;

    fmt::TextureData tex;
    if (!fmt::loadTexture(path, tex, opt, err)) return 0;
    if (!tex.valid()) {
        if (err) *err = "decoded texture is empty: " + path;
        return 0;
    }

    // One CPU pointer per mip, in level order, which is the subresource order TextureDesc documents.
    std::vector<const void*> levels;
    levels.reserve(tex.levels.size());
    usize bytes = 0;
    for (const ImageData& lvl : tex.levels) {
        levels.push_back(lvl.pixels.data());
        bytes += lvl.pixels.size();
    }

    rhi::TextureDesc d;
    d.width  = tex.width;
    d.height = tex.height;
    d.mips   = static_cast<u32>(levels.size());
    d.format = tex.srgb ? rhi::Format::RGBA8UnormSrgb : rhi::Format::RGBA8Unorm;
    d.bind   = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.initialData      = levels.data();
    d.initialDataCount = static_cast<u32>(levels.size());
    d.initialRowPitch  = tex.levels.front().rowPitch();
    d.debugName = path.c_str();

    const rhi::TextureHandle h = res.createTexture(d);
    if (!h) {
        if (err) *err = "the GPU texture could not be created: " + path;
        return 0;
    }
    if (info) {
        *info = TextureUploadInfo{tex.width, tex.height, d.mips, bytes};
        // The last level is 1x1 (generateMipChain loops until both extents are 1). Its texel is
        // stored in the texture's OWN encoding, so an sRGB texture needs decoding back to linear
        // -- downsample() averaged in linear and then re-encoded, and handing the encoded byte
        // straight out would report a colour roughly twice as bright as the texture really is.
        const ImageData& last = tex.levels.back();
        if (last.pixels.size() >= 4) {
            for (int c = 0; c < 3; ++c) {
                const f32 v = static_cast<f32>(last.pixels[c]) / 255.0f;
                info->averageLinear[c] = tex.srgb
                    ? (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f))
                    : v;
            }
        }
    }
    return h;
}

} // namespace aver::assets
