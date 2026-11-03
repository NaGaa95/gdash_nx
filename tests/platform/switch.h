#ifndef TEST_SWITCH_H
#define TEST_SWITCH_H
#include <stdint.h>
typedef uint64_t u64;
typedef int Mutex;
static inline void mutexLock(Mutex *mutex) { (void)mutex; }
static inline void mutexUnlock(Mutex *mutex) { (void)mutex; }
static inline u64 armGetSystemTickFreq(void) { return 1000000; }
static inline u64 armGetSystemTick(void) { return 2000000; }
#endif
