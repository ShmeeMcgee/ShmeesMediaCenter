// Shmees Media Center: C library calls the console's runtime leaves out that FFmpeg needs.

#ifdef __XSDK__
#include <time.h>

// newlib's clock() goes through times(), which xsdk doesn't have (xsdk:docs/GUIDE.md, "Porting"). FFmpeg calls clock()
// only to mix into a random seed (libavutil/random_seed.c); the time since start serves.
clock_t clock(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (clock_t)((double)ts.tv_sec * CLOCKS_PER_SEC + (double)ts.tv_nsec * CLOCKS_PER_SEC / 1e9);
}
#else
// Nothing: the host's C library has all of it. ISO C wants something in a file.
typedef int smc_compat_unused;
#endif
