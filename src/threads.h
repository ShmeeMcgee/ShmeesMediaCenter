// Shmees Media Center: which of the console's six hardware threads each thread runs on. xsdk starts every thread on
// its creator's hardware thread (xsdk:docs/GUIDE.md, "Threads and where they run"), so without this FFmpeg's decoding
// threads would all share the UI's.
//
//   0: the UI, raised, and one decoding thread (core 0)
//   1: file reading, audio, posters, the library scan, raised, and one decoding thread (core 0)
//   2-5: FFmpeg's other video decoding threads (cores 1 and 2)
#ifndef SMC_THREADS_H
#define SMC_THREADS_H

#include <pthread.h>
#include <stdint.h>

enum {
	HW_UI = 0,
	HW_IO = 1,
};

// Moves a thread to a hardware thread.
void threads_place(pthread_t thread, uint32_t hardware_thread);
// Threads created after this, from any thread, go round the given hardware threads in turn, until threads_place_none.
void threads_place_round(const uint8_t *hardware_threads, uint32_t count);
void threads_place_none(void);
// Raises or lowers a thread's priority, -16 to 16 relative to normal (xsdk:docs/GUIDE.md, "Threads and where they
// run"): a raised thread that mostly waits, as the UI does for each vblank, runs as soon as it wakes even with a busy
// decoding thread on its hardware thread.
void threads_set_priority(pthread_t thread, int priority);
void threads_set_my_priority(int priority);

#endif
