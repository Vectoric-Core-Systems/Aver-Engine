// The second half of the fixture: a header that includes its sibling with an ANGLED include, which
// is the shape a vendored third-party tree uses internally and the one that was silently broken.
//
// Angled and quoted are NOT the same lookup. A quoted include searches the including file's own
// directory first, so it reaches a custom IDxcIncludeHandler with no help; an angled one searches
// ONLY the -I list, so with no -I argument DXC never asks the handler at all and reports
// `file not found with <angled> include; use "quotes" instead`. The include is written from the
// deployment ROOT here (not "./Inner.hlsli") because that is how a vendored header writes its own
// includes -- it cannot know where it was deployed to.
#ifndef AVER_INC_PROBE_OUTER_HLSLI
#define AVER_INC_PROBE_OUTER_HLSLI

#include <AverIncProbe/Inner.hlsli>

float averIncProbeOuter(float x) { return averIncProbeInner(x) + 1.0; }

#endif
