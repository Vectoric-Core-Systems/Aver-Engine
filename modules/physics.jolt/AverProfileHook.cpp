// AverProfileHook.cpp -- ours, not vendored. Compiled INTO the Jolt target (modules/physics.jolt/CMakeLists.txt adds it when
// AVER_PHYSICS_PROFILER is on).
//
// A statically linked Jolt built with JPH_EXTERNAL_PROFILE leaves JPH::ExternalProfileMeasurement's constructor and
// destructor for the user to supply (Jolt/Core/Profiler.h). They live here, and not in Aver.Physics, because PhysicsTest
// links Jolt directly: defined only in the DLL they would be unresolved symbols in that exe.
//
// The first 8 bytes of the measurement's 64 hold the sink that was current when the scope began, so a scope that began while
// profiling was off never calls end, and one that began while it was on always does, even if profiling is switched off in
// between. A null sink costs a relaxed load and a branch.
#include <Jolt/Jolt.h>

#ifdef JPH_EXTERNAL_PROFILE

#include <Jolt/Core/Profiler.h>

#include "../physics/src/PhysicsProfile.hpp"

#include <cstring>

namespace aver::physics::profile {
std::atomic<const Sink*> g_sink{nullptr};
}

JPH_NAMESPACE_BEGIN

ExternalProfileMeasurement::ExternalProfileMeasurement(const char *inName, uint32 inColor)
{
	const aver::physics::profile::Sink *sink = aver::physics::profile::g_sink.load(std::memory_order_relaxed);
	std::memcpy(mUserData, &sink, sizeof(sink));
	if (sink != nullptr)
		sink->begin(inName, inColor, mUserData + sizeof(sink));
}

ExternalProfileMeasurement::~ExternalProfileMeasurement()
{
	const aver::physics::profile::Sink *sink;
	std::memcpy(&sink, mUserData, sizeof(sink));
	if (sink != nullptr)
		sink->end(mUserData + sizeof(sink));
}

JPH_NAMESPACE_END

#endif // JPH_EXTERNAL_PROFILE
