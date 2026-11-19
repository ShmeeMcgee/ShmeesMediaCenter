// Shmees Media Center: thread placement (threads.h). On the console the link wraps pthread_create
// (-Wl,--wrap=pthread_create), so threads FFmpeg starts inside avcodec_open2 can be placed as they're made.

#include "threads.h"

#include <stdbool.h>

#include "log.h"

#ifdef __XSDK__
#include "xsdk/thread.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t round_list[8];
static uint32_t round_count;
static uint32_t round_next;

void threads_place(pthread_t thread, uint32_t hardware_thread)
{
	const NTSTATUS status = xsdk_thread_place(xsdk_pthread_handle(thread), hardware_thread);
	if (!NT_SUCCESS(status))
		smc_log("threads: placing a thread on %u failed: 0x%08X", (unsigned)hardware_thread, (unsigned)status);
}

void threads_place_round(const uint8_t *hardware_threads, uint32_t count)
{
	pthread_mutex_lock(&lock);
	round_count = count > sizeof round_list ? sizeof round_list : count;
	for (uint32_t i = 0; i < round_count; i++)
		round_list[i] = hardware_threads[i];
	round_next = 0;
	pthread_mutex_unlock(&lock);
}

void threads_place_none(void)
{
	pthread_mutex_lock(&lock);
	round_count = 0;
	pthread_mutex_unlock(&lock);
}

void threads_set_priority(pthread_t thread, int priority)
{
	const NTSTATUS status = xsdk_thread_set_priority(xsdk_pthread_handle(thread), priority, NULL);
	if (!NT_SUCCESS(status))
		smc_log("threads: priority %d failed: 0x%08X", priority, (unsigned)status);
}

void threads_set_my_priority(int priority)
{
	const NTSTATUS status = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, priority, NULL);
	if (!NT_SUCCESS(status))
		smc_log("threads: priority %d failed: 0x%08X", priority, (unsigned)status);
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *argument);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *argument)
{
	const int error = __real_pthread_create(thread, attr, start, argument);
	if (error)
		return error;
	pthread_mutex_lock(&lock);
	const bool place = round_count > 0;
	const uint32_t hw = place ? round_list[round_next++ % round_count] : 0;
	pthread_mutex_unlock(&lock);
	if (place)
		threads_place(*thread, hw);
	return 0;
}

#else
// The host preview: one machine's scheduler does the placing.
void threads_place(pthread_t thread, uint32_t hardware_thread)
{
	(void)thread;
	(void)hardware_thread;
}
void threads_place_round(const uint8_t *hardware_threads, uint32_t count)
{
	(void)hardware_threads;
	(void)count;
}
void threads_place_none(void)
{
}
void threads_set_priority(pthread_t thread, int priority)
{
	(void)thread;
	(void)priority;
}
void threads_set_my_priority(int priority)
{
	(void)priority;
}
#endif
