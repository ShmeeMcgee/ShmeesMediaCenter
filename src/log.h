// Shmees Media Center: logging. xsdk writes the log to <title>.log beside the .xex (game:\ShmeesMediaCenter.log), from
// any thread (xsdk:docs/GUIDE.md, "Files and paths").
#ifndef SMC_LOG_H
#define SMC_LOG_H

#include "xsdk/log.h"

#define smc_log(...) xsdk_log_write(__VA_ARGS__)

#endif
