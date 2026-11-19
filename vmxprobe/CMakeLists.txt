# Shmees VMX Probe: finds which of the vector instructions FFmpeg's PowerPC code uses the Xbox 360 can run.
cmake_minimum_required(VERSION 3.25)
project(ShmeesVMXProbe C)
add_executable(ShmeesVMXProbe main.c)
set_target_properties(ShmeesVMXProbe PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(ShmeesVMXProbe PRIVATE -O1 -Wall -Wextra -Werror -Wno-format-truncation)
xsdk_add_xex(ShmeesVMXProbe TITLE_ID 0x53484D56 NAME "Shmees VMX Probe" VERSION 1.0.0.0)
