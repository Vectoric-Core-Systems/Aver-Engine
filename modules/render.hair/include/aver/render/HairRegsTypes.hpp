#pragma once
// The row types of the generated register table (src/HairRegs.inc, scripts/gen-hair-shader-regs.py): for each of Jolt's fifteen hair kernels and
// Aver's wind kernel, the wrapper file that compiles it and the registers of its bindings. RhiComputeHost.cpp includes the table to build
// pipeline layouts and to resolve Jolt's binding NAMES (gPositions, ...) to slots; tests/render.hair/HairRegsTest includes it to check it
// against Jolt's headers.
//
// A binding's kind is 0 for a read-only slot (tN, StructuredBuffer) and 1 for a read-write slot (uN, RWStructuredBuffer); the slot is N. Constants
// are not in the table: gContext and gWind are b3, gIterationContext is b4 (kHairConstantSlotContext / kHairConstantSlotIteration).
namespace aver::render {

struct HairKernelBinding {
    const char* name;
    unsigned char kind;    // 0 read-only (t), 1 read-write (u)
    unsigned char slot;
};

struct HairKernel {
    const char* name;             // Jolt's shader name ("HairIntegrate"), or "AverHairWind"
    const char* file;             // the wrapper in <exe>/shaders
    unsigned srvCount;
    unsigned uavCount;
    unsigned bindingCount;
    const HairKernelBinding* bindings;
};

// The constant slots (registers b3 and b4). Slot 3 holds gContext (Jolt) or gWind (Aver), slot 4 gIterationContext.
inline constexpr unsigned kHairConstantSlotContext = 3;
inline constexpr unsigned kHairConstantSlotIteration = 4;

} // namespace aver::render
