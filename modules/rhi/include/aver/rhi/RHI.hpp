#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>
#include <vector>

// Aver RHI — the single render-hardware abstraction every GPU consumer targets.
// Backends (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend always
// exists as a fallback so the engine runs headless / on unsupported hardware.
namespace aver::rhi {

enum class Backend { Null, D3D12, D3D11, Vulkan };
const char* backendName(Backend b);

struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 2;
};

class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    virtual void present() = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

// Interleaved mesh vertex: position + normal (engine space, cm) + UV0. Enough for lit, textured
// solid rendering; tangents/skin come with the asset mesh pipeline later. There is deliberately NO
// tangent field: nothing authored has one, and a zero tangent that normalize() turns into NaN is
// the documented 0x141 TDR failure mode on this hardware. The material shading derives its tangent
// frame from ddx/ddy of world position and UV instead.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};

// Every consumer of this struct reads it through something that cannot be checked at compile time:
// the D3D12 input layout names offsets as literals, the mesh-shader path binds the buffer as a RAW
// root SRV whose element size comes solely from HLSL `struct MeshVtx`, and an input layout naming
// fewer elements than the buffer holds is legal D3D12. One byte of drift is silent in all three.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI");
static_assert(offsetof(MeshVertex, px) == 0,
              "position must stay first: the DXR BLAS description points at the vertex buffer base");

// MeshHandle / LineHandle are declared in RHIResources.hpp so feature modules can name geometry
// without pulling in the whole device interface.

// Line vertex: position + colour (unlit), for grids/gizmos/debug.
struct LineVertex {
    f32 px, py, pz;
    f32 r, g, b;
};

struct DeviceDesc {
    // Preference order; createDevice() returns the first compiled-in backend that
    // initialises, falling back to Null.
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
};

// What the physical device can actually do. Queried once at init; consumers (e.g. the Voxi
// render module) use it to decide which quality settings are offerable vs greyed out, so the
// UI never advertises a feature the hardware cannot run.
struct DeviceCaps {
    u32 msaaMask = 1;            // bit N set => N samples supported (bits 1,2,4,8)
    u32 maxMsaaSamples = 1;      // highest supported sample count (1 = no MSAA)
    u32 rayTracingTier = 0;      // 0 = none, 10 = DXR 1.0, 11 = DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;      // needed for voxel radiance read-modify-write
    bool conservativeRaster = false; // needed for watertight voxelization
    u32 shaderModel = 50;            // 51 = SM 5.1, 60 = SM 6.0, 65 = SM 6.5, ...
    u32 meshShaderTier = 0;          // 0 = none, 1 = Tier 1 (D3D12 Ultimate)
    bool dxcAvailable = false;       // DXIL compiler present (needed for SM 6.x)
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual DeviceCaps caps() const { return {}; }

    // Generic resource creation for render-feature modules. Returns nullptr on backends without
    // GPU support (Null / the D3D11 + Vulkan stubs), which is how a feature declines to initialise
    // instead of failing the engine.
    virtual IResourceFactory* resources() { return nullptr; }

    // Render-feature registration. NON-owning: the caller keeps the feature alive.
    virtual void addRenderFeature(IRenderFeature* f) { (void)f; }
    virtual void removeRenderFeature(IRenderFeature* f) { (void)f; }

    // Target formats a feature must match when building pipelines that draw into the scene.
    virtual Format backbufferFormat() const { return Format::Unknown; }
    virtual Format depthFormat() const { return Format::Unknown; }

    // Anti-aliasing sample count. Changing it rebuilds the scene targets and every PSO, so it
    // is a real (if heavyweight) runtime setting. Returns false if the count is unsupported.
    virtual u32 sampleCount() const { return 1; }
    virtual bool setSampleCount(u32 samples) { (void)samples; return false; }
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

    // Frame clear colour (linear RGBA, 0..1). Default no-op for backends without a target.
    virtual void setClearColor(f32 r, f32 g, f32 b, f32 a) { (void)r; (void)g; (void)b; (void)a; }

    // Confine scene rendering to a sub-rectangle of the backbuffer, in physical pixels with a
    // top-left origin. Used by the editor so the 3D view fills only the dockspace's central
    // node instead of the whole window. (0,0,0,0) = full backbuffer.
    virtual void setViewportRect(u32 x, u32 y, u32 w, u32 h) { (void)x; (void)y; (void)w; (void)h; }

