// 
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct BrandIconState {
    rhi::TextureHandle logoTexture_=0;   // 0 when logo.png was absent or undecodable
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    rhi::TextureHandle logoSmallTexture_=0;   // the logo area-averaged down to the menu bar's size (the full one has no mips)
    u64 logoSmallUiId_=0;
    f32 logoAspect_=1.0f;
};
