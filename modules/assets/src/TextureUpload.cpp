#include "aver/assets/TextureUpload.hpp"

#include "aver/formats/Texture.hpp"

namespace aver::assets {

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

    // One CPU pointer per mip, in level order, which is exactly the subresource order TextureDesc
    // documents. Built into a local vector because the pixels live in separate ImageData allocations
    // -- there is no single contiguous blob to hand over.
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
    // The sRGB decode is the VIEW's job, not the decoder's: the pixels stay as authored and the
    // hardware linearises on every tap, including inside the filter. Decoding on the CPU instead
    // would bake the curve into 8 bits and lose precision in the darks, where sRGB spends most of
    // its codes.
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
    if (info) *info = TextureUploadInfo{tex.width, tex.height, d.mips, bytes};
    return h;
}

} // namespace aver::assets
