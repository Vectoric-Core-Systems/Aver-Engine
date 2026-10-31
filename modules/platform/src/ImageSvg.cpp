// SVG rasterising through the vendored NanoSVG (third_party/nanosvg; the implementation is compiled in
// ThirdPartyNanoSvg.cpp). See Image.hpp for the size rules.
//
// NanoSVG draws paths, basic shapes, gradients, strokes and dashes. It has no text, filters, masks, CSS
// beyond simple class selectors, or external references, so an icon that relies on them comes out
// without those parts rather than failing.
#include "ImageCodecs.hpp"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

namespace aver::detail {

namespace {

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

struct ImageDeleter { void operator()(NSVGimage* p) const { if (p) nsvgDelete(p); } };
struct RastDeleter { void operator()(NSVGrasterizer* p) const { if (p) nsvgDeleteRasterizer(p); } };

} // namespace

bool decodeSvg(const u8* data, usize size, u32 reqW, u32 reqH, ImageData& out, std::string* err) {
    out = ImageData{};
    if (size == 0 || size > kMaxSvgBytes) return fail(err, "SVG: the document is empty or larger than the decoder allows");
    try {
        // NanoSVG edits the text it parses, and needs it null terminated.
        std::string text(reinterpret_cast<const char*>(data), size);
        if (text.find('\0') != std::string::npos) return fail(err, "SVG: the document contains a NUL byte");
        const std::unique_ptr<NSVGimage, ImageDeleter> image(nsvgParse(text.data(), "px", 96.0f));
        if (!image) return fail(err, "SVG: the document could not be parsed");

        const f64 iw = image->width, ih = image->height;
        if (!(iw > 0.0) || !(ih > 0.0) || !std::isfinite(iw) || !std::isfinite(ih))
            return fail(err, "SVG: the document has no usable size");

        f64 scale = 1.0, tx = 0.0, ty = 0.0;
        f64 w = 0.0, h = 0.0;
        if (reqW != 0 && reqH != 0) {
            w = reqW;
            h = reqH;
            scale = std::min(w / iw, h / ih);
            tx = (w - iw * scale) * 0.5;
            ty = (h - ih * scale) * 0.5;
        } else if (reqW != 0) {
            w = reqW;
            scale = w / iw;
            h = std::max(1.0, std::ceil(ih * scale - 1e-6));
        } else if (reqH != 0) {
            h = reqH;
            scale = h / ih;
            w = std::max(1.0, std::ceil(iw * scale - 1e-6));
        } else {
            w = std::ceil(iw - 1e-6);
            h = std::ceil(ih - 1e-6);
        }
        // Checked as f64 before any conversion or allocation: a document may state 1e30 px.
        if (!(w >= 1.0) || !(h >= 1.0) || w > kMaxImageDimension || h > kMaxImageDimension)
            return fail(err, "SVG: the raster size is outside what the decoder allows");
        const u32 uw = static_cast<u32>(w), uh = static_cast<u32>(h);
        if (u64(uw) * uh > kMaxImagePixels) return fail(err, "SVG: the raster has more pixels than the decoder allows");

        const std::unique_ptr<NSVGrasterizer, RastDeleter> rast(nsvgCreateRasterizer());
        if (!rast) return fail(err, "SVG: out of memory");
        out.pixels.assign(usize(uw) * uh * 4, 0);
        nsvgRasterize(rast.get(), image.get(), static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(scale),
                      out.pixels.data(), static_cast<int>(uw), static_cast<int>(uh), static_cast<int>(uw * 4));
        out.width = uw;
        out.height = uh;
        out.channels = 4;
        return true;
    } catch (const std::bad_alloc&) {
        out = ImageData{};
        return fail(err, "SVG: out of memory");
    }
}

} // namespace aver::detail
