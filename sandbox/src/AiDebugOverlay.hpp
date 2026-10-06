#pragma once
// AI debug viewport overlays: sight cones, hearing radii, current path, steering vectors and BT state
// above heads. A pure feed (synapse::AiDebugSink) plus built-in feeders for the components this
// editor can see (CSynapsePerception, CSynapseAgent, CSynapseBehavior); other AI systems add their
// own through AiDebugSink::addProvider, so hearing, steering and cover show up without this file
// knowing about them.
//
// Wiring (see docs/BLACKBOARD_BT.md): a Show menu entry per option in AiDebugOptions, and once per
// frame in the viewport draw: aiDebugCollect(opts), upload aiDebugLineVertices() as a line mesh
// drawn like the navigation overlay, then aiDebugDrawLabels() inside the viewport window.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/synapse/AiDebug.hpp"

#include <vector>

namespace aver::editor {

struct AiDebugOptions {
    bool sight = false;
    bool hearing = false;
    bool path = false;
    bool steering = false;
    bool btState = false;
    bool cover = false;
    f32  labelMaxDistanceCm = 4000.0f;   // BT state text is dropped beyond this

    u32  mask() const;
    bool any() const { return mask() != 0; }
};

// Resets the sink, enables the options' categories, runs the built-in feeders and then every
// provider. Call once per frame while any option is on; with none on it only clears the sink.
void aiDebugCollect(const AiDebugOptions& options, synapse::AiDebugSink& sink = synapse::aiDebug());

// Two vertices per line, colours 0..1; ready for rhi createLineMesh / drawLines.
std::vector<rhi::LineVertex> aiDebugLineVertices(const synapse::AiDebugSink& sink);

// World point to viewport pixels (row-vector matrix, y down). False behind the eye.
bool aiDebugProject(const Mat4& viewProj, const Vec3& world, f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                    f32& outX, f32& outY);

#if AVER_WITH_IMGUI
// Draws the sink's text labels on the foreground draw list, skipping labels beyond maxDistCm of `eye`.
void aiDebugDrawLabels(const synapse::AiDebugSink& sink, const Mat4& viewProj, f32 vpX, f32 vpY,
                       f32 vpW, f32 vpH, const Vec3& eye, f32 maxDistCm);
#endif

} // namespace aver::editor
