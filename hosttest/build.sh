#!/usr/bin/env bash
# Build the host harness. Mirrors external/libchdr_pico.cmake's source list +
# defines so libchdr compiles the same on host as on target. The libchdr
# alloc force-include lands on every TU (vs. per-target on Pico) — the host
# alloc-wrap file `hosttest/cd_chd_alloc_host.c` works around that by undef'ing
# the macros before pulling in <stdlib.h>.
#
# Drop -fsanitize=address for faster runs. PCE_HOST_DEBUG enables the VDC-solo
# hook in gfx.c (PCE_SOLO_VDC env var); ENABLE_CHD=1 pulls in cd_chd.c +
# libchdr so .chd discs can be opened.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

CHDR=external/libchdr
CFLAGS=(
  -O1 -g -fsanitize=address
  -DPCE_HOST_DEBUG -DENABLE_CHD=1 -DPICO_RP2350=1
  # Diagnostics (gated off by default; flip to =1 to re-enable):
  #   -DGFX_DEBUG_LOAD=1  per-frame sprite-row + sprite-0 collision proxy (PCE_TRACE_LOAD)
  #   -DCD_DEBUG_READ=1  per-frame data-sector read tally (PCE_TRACE_CD)
  #   -DCD_DEBUG_IO=1     per-frame $18xx read histogram (freeze io: dump)
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

HOST_SRC=(
  hosttest/host_main.c
  hosttest/stubs.c
  hosttest/fatfs_host.c
  hosttest/cd_chd_alloc_host.c
)
PCE_SRC=(
  pce-go/pce-go.c pce-go/pce.c pce-go/gfx.c pce-go/h6280.c pce-go/psg.c
  pce-go/cd.c pce-go/cd_chd.c
)
CHDR_SRC=(
  "$CHDR/src/libchdr_bitstream.c"
  "$CHDR/src/libchdr_cdrom.c"
  "$CHDR/src/libchdr_chd.c"
  "$CHDR/src/libchdr_flac.c"
  "$CHDR/src/libchdr_huffman.c"
  "$CHDR/src/libchdr_codec_cdfl.c"
  "$CHDR/src/libchdr_codec_cdlz.c"
  "$CHDR/src/libchdr_codec_cdzl.c"
  "$CHDR/src/libchdr_codec_cdzs.c"
  "$CHDR/src/libchdr_codec_flac.c"
  "$CHDR/src/libchdr_codec_huff.c"
  "$CHDR/src/libchdr_codec_lzma.c"
  "$CHDR/src/libchdr_codec_zlib.c"
  "$CHDR/src/libchdr_codec_zstd.c"
  "$CHDR/deps/miniz-3.1.1/miniz.c"
  "$CHDR/deps/lzma-25.01/src/LzmaDec.c"
  "$CHDR/deps/zstd-1.5.7/zstddeclib.c"
)

exec gcc "${CFLAGS[@]}" -o hosttest/pce_host \
  "${HOST_SRC[@]}" "${PCE_SRC[@]}" "${CHDR_SRC[@]}" -lm
