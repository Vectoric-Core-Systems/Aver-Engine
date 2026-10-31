#ifndef AVER_FRAMEWORK_RTEX_ABI_H
#define AVER_FRAMEWORK_RTEX_ABI_H

/* Runtime textures (Aver.Framework, E8): a texture a script builds from pixels at run time and updates whenever it likes. Same shape as
 * framework_rtmesh_abi.h and framework_subview_abi.h: the script PUBLISHES pixels, the host READS each frame and uploads what changed. Nothing exists, and
 * nothing is uploaded, until a script or a converted level makes one.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/uint64_t/uint8_t/const char* only; 1 on success and 0 on refusal. All state lives in this
 * DLL, single game thread.
 *
 * A texture is named by a KEY (UTF-8, 1..120 bytes). A material uses it by putting AVER_FW_RTEX_MATERIAL_PREFIX + key ("@rt:rtex.<key>") in a texture slot
 * (aver_pbr_set_texture_path / the .ocmat TEX record); the next frame after the pixels change, every material naming it is rewritten. The UI reads the GPU
 * handle (aver_fw_rtex_gpu_handle) and puts it in a draw command's texture. A shader reads it through the material slot it sits in.
 *
 * PIXELS are tightly packed rows, top row first, no padding. Formats (AVER_FW_RTEX_FORMAT_*): RGBA8 is 4 bytes a pixel read as linear data, RGBA8_SRGB the
 * same bytes decoded as sRGB (colour images), R8 one byte, RGBA16F four IEEE halves (8 bytes a pixel). A full mip chain is built by the host for the 8-bit
 * formats when the texture asks for one (mips = 1); RGBA16F has level 0 only.
 *
 * Cost guard: at most AVER_FW_RTEX_DEFAULT_MAX_TEXTURES live, AVER_FW_RTEX_MAX_DIMENSION a side, AVER_FW_RTEX_MAX_BYTES per texture and
 * AVER_FW_RTEX_MAX_TOTAL_BYTES together (level 0, CPU copy), unless the host says otherwise (aver_fw_rtex_configure). */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_RTEX_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_rtex_abi_version(void);

#define AVER_FW_RTEX_MATERIAL_PREFIX "@rt:rtex."
#define AVER_FW_RTEX_DEFAULT_MAX_TEXTURES 256
#define AVER_FW_RTEX_MAX_DIMENSION 8192
#define AVER_FW_RTEX_MAX_BYTES (256 * 1024 * 1024)
#define AVER_FW_RTEX_MAX_TOTAL_BYTES (1024 * 1024 * 1024)

/* Pixel formats */
#define AVER_FW_RTEX_FORMAT_RGBA8      0
#define AVER_FW_RTEX_FORMAT_RGBA8_SRGB 1
#define AVER_FW_RTEX_FORMAT_R8         2
#define AVER_FW_RTEX_FORMAT_RGBA16F    3
#define AVER_FW_RTEX_FORMAT_COUNT      4

/* ---- script side ------------------------------------------------------------------------------------------------ */

/* Makes the texture named `key` (width x height, zero bytes everywhere: transparent black), or returns the id it already has (its size and format are kept).
 * mips = 1 asks for a full mip chain at upload (8-bit formats). 0 when refused: a bad key, a size of 0 or past AVER_FW_RTEX_MAX_DIMENSION, an unknown format,
 * a byte budget overrun, or the texture budget full. */
AVER_FW_ABI int32_t aver_fw_rtex_create(const char* key, int32_t width, int32_t height, int32_t format, int32_t mips);
AVER_FW_ABI int32_t aver_fw_rtex_destroy(int32_t id);
AVER_FW_ABI int32_t aver_fw_rtex_find(const char* key);
/* Replaces the whole image: width x height pixels of `format` in `data` (`bytes` must be exactly width * height * bytes-per-pixel). Changes the texture's size
 * and format to match. Marks it changed; the GPU texture is replaced next frame. 0 for a bad id, a size or format outside the limits, a wrong byte count, a
 * null pointer, or a budget overrun. */
