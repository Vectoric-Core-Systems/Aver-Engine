# libwebp (BSD-3-Clause) -- vendored, DECODER ONLY. Third-party: warnings silenced.
#
# The file list is upstream's libwebpdecoder (src/dec, the decoder half of src/dsp, the decoder half of
# src/utils), listed explicitly like third_party/meshoptimizer: the encoder sources sit in the same
# folders and must not be compiled in by a glob. Nothing is defined beyond what upstream's headers
# derive from the compiler (no HAVE_CONFIG_H): MSVC dispatches SSE2/SSE4.1/AVX2 at run time through
# VP8GetCPUInfo, arm64 uses NEON, and GCC/Clang on x86 use the SSE2 baseline (the SSE4.1 and AVX2 files
# compile to empty stubs there, as upstream's own non-flagged build does). WEBP_USE_THREAD is not set, so
# there is no pthread dependency and decoding is single threaded.
set(_webp_dec
  src/dec/alpha_dec.c src/dec/buffer_dec.c src/dec/frame_dec.c src/dec/idec_dec.c src/dec/io_dec.c
  src/dec/quant_dec.c src/dec/tree_dec.c src/dec/vp8_dec.c src/dec/vp8l_dec.c src/dec/webp_dec.c)
set(_webp_dsp
  src/dsp/alpha_processing.c src/dsp/cpu.c src/dsp/dec.c src/dsp/dec_clip_tables.c src/dsp/filters.c
  src/dsp/lossless.c src/dsp/rescaler.c src/dsp/upsampling.c src/dsp/yuv.c
  src/dsp/alpha_processing_sse2.c src/dsp/dec_sse2.c src/dsp/filters_sse2.c src/dsp/lossless_sse2.c
  src/dsp/rescaler_sse2.c src/dsp/upsampling_sse2.c src/dsp/yuv_sse2.c
  src/dsp/alpha_processing_sse41.c src/dsp/dec_sse41.c src/dsp/lossless_sse41.c
  src/dsp/upsampling_sse41.c src/dsp/yuv_sse41.c
  src/dsp/lossless_avx2.c
  src/dsp/alpha_processing_neon.c src/dsp/dec_neon.c src/dsp/filters_neon.c src/dsp/lossless_neon.c
  src/dsp/rescaler_neon.c src/dsp/upsampling_neon.c src/dsp/yuv_neon.c)
set(_webp_utils
  src/utils/bit_reader_utils.c src/utils/color_cache_utils.c src/utils/filters_utils.c
  src/utils/huffman_utils.c src/utils/palette.c src/utils/quant_levels_dec_utils.c
  src/utils/random_utils.c src/utils/rescaler_utils.c src/utils/thread_utils.c src/utils/utils.c)

add_library(webpdecoder STATIC ${_webp_dec} ${_webp_dsp} ${_webp_utils})
# The sources include each other as "src/dsp/dsp.h" and the public header is "src/webp/decode.h", so the
# include root is this folder. SYSTEM: nothing here is ours to warn about.
target_include_directories(webpdecoder SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(webpdecoder PRIVATE /w)
else()
  target_compile_options(webpdecoder PRIVATE -w)
endif()
set_target_properties(webpdecoder PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
