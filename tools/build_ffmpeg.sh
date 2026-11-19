#!/bin/bash
# Builds FFmpeg's libraries for the console with xsdk's toolchain: H.264, HEVC, MPEG-4 Part 2 and MPEG-2 video; AAC,
# AC-3, E-AC-3 and MP3/MP2 audio; MP4/MOV, Matroska, AVI, MPEG-TS and MP3 files. LGPL 2.1 or later: nothing GPL is
# enabled. Usage: build_ffmpeg.sh <ffmpeg source> <xsdk> <toolchain prefix> <install prefix>
set -euo pipefail
SRC=$1 XSDK=$2 TOOLCHAIN=$3 PREFIX=$4
export PATH="$TOOLCHAIN/bin:$PATH"
# One change to FFmpeg (tools/ffmpeg-xsdk.patch): the dcbzl cache-line check only with the PowerPC code switched on.
patch -d "$SRC" -p1 -N < "$(dirname "$0")/ffmpeg-xsdk.patch" || true
"$SRC/configure" --prefix="$PREFIX" --enable-cross-compile --cross-prefix=powerpc-xsdk-elf- --arch=ppc --target-os=none \
	--disable-runtime-cpudetect --enable-altivec --disable-vsx --disable-power8 \
	--disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
	--disable-swscale --disable-debug --enable-pthreads --disable-everything \
	--enable-decoder=h264,hevc,mpeg4,mpeg2video,mpeg1video,aac,aac_latm,ac3,eac3,mp3float,mp2float \
	--enable-demuxer=mov,matroska,avi,mpegts,mp3 \
	--enable-parser=h264,hevc,aac,aac_latm,ac3,mpeg4video,mpegaudio,mpegvideo \
	--enable-protocol=file \
	--extra-cflags="-mcpu=cell -fno-pic -I$XSDK/include -O3" \
	--extra-ldflags="-static -nostdlib -T $XSDK/src/xsdk.ld" \
	--extra-libs="-Wl,--start-group -lstdc++ $XSDK/build/libxsdk.a -lc -lm -lgcc -Wl,--end-group"
make -j"$(nproc)"
make install
