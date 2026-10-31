// The NanoSVG implementation, compiled in exactly this translation unit with warnings off (see
// modules/platform/CMakeLists.txt). Nothing of ours belongs here; the wrapper is ImageSvg.cpp.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