    // GPU self-test: clear a tiny offscreen target to `in` and read the pixel back into
    // `outRGBA`. Returns true if the read-back matches (proves the GPU path works). A
    // backend without real GPU support returns false.
    virtual bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) { (void)inRGBA; (void)outRGBA; return false; }

    // ----- Immediate lit-mesh rendering (Phase 3) -----
    // Upload a static mesh (positions+normals+indices); returns a handle (0 = failure).
    virtual MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount,
                                  const u32* indices, u32 indexCount) {
        (void)verts; (void)vertexCount; (void)indices; (void)indexCount; return 0;
    }
    // Per-frame camera (row-major, row-vector viewProj = view*proj). invViewProj is used
    // to reconstruct world-space rays for the procedural sky.
    virtual void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 cameraPos[3]) {
        (void)viewProj; (void)invViewProj; (void)cameraPos;
    }
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // Procedural sky + distance-fog atmosphere. When enabled, a gradient sky (with a sun
    // disk along the light direction) is drawn behind the scene and meshes fade to fogColor.
    virtual void setSky(bool enabled, const f32 zenith[3], const f32 horizon[3],
                        const f32 fogColor[3], f32 fogDensity) {
        (void)enabled; (void)zenith; (void)horizon; (void)fogColor; (void)fogDensity;
    }
    // Record one draw of `mesh` with a world matrix (row-major), base colour, and PBR
    // metallic/roughness (0..1).
    virtual void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
    }

    // Unlit line geometry (grid, gizmos): per-vertex colour, drawn as a line list.
    virtual LineHandle createLineMesh(const LineVertex* verts, u32 count) { (void)verts; (void)count; return 0; }
    virtual void drawLines(LineHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }

    // Mesh shader geometry path (D3D12 Ultimate: mesh-shader Tier 1 + SM 6.5). Replaces the
    // input-assembler vertex path for every draw, which is why it lives here and not in a render
    // feature: it is a property of how geometry reaches the rasteriser, not of any one effect.
    // Ignored when unavailable.
    //
    // Global illumination, shadows and ray tracing used to be declared alongside this. They are a
    // render feature's business now (see IRenderFeature in RHIResources.hpp) and the app configures
    // that feature directly, so no vocabulary they introduced survives in this interface.
    virtual void setMeshShaders(bool enabled) { (void)enabled; }
    virtual bool meshShadersActive() const { return false; }

    // Render subsequent meshes as wireframe until toggled off.
    virtual void setWireframe(bool on) { (void)on; }

    // Line depth testing. Default true (grids occlude behind geometry); set false to draw
    // subsequent lines as an always-on-top overlay (editor gizmos) until toggled back on.
    virtual void setLineDepth(bool testDepth) { (void)testDepth; }

    // Verification: capture the backbuffer pixel at (x,y) during the next presented
    // frame. Poll getCapture() afterward. Lets tests confirm objects actually rasterize.
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
    // Full captured frame (tight RGBA8, top-to-bottom) after a requestCapture completes.
    virtual bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) { (void)outRGBA; (void)w; (void)h; return false; }

    // ----- In-window UI (Dear ImGui) -----
    // Initialise ImGui on this device for the given native window. Returns false if the
    // backend has no UI support. After init, build widgets between uiNewFrame() (called
    // by the engine after beginFrame) and endFrame() (which records the UI draw data).
    virtual bool uiInit(void* windowHandle) { (void)windowHandle; return false; }
    virtual void uiNewFrame() {}
    virtual void uiShutdown() {}
    virtual bool uiActive() const { return false; }
    virtual bool uiWantsMouse() const { return false; }    // true when the cursor is over UI
    virtual bool uiWantsKeyboard() const { return false; }

    // Make a texture drawable by the UI, returning the identifier the UI layer expects. It is a
    // plain integer here on purpose: the UI's own texture-handle type must not cross into the RHI
    // headers, or every consumer of this header acquires a dependency on the UI library.
    //
    // The descriptor is allocated once and cached on the texture, so calling this every frame is
    // free. Returns 0 where the backend hosts no UI, which the caller treats as "no image".
    virtual u64 uiTextureId(TextureHandle t) { (void)t; return 0; }
};

IDevice* createDevice(const DeviceDesc& desc = {});
void destroyDevice(IDevice* device);

// ----- UI (Dear ImGui) window-message routing -----
// A backend that hosts ImGui registers a handler here; the platform Window forwards raw
// messages to uiWndProc so ImGui receives input without Platform depending on the UI.
using UiWndProcFn = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
void registerUiWndProc(UiWndProcFn fn);
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam);

} // namespace aver::rhi
