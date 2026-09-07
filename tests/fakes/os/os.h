#ifndef TEST_OS_H
#define TEST_OS_H
#include <stdint.h>
typedef uint32_t os_time_t;
#define OS_TICKS_PER_SEC 128
os_time_t os_time_get(void);
void os_time_delay(os_time_t ticks);
static inline os_time_t os_time_ms_to_ticks32(uint32_t ms) { return ms * 128 / 1000; }
#endif
