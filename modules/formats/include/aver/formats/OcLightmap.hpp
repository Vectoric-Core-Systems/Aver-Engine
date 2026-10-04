#pragma once
// .oclightmap — a baked irradiance atlas for ONE static mesh instance. AVR1 container (the same
// container GiCache.cpp's .cache entries sit on), subtype 'LMAP'. Chosen rather than reusing GIVL:
// a lightmap is a per-INSTANCE bake keyed to one placement's UV unwrap, not a per-VOLUME bake keyed
// to a voxel grid, and the two should never parse as each other by accident of sharing a tag.
//
// TEXEL ENCODING: RGB9E5, NOT RGBA16F. GiCache's GVOX chunk already picked half-float for its
// volume and that was the right call there -- a voxel can go negative in nobody's model of light,
// but the SAMPLER reads it as a directional SH-ish term in some GI schemes, so a sign bit earns its
// keep. A lightmap texel never does: it is accumulated outgoing irradiance, which is a sum of
// non-negative contributions and cannot be negative by construction. Spending a sign bit nothing
// here will ever use is the wrong trade against RGB9E5's actual cost, which is real: a SHARED
// exponent means the three channels are quantised to the same scale, so a channel much dimmer than
// the brightest one in the same texel loses precision to it (a saturated blue highlight next to a
// near-zero red component, say). Baked GI is overwhelmingly white-ish indirect light rather than
// deeply saturated colour, which is the case this format is betting on.
//
// What that buys back: RGB9E5 packs into 4 bytes/texel against RGBA16F's 8 (or 6, if a packed
// RGB16F existed as a real GPU format, which neither D3D12 nor Vulkan ship) -- half the atlas
// memory and half the disk footprint for a format whose whole reason to exist is that atlases get
// large. And it makes "this texel is negative" a type error rather than a runtime one: RGB9E5 has
// no sign bit to mis-set, which closes off exactly the class of bug the negative-radiance note
// documents (acesTonemap(-1) reading as a *confident* 0.0 -- acesTonemap floors negative/NaN input
// at zero since ded8784a; it read 1.0 before that fix -- rather than an obviously broken value) for
// this data specifically -- an arithmetic slip upstream of the bake can still hand this format a
// negative float, but it cannot survive the encode, because negative numbers are not representable
// at all. See OcLightmap.cpp's encodeRGB9E5/decodeRGB9E5 for the actual bit layout (9-bit mantissa x
// 3, 5-bit shared exponent, the same shape as D3D's R9G9B9E5_SHAREDEXP / Vulkan's
// E5B9G9R9_UFLOAT_PACK32) -- restated here rather than shared with any GPU-side code, because
// nothing in this engine samples a lightmap on the GPU yet; this is CPU-side storage only, decoded
// to plain floats the moment anything reads it back.
//
// LIGHTMAP UVs LIVE HERE, DELIBERATELY NOT IN .ocmesh. A second UV set is exactly the kind of field
// that looks small and is not: OcMesh.hpp's own VTXS layout interleaves UV0 into a fixed per-vertex
// stride (see that file's slot-1 comment), and every importer (glTF, OBJ, USD), every mesh writer,
// and the runtime's own vertex layout would all need to grow a second stream just to carry a set of
// coordinates most meshes will never have baked. Carrying the UVs here instead means a mesh can
// GAIN a lightmap, or lose one, without being re-cooked, and the feature costs exactly the two files
// this header and its .cpp are -- nothing upstream of them has to know lightmapping exists.
//
// THE COST OF THAT CHOICE, stated rather than hidden: this format has no vertex identity to match
// against, only a COUNT. `vertexCount` is recorded at bake time and the caller (whoever is about to
// sample this lightmap against a live OcMeshData) is responsible for checking it against the mesh's
// CURRENT vertexCount() before trusting `uv` at all -- see OcLightmap::vertexCount's own comment.
// A mesh re-exported from the DCC with the same triangles but a different vertex order, a different
// welding threshold, or one extra vertex from a fixed seam invalidates the match even though nothing
// about the mesh's APPEARANCE changed, and this format cannot tell that case apart from a genuinely
// unrelated mesh -- it only has a number to compare, not the vertices themselves. `sourceHash`
// exists for the coarser, cheaper version of the same problem: a mesh that changed content but
// happens to keep the same vertex count. Neither field is checked by writeOcLightmap or
// readOcLightmap, because neither function has a live mesh to check it against -- the comparison is
// the CALLER'S job, at the point where a lightmap is about to be bound to a mesh instance.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One decoded texel: RGB irradiance as plain floats, whatever the on-disk packing was. Never
// negative -- see this header's own comment on RGB9E5 for why that is enforced by the encoding
// itself rather than by a check here.
struct LightmapTexel {
    f32 r = 0.0f, g = 0.0f, b = 0.0f;
};

