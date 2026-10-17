#pragma once
// Soft-body viewport overlay (Show > Soft Bodies): every beam of every cloth and destructible coloured by its
// strain (blue compressed, green at rest, red at its break strain, a white tint for the permanent set, BROKEN
// beams magenta), pinned particles as yellow crosses, piece bounds, the wind arrow, and the budget line. The
// strain shown is the same Beam data the damage step uses, so what you see is what will tear.
//
// Pure feed -> rhi::LineVertex, drawn like the motion overlay (see MotionDebugOverlay.hpp): once per frame while
// an option is on, softBodyDebugCollect(opts, frame), upload frame.lines as a line mesh and draw it with an
// identity matrix, then softBodyDebugDrawLabels inside the viewport window.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"
#endif

#include <string>
#include <vector>

namespace aver::editor {

struct SoftBodyDebugOptions {
    bool strain = false;    // every beam coloured by strain
    bool pins = false;      // pinned particles
    bool pieces = false;    // piece bounds
    bool wind = false;      // the wind arrow
    bool budget = false;    // pieces / active particles / rebuilds line
    bool any() const { return strain || pins || pieces || wind || budget; }
};

struct SoftBodyDebugLabel {
    Vec3 world;
    std::string text;
    u32 rgba = 0xFFFFFFFFu;
};

struct SoftBodyDebugFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    std::vector<SoftBodyDebugLabel> labels;
    std::string budgetText;               // drawn in the corner of the viewport
    void clear() { lines.clear(); labels.clear(); budgetText.clear(); }
};

// The colour of a beam: strain = (length - rest0) / rest0, breakStrain the material's limit (0 = 0.3), plastic01 the
// permanent set as a fraction of the beam (0..1, tints toward white), broken = magenta. Pure; a test pins it.
void softBodyStrainColour(f32 strain, f32 breakStrain, f32 plastic01, bool broken, f32& r, f32& g, f32& b);

#if AVER_MODULE_SOFTBODY_SCENE
// Appends one cage: its beams by strain and its pins.
void softBodyDebugFeedCage(const softbody::Cage& cage, const SoftBodyDebugOptions& opts, SoftBodyDebugFrame& out);

// Clears `out`, then feeds every entity with a soft-body entry in `sys` (the process-global world's CSoftBody pool).
void softBodyDebugCollect(const SoftBodyDebugOptions& opts, softbody::SoftBodySystem& sys, SoftBodyDebugFrame& out);
#endif

#if AVER_WITH_IMGUI
// The budget line in the viewport's top-left corner.
void softBodyDebugDrawBudget(const SoftBodyDebugFrame& frame, f32 vpX, f32 vpY);
#endif

}  // namespace aver::editor
