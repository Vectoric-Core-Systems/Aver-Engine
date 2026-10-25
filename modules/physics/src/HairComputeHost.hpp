// HairComputeHost.hpp -- Jolt's ComputeSystem over the function table the host installs (physics_hair_abi.h, AverPhysComputeHostV1).
//
// SRC-LOCAL, like PhysicsInternal.hpp: it names JPH:: types. docs/plans/wave11/SOFTBODY_HAIR.md section 2.10 says why Jolt's own DX12 and
// Vulkan compute backends stay off: the renderer's frame cannot order itself after a private Jolt queue, two trackers over one resource corrupt
// state, and the RHI cannot hand out its native device. The adapter below gives D3D12 and Vulkan parity from one code path: it forwards
// every Jolt call, one to one, to the table, and the RHI side (Aver.Render.Hair) implements the table.
//
// The ComputeSystem class is in the GLOBAL namespace on purpose: Jolt's RTTI macros define a free GetRTTIOfType beside the class and
// stringize its name, and a class in the global namespace is how Jolt's own samples use them.
#pragma once
#include "aver/physics/physics_hair_abi.h"

#include <Jolt/Jolt.h>
#include <Jolt/Compute/ComputeBuffer.h>
#include <Jolt/Compute/ComputeQueue.h>
#include <Jolt/Compute/ComputeShader.h>
#include <Jolt/Compute/ComputeSystem.h>
#include <Jolt/Core/Reference.h>

#include <cstdint>
#include <unordered_set>

namespace aver::physics::detail {

// A buffer of the host. Owns one host handle and releases it.
class HostBuffer final : public JPH::ComputeBuffer {
public:
    HostBuffer(EType type, JPH::uint64 count, JPH::uint stride, const AverPhysComputeHostV1& host, AverPhysHostHandle handle)
        : JPH::ComputeBuffer(type, count, stride), host_(host), handle_(handle) {}
    ~HostBuffer() override;

    JPH::ComputeBufferResult CreateReadBackBuffer() const override;
    AverPhysHostHandle handle() const { return handle_; }

private:
    void* MapInternal(EMode mode) override;
    void UnmapInternal() override;

    AverPhysComputeHostV1 host_;      // a copy: the buffer may outlive a reinstall
    AverPhysHostHandle handle_ = 0;
};

class HostShader final : public JPH::ComputeShader {
public:
    HostShader(JPH::uint32 gx, JPH::uint32 gy, JPH::uint32 gz, const AverPhysComputeHostV1& host, AverPhysHostHandle handle)
        : JPH::ComputeShader(gx, gy, gz), host_(host), handle_(handle) {}
    ~HostShader() override;
    AverPhysHostHandle handle() const { return handle_; }

private:
    AverPhysComputeHostV1 host_;
    AverPhysHostHandle handle_ = 0;
};

// A command queue of the host. Holds a reference on every buffer and shader bound until Wait, as Jolt requires.
class HostQueue final : public JPH::ComputeQueue {
public:
    HostQueue(const AverPhysComputeHostV1& host, AverPhysHostHandle handle) : host_(host), handle_(handle) {}
    ~HostQueue() override;

    void SetShader(const JPH::ComputeShader* shader) override;
    void SetConstantBuffer(const char* name, const JPH::ComputeBuffer* buffer) override;
    void SetBuffer(const char* name, const JPH::ComputeBuffer* buffer) override;
    void SetRWBuffer(const char* name, JPH::ComputeBuffer* buffer, EBarrier barrier = EBarrier::Yes) override;
    void Dispatch(JPH::uint gx, JPH::uint gy = 1, JPH::uint gz = 1) override;
    void ScheduleReadback(JPH::ComputeBuffer* dst, const JPH::ComputeBuffer* src) override;
    void Execute() override;
    void Wait() override;

private:
    AverPhysComputeHostV1 host_;
    AverPhysHostHandle handle_ = 0;
    JPH::RefConst<JPH::ComputeShader> shader_;
    std::unordered_set<JPH::RefConst<JPH::ComputeBuffer>> used_;
};

// Forwards to another queue and counts the dispatches, so aver_phys_hair_info can say how many an update recorded on either backend.
class CountingQueue final : public JPH::ComputeQueue {
public:
    explicit CountingQueue(JPH::Ref<JPH::ComputeQueue> inner) : inner_(std::move(inner)) {}

    void SetShader(const JPH::ComputeShader* shader) override { inner_->SetShader(shader); }
    void SetConstantBuffer(const char* name, const JPH::ComputeBuffer* buffer) override { inner_->SetConstantBuffer(name, buffer); }
    void SetBuffer(const char* name, const JPH::ComputeBuffer* buffer) override { inner_->SetBuffer(name, buffer); }
    void SetRWBuffer(const char* name, JPH::ComputeBuffer* buffer, EBarrier barrier = EBarrier::Yes) override { inner_->SetRWBuffer(name, buffer, barrier); }
    void Dispatch(JPH::uint gx, JPH::uint gy = 1, JPH::uint gz = 1) override { ++dispatches_; inner_->Dispatch(gx, gy, gz); }
    void ScheduleReadback(JPH::ComputeBuffer* dst, const JPH::ComputeBuffer* src) override { inner_->ScheduleReadback(dst, src); }
    void Execute() override { inner_->Execute(); }
    void Wait() override { inner_->Wait(); }

    uint32_t dispatches() const { return dispatches_; }

private:
    JPH::Ref<JPH::ComputeQueue> inner_;
    uint32_t dispatches_ = 0;
};

} // namespace aver::physics::detail

// The compute system over the table. Needs a default constructor for Jolt's RTTI; call setHost before use.
class AverHostComputeSystem final : public JPH::ComputeSystem {
public:
    JPH_DECLARE_RTTI_VIRTUAL(JPH_EXPORT, AverHostComputeSystem)

    AverHostComputeSystem() = default;
    void setHost(const AverPhysComputeHostV1& host) { host_ = host; }

    JPH::ComputeShaderResult CreateComputeShader(const char* name, JPH::uint32 gx, JPH::uint32 gy = 1, JPH::uint32 gz = 1) override;
    JPH::ComputeBufferResult CreateComputeBuffer(JPH::ComputeBuffer::EType type, JPH::uint64 count, JPH::uint stride, const void* data = nullptr) override;
    JPH::ComputeQueueResult CreateComputeQueue() override;

private:
    AverPhysComputeHostV1 host_ = {};
};