// A whole baked lightmap for one static mesh instance.
struct OcLightmap {
    u32 width = 0, height = 0;     // texel grid dimensions

    // The vertex count the UVs below were unwrapped against. NOT a promise that any particular
    // mesh still has this many vertices -- see this header's own comment on why a re-export can
    // silently invalidate `uv` while leaving this field looking perfectly plausible on its own.
    u32 vertexCount = 0;

    std::string sourceMesh;        // project-relative path this bake was made for, e.g.
                                    // "Content/Meshes/Wall.ocmesh" -- for identifying a stale
                                    // lightmap, not for loading one: nothing here opens this path.
    u64 sourceHash = 0;            // that mesh's content hash at bake time, for the same purpose

    std::vector<f32> uv;                 // 2 per vertex (u, v); vertexCount*2 entries
    std::vector<LightmapTexel> texels;   // width*height entries, row-major: texel(x,y) at y*width+x

    u32 texelCount() const { return width * height; }

    // True when uv/texels are consistently sized for width/height/vertexCount. This is the same
    // split OcMeshData::valid() draws: whether the STRUCT is internally consistent, not whether it
    // is something writeOcLightmap will actually accept (that also refuses an empty sourceMesh --
    // see the .cpp).
    bool valid() const {
        return width > 0 && height > 0 && vertexCount > 0 &&
               uv.size() == usize(vertexCount) * 2 &&
               texels.size() == usize(width) * usize(height);
    }
};

// Writes an .oclightmap file. Returns false with `err` set when `lm` is not valid() (see above), or
// when sourceMesh is empty -- a lightmap naming no mesh has nothing for a caller to detect
// staleness against, which defeats the reason sourceMesh/sourceHash exist at all.
bool writeOcLightmap(const std::string& path, const OcLightmap& lm, std::string* err = nullptr);

// Reads an .oclightmap file. Returns false with `err` set on a missing file, bad magic, a truncated
// or corrupt container, a subtype that is not 'LMAP', or a chunk whose length disagrees with what
// its own header fields say it should be -- the same "checked against the arithmetic, not a stored
// size" rule GiCache's loadGiCache follows, and for the identical reason: a stored size can agree
// with a payload that is simply wrong, but width*height and vertexCount cannot.
//
// DOES NOT CHECK vertexCount AGAINST ANY MESH -- see this header's own comment above for why that
// is the caller's job, not this function's: readOcLightmap has no mesh to compare against, only the
// bytes in this one file.
bool readOcLightmap(const std::string& path, OcLightmap& out, std::string* err = nullptr);

// Bytes the UV and texel payloads occupy together: vertexCount*2 f32s, plus width*height RGB9E5
// texels at 4 bytes each. Pure arithmetic on the struct's own fields, no I/O -- the same role
// giCacheTotalBytes plays for a GI volume, and what a caller can use to estimate a lightmap's
// footprint (or a tool can use to budget an atlas) without reading the file.
usize ocLightmapBytes(const OcLightmap& lm);

} // namespace aver::fmt
