// sr::Quality: the four AverSR quality levels named in docs/AVERSR.md ("Quality levels") and in
// this module's own naming table ("Types | unprefixed inside aver::sr | sr::Quality"). Header-only
// and dependency-free beyond aver/core/Types.hpp -- this is a caller-facing convenience (a name and
// a table lookup), not part of the rhi::IUpscaler seam itself, so it carries no RHI include and no
// GPU call of its own.
#pragma once

#include "aver/core/Types.hpp"

#include <cctype>
#include <cstring>

namespace aver::sr {

// `Off` renders at native resolution and runs no upscaler at all -- the pre-existing behaviour, and
// it must stay bit-identical to a tree with no AverSR in it (see docs/AVERSR.md "Quality levels").
// The other three map onto rhi::IDevice::setRenderScale; AverSR does not have a second resolution
// concept of its own. `Quality::Quality` (the enumerator sharing its name with the type) reads oddly
// but is the exact word docs/AVERSR.md's table uses, and inventing a different one here would just
// be a second name for the same level.
enum class Quality : u32 { Off = 0, Quality = 1, Balanced = 2, Performance = 3 };

// The render-scale table from docs/AVERSR.md, and the one place it is spelled out as numbers -- a
// caller (the editor's --aversr flag, a render-settings combo, a game's own settings menu) asks
// this rather than hard-coding 0.67/0.58/0.50, so there is exactly one place to change if the table
// ever does.
inline f32 renderScaleFor(Quality q) {
    switch (q) {
        case Quality::Quality:     return 0.67f;
        case Quality::Balanced:    return 0.58f;
        case Quality::Performance: return 0.50f;
        case Quality::Off:         return 1.00f;
    }
    return 1.00f;
}

// The brand-layer name for a level, matching docs/AVERSR.md's table exactly -- what a log line or a
// UI combo should show, never a hand-typed string that can drift from the table.
inline const char* qualityName(Quality q) {
    switch (q) {
        case Quality::Quality:     return "Quality";
        case Quality::Balanced:    return "Balanced";
        case Quality::Performance: return "Performance";
        case Quality::Off:         return "Off";
    }
    return "Off";
}

// Case-insensitive parse of a CLI/UI string ("off", "quality", "balanced", "performance") into a
// Quality. Returns false and leaves `out` untouched on anything else, so a caller can tell a typo
// from an explicit Off rather than silently defaulting one into the other.
inline bool parseQuality(const char* s, Quality& out) {
    if (!s || !*s) return false;
    auto ieq = [](const char* a, const char* b) {
        for (; *a && *b; ++a, ++b)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
        return *a == *b;
    };
    if (ieq(s, "off"))         { out = Quality::Off;         return true; }
    if (ieq(s, "quality"))     { out = Quality::Quality;     return true; }
    if (ieq(s, "balanced"))    { out = Quality::Balanced;    return true; }
    if (ieq(s, "performance")) { out = Quality::Performance; return true; }
    return false;
}

} // namespace aver::sr
