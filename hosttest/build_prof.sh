#!/usr/bin/env bash
# Profiling build of the host harness (gprof). Mirrors build.sh's source list
# but swaps ASan/-O1 for -O2 -pg so gprof can rank the hot render functions.
# Output: hosttest/pce_host_prof (gitignored). Not part of the normal build.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root
CHDR=external/libchdr
CFLAGS=(
  -O2 -g -pg
  -DPCE_HOST_DEBUG -DENABLE_CHD=1 -DPICO_RP2350=1
  -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0
  -DMINIZ_NO_STDIO=1 -DMINIZ_NO_TIME=1
  -DMINIZ_NO_ARCHIVE_APIS=1 -DMINIZ_NO_ARCHIVE_WRITING_APIS=1
  -I hosttest/shim -I pce-go
  -I "$CHDR/include" -I "$CHDR/src"
  -I "$CHDR/deps/miniz-3.1.1"
  -I "$CHDR/deps/lzma-25.01/include"
  -I "$CHDR/deps/zstd-1.5.7"
  -include external/libchdr_alloc.h
  -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function
  -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-sign-compare
  -Wno-pointer-sign -Wno-strict-aliasing
)
HOST_SRC=( hosttest/host_main.c hosttest/stubs.c hosttest/fatfs_host.c hosttest/cd_chd_alloc_host.c )
PCE_SRC=( pce-go/pce-go.c pce-go/pce.c pce-go/gfx.c pce-go/h6280.c pce-go/psg.c pce-go/cd.c pce-go/cd_chd.c )
CHDR_SRC=(
  "$CHDR/src/libchdr_bitstream.c" "$CHDR/src/libchdr_cdrom.c" "$CHDR/src/libchdr_chd.c"
  "$CHDR/src/libchdr_flac.c" "$CHDR/src/libchdr_huffman.c" "$CHDR/src/libchdr_codec_cdfl.c"
  "$CHDR/src/libchdr_codec_cdlz.c" "$CHDR/src/libchdr_codec_cdzl.c" "$CHDR/src/libchdr_codec_cdzs.c"
  "$CHDR/src/libchdr_codec_flac.c" "$CHDR/src/libchdr_codec_huff.c" "$CHDR/src/libchdr_codec_lzma.c"
  "$CHDR/src/libchdr_codec_zlib.c" "$CHDR/src/libchdr_codec_zstd.c"
  "$CHDR/deps/miniz-3.1.1/miniz.c" "$CHDR/deps/lzma-25.01/src/LzmaDec.c" "$CHDR/deps/zstd-1.5.7/zstddeclib.c"
)
gcc "${CFLAGS[@]}" -o hosttest/pce_host_prof "${HOST_SRC[@]}" "${PCE_SRC[@]}" "${CHDR_SRC[@]}" -lm
echo "built hosttest/pce_host_prof"
