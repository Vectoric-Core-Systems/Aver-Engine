// A FIXTURE, not a shader anything renders with. It exists so ShaderIncludeResolveTest can compile
// an `#include` whose resolution is the thing under test, without depending on a real vendored tree
// (RTXDI's) that this module must not be coupled to.
//
// IT LIVES IN A SUBDIRECTORY ON PURPOSE. bin/shaders is one shared, flat, case-insensitively
// collapsed directory that every module deploys into; the whole point of the include handler being
// directory-aware is that "AverIncProbe/Inner.hlsli" resolves as a PATH rather than as the basename
// "Inner.hlsli", which is a name a third-party tree could plausibly also ship.
#ifndef AVER_INC_PROBE_INNER_HLSLI
#define AVER_INC_PROBE_INNER_HLSLI

float averIncProbeInner(float x) { return x * 2.0; }

#endif
