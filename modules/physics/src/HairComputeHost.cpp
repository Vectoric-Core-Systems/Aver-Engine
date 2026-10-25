// HairComputeHost.cpp -- see HairComputeHost.hpp. Every Jolt compute call is forwarded to the host table unchanged; the only state here
// is the references Jolt requires a queue to hold until the work is known to be finished.
#include "HairComputeHost.hpp"

using aver::physics::detail::HostBuffer;
using aver::physics::detail::HostQueue;
using aver::physics::detail::HostShader;

JPH_IMPLEMENT_RTTI_VIRTUAL(AverHostComputeSystem)
{
    JPH_ADD_BASE_CLASS(AverHostComputeSystem, JPH::ComputeSystem)
}

namespace aver::physics::detail {

// ---- HostBuffer ----

HostBuffer::~HostBuffer() {
    if (handle_ != 0 && host_.release) host_.release(host_.user, handle_);
}

JPH::ComputeBufferResult HostBuffer::CreateReadBackBuffer() const {
    JPH::ComputeBufferResult result;
    if (!host_.create_buffer) { result.SetError("hair compute host has no create_buffer"); return result; }
    const AverPhysHostHandle h = host_.create_buffer(host_.user, AVER_PHYS_CB_READBACK, mSize, mStride, nullptr);
    if (h == 0) { result.SetError("hair compute host refused a readback buffer"); return result; }
    result.Set(new HostBuffer(EType::ReadbackBuffer, mSize, mStride, host_, h));
    return result;
}

void* HostBuffer::MapInternal(EMode mode) {
    return host_.map ? host_.map(host_.user, handle_, mode == EMode::Write ? 1 : 0) : nullptr;
}

void HostBuffer::UnmapInternal() {
    if (host_.unmap) host_.unmap(host_.user, handle_);
}

// ---- HostShader ----

HostShader::~HostShader() {
    if (handle_ != 0 && host_.release) host_.release(host_.user, handle_);
}

// ---- HostQueue ----

HostQueue::~HostQueue() {
    if (handle_ != 0 && host_.release) host_.release(host_.user, handle_);
}

void HostQueue::SetShader(const JPH::ComputeShader* shader) {
    shader_ = shader;
    host_.set_shader(host_.user, handle_, static_cast<const HostShader*>(shader)->handle());
}

void HostQueue::SetConstantBuffer(const char* name, const JPH::ComputeBuffer* buffer) {
    if (buffer == nullptr) return;
    host_.bind(host_.user, handle_, name, static_cast<const HostBuffer*>(buffer)->handle(), AVER_PHYS_BIND_CONSTANT, 0);
    used_.insert(JPH::RefConst<JPH::ComputeBuffer>(buffer));
}

void HostQueue::SetBuffer(const char* name, const JPH::ComputeBuffer* buffer) {
    if (buffer == nullptr) return;
    host_.bind(host_.user, handle_, name, static_cast<const HostBuffer*>(buffer)->handle(), AVER_PHYS_BIND_READ, 0);
    used_.insert(JPH::RefConst<JPH::ComputeBuffer>(buffer));
}

void HostQueue::SetRWBuffer(const char* name, JPH::ComputeBuffer* buffer, EBarrier barrier) {
    if (buffer == nullptr) return;
    host_.bind(host_.user, handle_, name, static_cast<const HostBuffer*>(buffer)->handle(), AVER_PHYS_BIND_RW,
               barrier == EBarrier::Yes ? 1 : 0);
    used_.insert(JPH::RefConst<JPH::ComputeBuffer>(buffer));
}

void HostQueue::Dispatch(JPH::uint gx, JPH::uint gy, JPH::uint gz) {
    host_.dispatch(host_.user, handle_, gx, gy, gz);
}

void HostQueue::ScheduleReadback(JPH::ComputeBuffer* dst, const JPH::ComputeBuffer* src) {
    if (dst == nullptr || src == nullptr) return;
    host_.readback(host_.user, handle_, static_cast<const HostBuffer*>(dst)->handle(), static_cast<const HostBuffer*>(src)->handle());
    used_.insert(JPH::RefConst<JPH::ComputeBuffer>(dst));
    used_.insert(JPH::RefConst<JPH::ComputeBuffer>(src));
}

void HostQueue::Execute() {
    host_.execute(host_.user, handle_);
}

void HostQueue::Wait() {
    host_.wait(host_.user, handle_);
    used_.clear();
    shader_ = nullptr;
}

} // namespace aver::physics::detail

// ---- AverHostComputeSystem ----

JPH::ComputeShaderResult AverHostComputeSystem::CreateComputeShader(const char* name, JPH::uint32 gx, JPH::uint32 gy, JPH::uint32 gz) {
    JPH::ComputeShaderResult result;
    if (!host_.create_shader) { result.SetError("hair compute host has no create_shader"); return result; }
    const AverPhysHostHandle h = host_.create_shader(host_.user, name, gx, gy, gz);
    if (h == 0) { result.SetError("hair compute host does not know this shader"); return result; }
    result.Set(new HostShader(gx, gy, gz, host_, h));
    return result;
}

JPH::ComputeBufferResult AverHostComputeSystem::CreateComputeBuffer(JPH::ComputeBuffer::EType type, JPH::uint64 count, JPH::uint stride, const void* data) {
    JPH::ComputeBufferResult result;
    if (!host_.create_buffer) { result.SetError("hair compute host has no create_buffer"); return result; }
    int32_t hostType = AVER_PHYS_CB_RW;
    switch (type) {
        case JPH::ComputeBuffer::EType::UploadBuffer:   hostType = AVER_PHYS_CB_UPLOAD; break;
        case JPH::ComputeBuffer::EType::ReadbackBuffer: hostType = AVER_PHYS_CB_READBACK; break;
        case JPH::ComputeBuffer::EType::ConstantBuffer: hostType = AVER_PHYS_CB_CONSTANT; break;
        case JPH::ComputeBuffer::EType::Buffer:         hostType = AVER_PHYS_CB_BUFFER; break;
        case JPH::ComputeBuffer::EType::RWBuffer:       hostType = AVER_PHYS_CB_RW; break;
    }
    const AverPhysHostHandle h = host_.create_buffer(host_.user, hostType, count, stride, data);
    if (h == 0) { result.SetError("hair compute host refused a buffer"); return result; }
    result.Set(new HostBuffer(type, count, stride, host_, h));
    return result;
}

JPH::ComputeQueueResult AverHostComputeSystem::CreateComputeQueue() {
    JPH::ComputeQueueResult result;
    if (!host_.create_queue) { result.SetError("hair compute host has no create_queue"); return result; }
    const AverPhysHostHandle h = host_.create_queue(host_.user);
    if (h == 0) { result.SetError("hair compute host refused a queue"); return result; }
    result.Set(new HostQueue(host_, h));
    return result;
}
