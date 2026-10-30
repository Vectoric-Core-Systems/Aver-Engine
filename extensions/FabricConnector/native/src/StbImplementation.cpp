// The stb_image_write implementation. Aver.Formats.Material's ImportCook re-encodes textures through it and, by design, leaves the
// implementation to the executable that links it (modules/formats/CMakeLists.txt); this library is one such, so it carries its own.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