AVER_FW_ABI int32_t aver_fw_rtex_set_pixels(int32_t id, int32_t width, int32_t height, int32_t format, const uint8_t* data, int32_t bytes);
/* Overwrites a rectangle (x, y, w, h) of the current image with `bytes` of the texture's own format (w * h * bytes-per-pixel). 0 when the rectangle leaves
 * the image or the count is wrong. */
AVER_FW_ABI int32_t aver_fw_rtex_set_region(int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* data, int32_t bytes);
/* Copies the current pixels into out (cap bytes); returns the bytes the image holds (copy only when it fits), 0 for an unknown id. */
AVER_FW_ABI int32_t aver_fw_rtex_get_pixels(int32_t id, uint8_t* out, int32_t cap);
/* Six ints: width, height, format, mips (0/1), generation (counts changes), bytes. */
AVER_FW_ABI int32_t aver_fw_rtex_info(int32_t id, int32_t* out6);
/* The key, UTF-8, NUL-terminated into out (cap bytes including the NUL); returns the key's length, 0 for an unknown id. */
AVER_FW_ABI int32_t aver_fw_rtex_key(int32_t id, char* out, int32_t cap);
/* The texture-slot path a material names it by: "@rt:rtex.<key>", same output convention as aver_fw_rtex_key. */
AVER_FW_ABI int32_t aver_fw_rtex_material_path(int32_t id, char* out, int32_t cap);
/* The GPU texture handle the host uploaded for this texture (what a UI draw command carries), 0 until the first upload and for an unknown id. It changes when
 * the pixels do: read it again after each frame that updated the texture. */
AVER_FW_ABI uint64_t aver_fw_rtex_gpu_handle(int32_t id);

/* ---- image files ------------------------------------------------------------------------------------------------ */

/* Decodes an image file (PNG, JPEG, TGA, BMP, GIF, PSD, WebP, SVG at its own size, OpenEXR clamped to 0..1; the engine's own decoder, so the same set a texture
 * import reads) to 8-bit RGBA. First call with out = NULL to learn the size: returns 1 and writes width and height into out2; the second passes a buffer of
 * width * height * 4 bytes (cap) and gets the pixels. The path is used as given (absolute, or relative to the working directory: the framework does not know
 * the project's content root, so a script resolves a content path itself). A file the decoder cannot read, one past its pixel limit, or a buffer that is too
 * small, returns 0. */
AVER_FW_ABI int32_t aver_fw_image_decode(const char* path, int32_t* out2, uint8_t* out, int32_t cap);

/* ---- host side -------------------------------------------------------------------------------------------------- */

/* Sets the cost guard. maxTextures < 1 keeps the default. Called once from the host. */
AVER_FW_ABI void aver_fw_rtex_configure(int32_t maxTextures);
/* Textures changed since the host last took them: ids into outIds (at most max); the returned textures are no longer dirty. */
AVER_FW_ABI int32_t aver_fw_rtex_take_dirty(int32_t* outIds, int32_t max);
/* Textures destroyed since the host last asked: their ids, so the host can retire the GPU texture it made for each. */
AVER_FW_ABI int32_t aver_fw_rtex_take_removed(int32_t* outIds, int32_t max);
/* The pixels, pointers valid until the next call that changes any texture. Answers 0 for an unknown id. */
typedef struct AverFwRtexView {
    int32_t width;
    int32_t height;
    int32_t format;
    int32_t mips;           /* 1 = build a full chain */
    int32_t bytes;
    const uint8_t* pixels;  /* bytes long */
    const char* key;        /* UTF-8, NUL-terminated */
} AverFwRtexView;
AVER_FW_ABI int32_t aver_fw_rtex_view(int32_t id, AverFwRtexView* out);
/* The host records the GPU texture it made (0 = none). */
AVER_FW_ABI void aver_fw_rtex_host_set_gpu_handle(int32_t id, uint64_t handle);
AVER_FW_ABI int32_t aver_fw_rtex_count(void);
/* Session boundary: forgets every texture and queues their removals. */
AVER_FW_ABI void aver_fw_rtex_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_RTEX_ABI_H */
