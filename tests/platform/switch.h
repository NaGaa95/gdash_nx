#ifndef TEST_SWITCH_H
#define TEST_SWITCH_H
#include <stdint.h>
typedef uint64_t u64;
typedef int Mutex;
typedef int SetLanguage;
enum { SetLanguage_ENUS,SetLanguage_FR,SetLanguage_FRCA,SetLanguage_DE,
       SetLanguage_IT,SetLanguage_ES,SetLanguage_ES419,SetLanguage_NL,
       SetLanguage_PT,SetLanguage_PTBR,SetLanguage_RU,SetLanguage_JA,
       SetLanguage_KO,SetLanguage_ZHCN,SetLanguage_ZHHANS,SetLanguage_ZHTW,SetLanguage_ZHHANT };
#define R_SUCCEEDED(result) ((result)==0)
static inline int setInitialize(void) { return 0; }
static inline int setGetSystemLanguage(u64 *code) { *code=0;return 0; }
static inline int setMakeLanguage(u64 code,SetLanguage *language) { (void)code;*language=SetLanguage_ENUS;return 0; }
static inline void setExit(void) { }
static inline void mutexLock(Mutex *mutex) { (void)mutex; }
static inline void mutexUnlock(Mutex *mutex) { (void)mutex; }
static inline u64 armGetSystemTickFreq(void) { return 1000000; }
static inline u64 armGetSystemTick(void) { return 2000000; }
#endif
