// PhysicsDebugRenderer.hpp -- the line sink behind physics_debug_abi.h. SRC-LOCAL like PhysicsInternal.hpp: it names JPH:: types.
// Only compiled into anything when Jolt was built with JPH_DEBUG_RENDERER.
//
// A JPH::DebugRendererSimple that collects lines and text into buffers in the ENGINE's frame (cm, +X forward, +Y right, +Z up),
// laid out exactly as two rhi::LineVertex. Jolt's shape geometry arrives as triangle batches; this sink turns each batch into its
// FEATURE EDGES once, when the batch is created (an edge shared by two coplanar triangles is a tessellation diagonal and is dropped,
// shared edges are drawn once), so a box is its 12 edges and not 36 lines. DrawLine is called from worker threads while a step
// runs (contacts, supporting faces, continuous-collision markers), so every push takes the mutex.
#pragma once
#include "PhysicsInternal.hpp"

#ifdef JPH_DEBUG_RENDERER

#include <Jolt/Renderer/DebugRendererSimple.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aver::physics::detail {

// (x y z r g b) of the start, then of the end.
struct DebugLine {
    float a[3], ca[3], b[3], cb[3];
};
static_assert(sizeof(DebugLine) == 12 * sizeof(float), "a line is two rhi::LineVertex");

struct DebugLabelRec {
    float pos[3];
    float rgb[3];
    std::string text;
};

class LineSinkRenderer final : public JPH::DebugRendererSimple {
public:
    static constexpr size_t kArmedCap = 65536;     // lines kept from one fixed step
    static constexpr size_t kLabelCap = 2048;

    // DebugRendererSimple's constructor ran Initialize() while this class's overrides did not exist yet, so the cached box, sphere,
    // capsule, cone and cylinder geometry holds ITS batches, which DrawGeometry below would misread as ours. Run it again here:
    // every cached geometry is rebuilt through CreateTriangleBatch above, and the first set is released.
    LineSinkRenderer() { Initialize(); }

    // ---- one collect (main thread, between steps) ----
    // camCm null = no culling and no LOD hint. radiusCm <= 0 = unlimited.
    void beginCollect(const float* camCm, float radiusCm, int maxLines);
    bool truncated() const { return truncated_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::vector<DebugLine>& lines() const { return lines_; }
    const std::vector<DebugLabelRec>& labels() const { return labels_; }
    bool full() const { return static_cast<int>(lines_.size()) >= maxLines_; }
    // Appends the last fixed step's captured lines (within the radius) to the collect, honouring the budget.
    void appendArmed();

    // ---- armed capture (steps) ----
    void setArmed(uint32_t mask);
    uint32_t armed() const { return armed_mask_.load(std::memory_order_relaxed); }
    void stepBegin();
    void stepEnd() { in_step_.store(false, std::memory_order_relaxed); }

    // Jolt's view of the camera, metres (the level-of-detail pick); the engine-frame copy is kept for culling.
    bool cameraSet() const { return have_camera_; }
    JPH::Vec3 cameraJolt() const { return cam_jolt_; }
    float radiusMetres() const { return radius_cm_ > 0.0f ? radius_cm_ / kCmPerMetre : -1.0f; }

    // ---- JPH::DebugRenderer ----
    void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
    void DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight) override;

protected:
    Batch CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) override;
    Batch CreateTriangleBatch(const Vertex* inVertices, int inVertexCount, const JPH::uint32* inIndices, int inIndexCount) override;
    void DrawGeometry(JPH::RMat44Arg inModelMatrix, const JPH::AABox& inWorldSpaceBounds, float inLODScaleSq, JPH::ColorArg inModelColor,
                      const GeometryRef& inGeometry, ECullMode inCullMode, ECastShadow inCastShadow, EDrawMode inDrawMode) override;

private:
    // A batch reduced to its feature edges: positions in model space and index pairs with the colour of the first triangle that had the edge.
    class EdgeBatch final : public JPH::RefTargetVirtual {
    public:
        JPH_OVERRIDE_NEW_DELETE
        void AddRef() override { ++ref_count_; }
        void Release() override { if (--ref_count_ == 0) delete this; }
        std::vector<JPH::Float3> verts;
        struct Edge { JPH::uint32 a, b; JPH::Color color; };
        std::vector<Edge> edges;
    private:
        std::atomic<JPH::uint32> ref_count_{0};
    };
    Batch makeBatch(const std::vector<Vertex>& tris);   // 3 vertices per triangle

    bool farFromCamera(const float p[3]) const;

    std::mutex mutex_;
    std::vector<DebugLine> lines_;
    std::vector<DebugLine> step_lines_;
    std::vector<DebugLabelRec> labels_;
    int maxLines_ = 0;
    bool truncated_ = false;
    bool have_camera_ = false;
    float cam_cm_[3] = {0, 0, 0};
    float radius_cm_ = 0.0f;
    JPH::Vec3 cam_jolt_ = JPH::Vec3::sZero();
    std::atomic<uint32_t> armed_mask_{0};
    std::atomic<bool> in_step_{false};
};

// The one renderer, created on first use (Jolt allows one DebugRenderer::sInstance).
LineSinkRenderer& debugSink();

} // namespace aver::physics::detail

#endif // JPH_DEBUG_RENDERER
