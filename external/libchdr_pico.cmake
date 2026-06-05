# Minimal build of libchdr for the RP2350 / Pico SDK target.
#
# We skip libchdr's own CMakeLists.txt because it (a) defaults to also building
# a SHARED lib + installs, and (b) adds an unconditional add_subdirectory(tests)
# that pulls in host-only tools like chdr-benchmark which fail to link against
# the Pico SDK. Instead we declare a single static target `libchdr_pico` that
# bundles the libchdr sources plus miniz / LZMA SDK / zstd amalgamated decoder.
#
# Source set is sized for PC Engine CD CHDs (zlib + CDLZ + Huffman + zstd codecs;
# FLAC compiled in for completeness — rare in PCE CHDs but cheap to leave).
#
# Tunables:
#   CHDR_WANT_RAW_DATA_SECTOR=1  expose full 2352-byte raw sectors (we need this)
#   CHDR_WANT_SUBCODE=0          drop subchannel handling (saves ~3KB code)
#   CHDR_VERIFY_BLOCK_CRC=0      skip per-block CRC verify (CPU savings; we trust SD)

set(CHDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/libchdr)

add_library(libchdr_pico STATIC
    # core
    ${CHDR_DIR}/src/libchdr_bitstream.c
    ${CHDR_DIR}/src/libchdr_cdrom.c
    ${CHDR_DIR}/src/libchdr_chd.c
    ${CHDR_DIR}/src/libchdr_flac.c
    ${CHDR_DIR}/src/libchdr_huffman.c
    # codecs
    ${CHDR_DIR}/src/libchdr_codec_cdfl.c
    ${CHDR_DIR}/src/libchdr_codec_cdlz.c
    ${CHDR_DIR}/src/libchdr_codec_cdzl.c
    ${CHDR_DIR}/src/libchdr_codec_cdzs.c
    ${CHDR_DIR}/src/libchdr_codec_flac.c
    ${CHDR_DIR}/src/libchdr_codec_huff.c
    ${CHDR_DIR}/src/libchdr_codec_lzma.c
    ${CHDR_DIR}/src/libchdr_codec_zlib.c
    ${CHDR_DIR}/src/libchdr_codec_zstd.c
    # deps (amalgamated single-file decoders)
    ${CHDR_DIR}/deps/miniz-3.1.1/miniz.c
    ${CHDR_DIR}/deps/lzma-25.01/src/LzmaDec.c
    ${CHDR_DIR}/deps/zstd-1.5.7/zstddeclib.c
)

target_include_directories(libchdr_pico PUBLIC
    ${CHDR_DIR}/include
)
target_include_directories(libchdr_pico PRIVATE
    ${CHDR_DIR}/src
    ${CHDR_DIR}/deps/miniz-3.1.1
    ${CHDR_DIR}/deps/lzma-25.01/include
    ${CHDR_DIR}/deps/zstd-1.5.7
)

target_compile_definitions(libchdr_pico PRIVATE
    WANT_RAW_DATA_SECTOR=1
    WANT_SUBCODE=0
    VERIFY_BLOCK_CRC=0
    # miniz: strip everything we don't use (we only call raw inflate via the
    # tinfl_* / mz_inflate path). Drops zip read+write APIs, stdio glue, and
    # the time/stat helpers that pull in utime/fopen/_unlink/etc. from newlib.
    MINIZ_NO_STDIO=1
    MINIZ_NO_TIME=1
    MINIZ_NO_ARCHIVE_APIS=1
    MINIZ_NO_ARCHIVE_WRITING_APIS=1
)

# Silence warnings from third-party code (these are not our sources to fix).
target_compile_options(libchdr_pico PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-pointer-sign
    -Wno-strict-aliasing
)
# Reroute libchdr's stdlib malloc/free/calloc/realloc to PSRAM via linker
# --wrap: cleaner than macro-redefining stdlib names (which breaks
# declarations like `extern void *calloc(size_t, size_t)`). The wrap is on
# the final executable below (target_link_options on picopcePlus), not on
# this static lib — but the wrappers must exist for it to work.
