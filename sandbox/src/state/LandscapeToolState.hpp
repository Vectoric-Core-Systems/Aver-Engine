// Landscape, foliage and water-spline tools: brush parameters, stroke/drag state, species palette.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct LandscapeToolState {
#if AVER_MODULE_LANDSCAPE
#if AVER_MODULE_SCENE && AVER_WITH_IMGUI
    bool    foliageStroking_ = false;
    f32     foliageAccum_    = 0.0f;
    // One entry per instance this session's brush placed.
    struct FoliagePlaced { Vec3 pos; f32 solidRadiusCm; };
    std::vector<FoliagePlaced> foliagePlaced_;
#endif
#endif
    // Create-a-landscape knobs: grid resolution + tile spacing (read by landscape mode panel).
    int landCreateSamples_ = 513;
    f32 landCreateSpacingCm_ = 100.0f;
#if AVER_MODULE_LANDSCAPE
    // 1.0 is the smoothstep the brush always had (see BrushParams::falloff).
    f32 sculptFalloff_ = 1.0f;
    // One entry per .ocfoliage TYPE ASSET, not per mesh. Reusable asset, not a global brush setting (see OcFoliage.hpp).
    struct FoliageSpecies {
        std::string name;         // display name, the file stem
        std::string assetPath;    // the .ocfoliage this came from -- opened by the palette's Edit button
        fmt::OcFoliageData type;  // mesh/material/scale/weight/randomizeYaw/collisionRadiusCm/alignToNormal
        bool         enabled = true;
    };
    std::vector<FoliageSpecies> foliagePalette_;
    // BRUSH-ONLY: tool held vs. what it places; instance randomisation lives per type above.
    f32  foliageRadiusCm_    = 800.0f;
    f32  foliageDensity_     = 6.0f;     // attempted placements per brush application
    bool foliageErase_       = false;    // Shift: remove instead of place
    u32  foliageSeed_        = 1u;       // one running RNG stream for the whole paint session
    // ---- wave3 layers/rivers: the Paint and Water tabs of the Landscape mode panel (LandscapePaintTool.cpp, WaterSplineTool.cpp) ----
    enum class LandscapeTab { Sculpt, Paint, Water };
    LandscapeTab landscapeTab_ = LandscapeTab::Sculpt;
    // Paint
    f32  paintRadiusCm_ = 600.0f, paintStrength_ = 0.6f, paintFalloff_ = 1.0f, paintOpacity_ = 1.0f;
    int  paintMode_ = 0;                  // 0 Paint, 1 Erase, 2 Smooth (landscape::PaintMode)
    int  paintLayer_ = 0;                 // the selected MATERIAL layer
    bool painting_ = false;               // LMB down with the paint tool
    f32  autoSlopeMinDeg_ = 25.0f, autoSlopeMaxDeg_ = 90.0f, autoFeatherDeg_ = 5.0f;
    f32  autoZMinCm_ = -100000.0f, autoZMaxCm_ = 100000.0f;
    // Water: bodies live in levelHeader_.rivers / lakes (rivers first in the selection index).
    int  waterSel_ = -1;                  // selected body
    int  waterPt_ = -1;                   // selected control point of it
    bool waterDrag_ = false;              // dragging that point
    bool waterGestureOpen_ = false;       // an edit gesture is in progress (the snapshot below is its "before")
    std::vector<fmt::OcRiver> waterGestureRivers_;
    std::vector<fmt::OcLake>  waterGestureLakes_;
    bool waterBodiesStale_ = false;       // the records changed: rebuild meshes and the query next frame
    rhi::LineHandle waterLines_ = 0;      // the selected body's centreline, bank edges and handles
    u64  waterLinesKey_ = 0;
    // Sculpt tool state: radius/strength shared across all brush modes.
    f32 sculptRadiusCm_ = 500.0f;
    f32 sculptStrengthCm_ = 150.0f;
    bool sculpting_ = false;              // LMB down, over the terrain, with a sculpt tool active
    f32 sculptFlattenTargetCm_ = 0.0f;    // captured once per stroke -- see BrushParams::flattenTargetCm
    f32 sculptRampStartCm_[2] = {0.0f, 0.0f};   // captured once per stroke -- see BrushParams::rampStartCm
    f32 sculptRampStartHeightCm_ = 0.0f;        // captured once per stroke -- see BrushParams::rampStartCm
    u32 sculptNoiseSeed_ = 0;                   // captured once per stroke -- see BrushParams::noiseSeed
    bool sculptCursorValid_ = false;      // true when this frame's cursor ray actually hit the section
    Vec3 sculptCursor_{0, 0, 0};          // world hit point, for the brush-radius ring and the next tick
    rhi::LineHandle brushRing_ = 0;       // a unit ring in the XY plane -- landscape heights run +Z
#endif
};
