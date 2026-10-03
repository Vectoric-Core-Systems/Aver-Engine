// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Dear ImGui implementation of aver::rhi::d3d12::IUiBackend (moved from D3D12Device.cpp).
#include "aver/rhi/d3d12/ImGuiUiBackend.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <string>

#include <wrl/client.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
// Forward-declared from imgui_impl_win32.h (not in its public API).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace aver::rhi::d3d12 {
namespace {

// Slots in the UI descriptor pool. ImGui 1.92 keeps several atlas textures alive at once.
struct UiSrvPool {
    static constexpr u32 kCount = 512;
    u64 cpuBase = 0, gpuBase = 0;
    u32 stride = 0;
    bool used[kCount] = {};

    bool alloc(u64* outCpu, u64* outGpu) {
        for (u32 i = 0; i < kCount; ++i) {
            if (used[i]) continue;
            used[i] = true;
            *outCpu = cpuBase + u64(i) * stride;
            *outGpu = gpuBase + u64(i) * stride;
            return true;
        }
        AVER_ERROR("[RHI.D3D12.ImGui] UI SRV descriptor pool exhausted ({} in use)", kCount);
        *outCpu = *outGpu = 0;
        return false;
    }
    void free(u64 cpu) {
        if (!stride || cpu < cpuBase) return;
        const u64 slot = (cpu - cpuBase) / stride;
        if (slot < kCount) used[slot] = false;
    }
};

// ImGui_ImplDX12_InitInfo's alloc/free callback shape (routed through UserData).
void SrvAllocThunk(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    auto* pool = static_cast<UiSrvPool*>(info->UserData);
    u64 cpu = 0, gpu = 0;
    pool->alloc(&cpu, &gpu);
    outCpu->ptr = static_cast<SIZE_T>(cpu);
    outGpu->ptr = gpu;
}
void SrvFreeThunk(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    static_cast<UiSrvPool*>(info->UserData)->free(static_cast<u64>(cpu.ptr));
}

} // namespace

// See UiBackend.hpp for the contract every method here implements.
class ImGuiD3D12Backend final : public IUiBackend {
public:
    // Safety net for a caller that skips the explicit device->uiShutdown()/shutdown() dance.
    ~ImGuiD3D12Backend() override { shutdown(); }

    bool init(void* hwnd, const UiBackendInitDesc& desc) override {
        if (active_) return true;
        if (!desc.device || !hwnd) return false;

        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.NumDescriptors = UiSrvPool::kCount;
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(desc.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap_)))) {
            AVER_ERROR("[RHI.D3D12.ImGui] UI SRV heap creation failed");
            return false;
        }
        pool_ = UiSrvPool{};
        pool_.cpuBase = srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr;
        pool_.gpuBase = srvHeap_->GetGPUDescriptorHandleForHeapStart().ptr;
        pool_.stride = desc.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Layout persists in editor-layout.ini in user data directory. View > Reset Layout rebuilds default.
        static std::string s_iniPath;
        if (s_iniPath.empty()) {
            const std::string dir = aver::userDataDir();
            if (!dir.empty()) s_iniPath = dir + "\\editor-layout.ini";
        }
        io.IniFilename = s_iniPath.empty() ? nullptr : s_iniPath.c_str();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd)) {
            AVER_ERROR("[RHI.D3D12.ImGui] ImGui_ImplWin32_Init failed");
            return false;
        }

        ImGui_ImplDX12_InitInfo info{};
        info.Device = desc.device;
        info.CommandQueue = desc.commandQueue;
        info.NumFramesInFlight = static_cast<int>(desc.frameCount);
        info.RTVFormat = desc.rtvFormat;
        info.SrvDescriptorHeap = srvHeap_.Get();
        info.UserData = &pool_;
        info.SrvDescriptorAllocFn = &SrvAllocThunk;
        info.SrvDescriptorFreeFn = &SrvFreeThunk;
        if (!ImGui_ImplDX12_Init(&info)) {
            AVER_ERROR("[RHI.D3D12.ImGui] ImGui_ImplDX12_Init failed");
            return false;
        }
        active_ = true;
        AVER_INFO("[RHI.D3D12.ImGui] ImGui UI initialised (docking)");
        return true;
    }

    void newFrame() override {
        if (!active_) return;
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void shutdown() override {
        if (!active_) return;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        pool_ = UiSrvPool{};
        active_ = false;
    }

    ID3D12DescriptorHeap* render(ID3D12GraphicsCommandList* cmdList) override {
        if (!active_) return nullptr;
        // Called once per ImGui frame (second UI render in frame interpolation reuses draw data).
        if (renderedFrame_ != ImGui::GetFrameCount()) {
            ImGui::Render();
            renderedFrame_ = ImGui::GetFrameCount();
        }
        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
        cmdList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
        return srvHeap_.Get();
    }

    bool wantsMouse() const override { return active_ && ImGui::GetIO().WantCaptureMouse; }
    bool wantsKeyboard() const override { return active_ && ImGui::GetIO().WantCaptureKeyboard; }

    bool wndProc(void* hwnd, u32 msg, u64 w, i64 l) override {
        return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                              static_cast<WPARAM>(w), static_cast<LPARAM>(l)) != 0;
    }

    bool allocTextureSrv(u64* outCpu, u64* outGpu) override {
        if (!active_) return false;
        return pool_.alloc(outCpu, outGpu);
    }
    void freeTextureSrv(u64 cpu) override {
        if (!active_) return;
        pool_.free(cpu);
    }

private:
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    UiSrvPool pool_;
    bool active_ = false;
    int renderedFrame_ = -1;   // Last ImGui::GetFrameCount() from ImGui::Render()
};

} // namespace aver::rhi::d3d12

namespace aver::rhi::d3d12::imgui_backend {
IUiBackend* create() { return new ImGuiD3D12Backend(); }
} // namespace aver::rhi::d3d12::imgui_backend
