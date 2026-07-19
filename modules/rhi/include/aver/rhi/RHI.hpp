#pragma once
#include "aver/core/Types.hpp"

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

// Interleaved mesh vertex: position + normal (engine space, cm). Enough for lit
// solid rendering; UVs/tangents/skin come with the asset mesh pipeline later.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
};

using MeshHandle = u32; // 0 = invalid

struct DeviceDesc {
    // Preference order; createDevice() returns the first compiled-in backend that
    // initialises, falling back to Null.
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

    // Frame clear colour (linear RGBA, 0..1). Default no-op for backends without a target.
    virtual void setClearColor(f32 r, f32 g, f32 b, f32 a) { (void)r; (void)g; (void)b; (void)a; }

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
    // Per-frame camera (row-major, row-vector viewProj = view*proj) and directional light.
    virtual void setCamera(const f32 viewProj[16], const f32 cameraPos[3]) { (void)viewProj; (void)cameraPos; }
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // Record one draw of `mesh` with a world matrix (row-major) and base colour.
    virtual void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4]) {
        (void)mesh; (void)world; (void)baseColor;
    }

    // Verification: capture the backbuffer pixel at (x,y) during the next presented
    // frame. Poll getCapture() afterward. Lets tests confirm objects actually rasterize.
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
};

IDevice* createDevice(const DeviceDesc& desc = {});
void destroyDevice(IDevice* device);

} // namespace aver::rhi
