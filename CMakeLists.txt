# Shmees Media Center: an Xbox 360 homebrew media center, built with xsdk (MIT). Build with xsdk's toolchain file:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=<prefix>
#   cmake --build build
# The .xex lands in build/ShmeesMediaCenter.xex.
cmake_minimum_required(VERSION 3.25)
project(ShmeesMediaCenter C)

set(SMC_VERSION "0.5.0-vector" CACHE STRING "The version the app shows")

add_executable(ShmeesMediaCenter
	src/main.c
	src/app.c
	src/gfx.c
	src/input.c
	src/library.c
	src/settings.c
	src/art.c
	src/probe.c
	src/player.c
	src/threads.c
	src/compat.c
	src/image_decode.c)
set_target_properties(ShmeesMediaCenter PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_include_directories(ShmeesMediaCenter PRIVATE src)
target_compile_definitions(ShmeesMediaCenter PRIVATE SMC_VERSION="${SMC_VERSION}")
# snprintf cutting a long title short for the screen is what the UI wants, so truncation warnings are off.
target_compile_options(ShmeesMediaCenter PRIVATE -O2 -Wall -Wextra -Werror -Wno-format-truncation)
# stb_image is third-party code: its warnings aren't ours to fix.
set_source_files_properties(src/image_decode.c PROPERTIES COMPILE_OPTIONS "-Wno-error")
# FFmpeg 9.0.2 (LGPL 2.1 or later), built for the console with only the decoders and demuxers the app needs:
# tools/build_ffmpeg.sh.
set(FFMPEG_PREFIX "$ENV{HOME}/ffmpeg-xsdk-vmx" CACHE PATH "Where tools/build_ffmpeg.sh installed FFmpeg")
target_include_directories(ShmeesMediaCenter SYSTEM PRIVATE "${FFMPEG_PREFIX}/include")
target_link_libraries(ShmeesMediaCenter PRIVATE
	"${FFMPEG_PREFIX}/lib/libavformat.a"
	"${FFMPEG_PREFIX}/lib/libavcodec.a"
	"${FFMPEG_PREFIX}/lib/libswresample.a"
	"${FFMPEG_PREFIX}/lib/libavutil.a"
	m)
# A rebuilt FFmpeg relinks the app.
set_property(TARGET ShmeesMediaCenter APPEND PROPERTY LINK_DEPENDS
	"${FFMPEG_PREFIX}/lib/libavformat.a" "${FFMPEG_PREFIX}/lib/libavcodec.a"
	"${FFMPEG_PREFIX}/lib/libswresample.a" "${FFMPEG_PREFIX}/lib/libavutil.a")
# Threads FFmpeg starts are placed on the decoding cores as they're made (src/threads.c).
target_link_options(ShmeesMediaCenter PRIVATE -Wl,--wrap=pthread_create)

# Writes ShmeesMediaCenter_shaders.h, with ui_vert and ui_frag.
xsdk_add_shaders(ShmeesMediaCenter shaders/ui.vert shaders/ui.frag shaders/video.frag)
# A title ID of its own keeps its saves apart from other titles': "SHMC".
xsdk_add_xex(ShmeesMediaCenter TITLE_ID 0x53484D43 NAME "Shmees Media Center" VERSION 0.5.0.0)
