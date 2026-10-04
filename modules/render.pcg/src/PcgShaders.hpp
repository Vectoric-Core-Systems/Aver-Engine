// The density-volume compute shader.
//
// MIRRORED FUNCTION FOR FUNCTION by PcgVolume.cpp, and by Aver.Pcg's sampleDensity in F#. Three
// implementations of one function is two too many to keep in step by hope, which is why
// --pcg-volume-test compares this against the C++ one voxel by voxel, every voxel, every run.
// The atmosphere model in this repo is kept honest the same way.
//
// THEY AGREE TO ONE ULP, NOT BIT-EXACTLY, and that was measured rather than assumed. On a 32^3
// two-layer field, 8234 of 32768 voxels match bit for bit and the rest differ by at most
// 1.79e-07 -- 2^-23. They still differ with coverageBias at 1.0, which rules out pow() and leaves
// floating-point CONTRACTION: DXC fuses multiply-add in the fBm accumulation where MSVC under
// /fp:precise does not. The integer hash below agrees exactly; only the float tail moves.
//
// EVERY INTEGER OPERATION HERE IS 32-BIT UNSIGNED AND WRAPS. That is true in HLSL, in C++ and in F#
// for uint32, and it is why the HASH half of this agrees exactly across all three -- and why the
// hash is splitmix32 written out rather than anything from a library.
#pragma once

namespace aver::pcg {


} // namespace aver::pcg
