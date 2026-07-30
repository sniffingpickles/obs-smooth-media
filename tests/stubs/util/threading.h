#pragma once

/*
 * Core tests do not link libobs; the production header only needs the
 * pthread types for audio_buffer and clock_tracker.
 */
#include <pthread.h>
#include <stdbool.h>

void os_set_thread_name(const char *name);

#if !defined(_MSC_VER)
static inline bool os_atomic_set_bool(volatile bool *ptr, bool value)
{
	return __atomic_exchange_n(ptr, value, __ATOMIC_SEQ_CST);
}

static inline bool os_atomic_exchange_bool(volatile bool *ptr, bool value)
{
	return os_atomic_set_bool(ptr, value);
}

static inline bool os_atomic_load_bool(const volatile bool *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}
#endif
