// The framework relay's provider (framework_particles_abi.h, aver_fw_set_particles_provider), with the ParticleSystem as
// `user`. Declared here so Aver.Framework never links this module; the composition root hands it across:
//   aver_fw_set_particles_provider(&particles::particleProvider, &particles::particleSystem());
// The op numbers are the AVER_FW_PARTICLES_OP_* of framework_particles_abi.h, mirrored in ParticleProvider.cpp
// (AbiEnumTest pins the header against the C# constants).
#pragma once
#include "aver/core/Types.hpp"

namespace aver::particles {

i32 particleProvider(i32 op, i32 entity, i64 h0, i64 h1, const f32* in, f32* outF, i32* outI, void* user);

} // namespace aver::particles
