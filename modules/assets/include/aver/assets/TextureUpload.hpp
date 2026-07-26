#pragma once
// The one place a decoded image becomes a GPU texture.
//
// This is the missing rung between fmt::loadTexture (bytes -> mip chain) and
// rhi::IResourceFactory::createTexture (mip chain -> handle), and it lives in its OWN target for a
// dependency reason rather than a tidiness one. Aver.Formats must not name the RHI, and the RHI must
// not know what a normal map is (see Texture.hpp) -- so the join has to happen above both. Every
// host that renders materials needs the same forty lines, so they are written once here instead of
// once per app.
//
// Deliberately STATELESS: no cache lives here. pbr::MaterialSystem already owns the resolved handles
// and destroys them at shutdown, and a second cache would be a second owner of the same texture.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>

namespace aver::assets {

// What the PIXELS mean. This is the only input that decides BOTH how mips are filtered and which
// view format the hardware decodes through, which is why it is one answer and not two flags a caller
// could set inconsistently. No image container states it, so it always comes from the call site.
enum class TextureUsage : u32 {
    Colour = 0,   // sRGB-encoded: base colour, emissive
    Data,         // linear: metallic-roughness, occlusion
    NormalMap,    // linear AND filtered as vectors, not as colours
};

// Reported back so a caller can log or budget without decoding the file a second time.
struct TextureUploadInfo {
    u32   width  = 0;
    u32   height = 0;
    u32   mips   = 0;
    usize bytes  = 0;   // total across the whole chain, as uploaded
};

// 0 on any failure, with the reason in `err` when one is asked for. Failure is NOT fatal to a
// material: the caller keeps the slot's identity fallback, which is a complete surface rather than a
// black one.
rhi::TextureHandle uploadTexture(rhi::IResourceFactory& res, const std::string& path,
                                 TextureUsage usage, std::string* err = nullptr,
                                 TextureUploadInfo* info = nullptr);

} // namespace aver::assets
