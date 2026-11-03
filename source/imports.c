/* imports.c -- libcocos2dcpp.so + libfmod.so import resolution
 *
 * Every undefined dynamic symbol of the two game libs is bound here. The FMOD
 * C/C++ API cross-resolves from the loaded libfmod.so automatically; only
 * createSound/createStream are overridden for asset-path rewriting. GL goes to
 * the native mesa/nouveau drivers, sockets through the bionic<->BSD converters,
 * threads/fs through the bionic shims, and the rest to newlib.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "paths.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "net_shim.h"
#include "pthr.h"
#include "imports.h"

// crt/newlib-provided symbols we forward by address
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __stack_chk_fail(void);

// newlib's setjmp/longjmp are real functions; grab their addresses without
// tripping over the setjmp.h macros. sigsetjmp/siglongjmp bind to the same
// entry points: the extra savemask argument lands in a register setjmp
// ignores, and the buffer (bionic-sized, 256 B) is larger than newlib's.
extern int newlib_setjmp(void *env) asm("setjmp");
extern void newlib_longjmp(void *env, int val) asm("longjmp");

// ---------------------------------------------------------------------------
// small local shims
// ---------------------------------------------------------------------------

static int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}

// bionic clock(): microseconds of process time (CLOCKS_PER_SEC = 1000000)
static long clock_fake(void) {
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq();
  return (long)(armGetSystemTick() / (freq / 1000000ull));
}

static int getpwuid_r_fake(unsigned uid, void *pwd, char *buf, size_t buflen, void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result)
    *result = NULL;
  return 0; // "not found"
}

// C++ static-local init guards (libfmod imports these from libstdc++.so)
static Mutex cxa_guard_lock;

static int __cxa_guard_acquire_fake(uint64_t *guard) {
  mutexLock(&cxa_guard_lock);
  if (*(uint8_t *)guard) {
    mutexUnlock(&cxa_guard_lock);
    return 0; // already initialized
  }
  return 1; // caller runs the initializer, then calls release
}

static void __cxa_guard_release_fake(uint64_t *guard) {
  *(uint8_t *)guard = 1;
  mutexUnlock(&cxa_guard_lock);
}

static void __cxa_pure_virtual_fake(void) {
  abort();
}

static void operator_delete_fake(void *p) {
  free(p);
}

// the game's stack-guard cookie (both the global and the TLS slot land here)
static uintptr_t fake_stack_chk_guard = 0x4242424242424242ull;

// bionic's _ctype_ data symbol: 1 + 256 char-class table, indexed _ctype_[c+1]
// (index 0 covers EOF). Filled in update_imports() from the C-locale classes.
unsigned char bionic_ctype_table[1 + 256];

static void fill_ctype_table(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char b = 0;
    if (isupper(c))  b |= 0x01; // _U
    if (islower(c))  b |= 0x02; // _L
    if (isdigit(c))  b |= 0x04; // _N
    if (isspace(c))  b |= 0x08; // _S
    if (ispunct(c))  b |= 0x10; // _P
    if (iscntrl(c))  b |= 0x20; // _C
    if (isxdigit(c)) b |= 0x40; // _X
    if (c == ' ')    b |= 0x80; // _B
    bionic_ctype_table[c + 1] = b;
  }
  bionic_ctype_table[0] = 0;
}

// ---------------------------------------------------------------------------
// dlopen/dlsym: libfmod probes its output backends at runtime. AAudio and
// OpenSL are reported missing, so FMOD uses its AudioTrack output (org.fmod.
// AudioDevice -> audio.c). dlopen(NULL)/dlopen("libfmod.so") search the loaded
// modules and this import table.
// ---------------------------------------------------------------------------

#define DL_HANDLE_GLOBAL ((void *)0x474c4f42) // "GLOB"

extern so_module fmod_mod; // main.c

static void *dlopen_fake(const char *name, int flags) {
  (void)flags;
  if (!name)
    return DL_HANDLE_GLOBAL;
  if (strstr(name, "libfmod"))
    return &fmod_mod;
  // libOpenSLES.so, libaaudio.so, libandroid.so, ...: not available
  return NULL;
}

static void *dlsym_fake(void *handle, const char *name) {
  if (!name)
    return NULL;
  if (handle == &fmod_mod)
    return (void *)so_try_find_addr_rx(&fmod_mod, name);
  if (handle == DL_HANDLE_GLOBAL) {
    DynLibFunction *f = so_find_import(dynlib_functions, dynlib_numfunctions, name);
    if (f)
      return (void *)f->func;
    return (void *)so_try_find_addr_rx(&fmod_mod, name);
  }
  return NULL;
}

static int dlclose_fake(void *handle) {
  (void)handle;
  return 0;
}

static char *dlerror_fake(void) {
  return "not found";
}

// ---------------------------------------------------------------------------
// FMOD createSound/createStream: rewrite "file:///android_asset/<rel>" to the
// loose SD assets dir (and the hardcoded Android data dir onto the save dir);
// everything else passes through. The import table binds the game's calls to
// these wrappers, which tail into the real libfmod exports.
// ---------------------------------------------------------------------------

#define FMOD_OPENMEMORY        0x00000800
#define FMOD_OPENMEMORY_POINT  0x10000000

typedef int (*FmodCreateFn)(void *sys, const char *name, unsigned mode, void *exinfo, void **out);
static FmodCreateFn real_fmod_createSound;
static FmodCreateFn real_fmod_createStream;

static const char *fmod_fix_path(const char *name, unsigned mode, char *buf, size_t buflen) {
  if (!name || (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT)))
    return name;
  if (strncmp(name, ANDROID_ASSET_URI, ANDROID_ASSET_URI_LEN) == 0) {
    snprintf(buf, buflen, "%s/%s", path_assets(), name + ANDROID_ASSET_URI_LEN);
    return buf;
  }
  const char *suffix = path_android_private_suffix(name);
  if (suffix) {
    if (suffix[0])
      snprintf(buf, buflen, "%s/%s", path_save(), suffix);
    else
      snprintf(buf, buflen, "%s", path_save());
    return buf;
  }
  return name;
}

static int fmod_createSound_hook(void *sys, const char *name, unsigned mode, void *exinfo, void **out) {
  char buf[1024];
  return real_fmod_createSound(sys, fmod_fix_path(name, mode, buf, sizeof(buf)), mode, exinfo, out);
}

static int fmod_createStream_hook(void *sys, const char *name, unsigned mode, void *exinfo, void **out) {
  char buf[1024];
  return real_fmod_createStream(sys, fmod_fix_path(name, mode, buf, sizeof(buf)), mode, exinfo, out);
}

#define FMOD_SYM_CREATESOUND  "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define FMOD_SYM_CREATESTREAM "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"

void fmod_hooks_init(so_module *fmod) {
  real_fmod_createSound = (FmodCreateFn)so_find_addr_rx(fmod, FMOD_SYM_CREATESOUND);
  real_fmod_createStream = (FmodCreateFn)so_find_addr_rx(fmod, FMOD_SYM_CREATESTREAM);
}

// --- shims for symbols newlib lacks (used by the newer SubZero 2.2.147 NDK) --

static int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

static int __signbit_fake(double x) {
  return signbit(x) ? 1 : 0;
}

// thread-local destructor registration: we don't run TLS dtors, so drop it
static int __cxa_thread_atexit_impl_fake(void (*func)(void *), void *obj, void *dso) {
  (void)func; (void)obj; (void)dso;
  return 0;
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- runtime / bionic ------------------------------------------------------
  { "__android_log_print", (uintptr_t)&__android_log_print_fake },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__assert2", (uintptr_t)&__assert2_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire_fake },
  { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release_fake },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual_fake },
  { "_ZdlPv", (uintptr_t)&operator_delete_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__fpclassifyd", (uintptr_t)&__fpclassifyd_fake },
  { "__isnanf", (uintptr_t)&__isnanf_fake },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
  { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&fake_stack_chk_guard },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "_ctype_", (uintptr_t)&bionic_ctype_table[0] },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syslog", (uintptr_t)&ret0 },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "setpriority", (uintptr_t)&setpriority_fake },
  { "basename", (uintptr_t)&basename_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },

  // --- process / signals (stubs) --------------------------------------------
  { "getpid", (uintptr_t)&getpid_fake },
  { "fork", (uintptr_t)&fork_fake },
  { "execl", (uintptr_t)&execl_fake },
  { "waitpid", (uintptr_t)&waitpid_fake },
  { "kill", (uintptr_t)&kill_fake },
  { "alarm", (uintptr_t)&alarm_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigprocmask", (uintptr_t)&sigprocmask_fake },
  { "sigemptyset", (uintptr_t)&ret0 },
  { "sigfillset", (uintptr_t)&ret0 },
  { "sigaddset", (uintptr_t)&ret0 },
  { "sigdelset", (uintptr_t)&ret0 },
  { "getuid", (uintptr_t)&ret0 },
  { "geteuid", (uintptr_t)&ret0 },
  { "getgid", (uintptr_t)&ret0 },
  { "getegid", (uintptr_t)&ret0 },
  { "setuid", (uintptr_t)&ret0 },
  { "setgid", (uintptr_t)&ret0 },
  { "umask", (uintptr_t)&ret0 },
  { "initgroups", (uintptr_t)&ret0 },
  { "getpwuid", (uintptr_t)&ret0 },
  { "getpwuid_r", (uintptr_t)&getpwuid_r_fake },
  { "tcgetattr", (uintptr_t)&retm1 },
  { "tcsetattr", (uintptr_t)&retm1 },

  // --- setjmp ----------------------------------------------------------------
  { "setjmp", (uintptr_t)&newlib_setjmp },
  { "longjmp", (uintptr_t)&newlib_longjmp },
  { "sigsetjmp", (uintptr_t)&newlib_setjmp },
  { "siglongjmp", (uintptr_t)&newlib_longjmp },

  // --- math ------------------------------------------------------------------
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "asinh", (uintptr_t)&asinh },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "fmax", (uintptr_t)&fmax },
  { "fmin", (uintptr_t)&fmin },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lround", (uintptr_t)&lround },
  { "modf", (uintptr_t)&modf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sinf", (uintptr_t)&sinf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },

  // --- memory / stdlib --------------------------------------------------------
  { "abort", (uintptr_t)&abort },
  { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi },
  { "bsearch", (uintptr_t)&bsearch },
  { "calloc", (uintptr_t)&calloc },
  { "exit", (uintptr_t)&exit },
  { "free", (uintptr_t)&free },
  { "getenv", (uintptr_t)&getenv },
  { "malloc", (uintptr_t)&malloc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memrchr", (uintptr_t)&memrchr_fake },
  { "memset", (uintptr_t)&memset },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "realloc", (uintptr_t)&realloc },
  { "srand", (uintptr_t)&srand },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "atoll", (uintptr_t)&atoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },

  // --- strings ----------------------------------------------------------------
  { "strcat", (uintptr_t)&strcat },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strspn", (uintptr_t)&strspn },
  { "strstr", (uintptr_t)&strstr },
  { "strtok", (uintptr_t)&strtok },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strxfrm", (uintptr_t)&strxfrm },

  // --- ctype / wide -------------------------------------------------------------
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "isgraph", (uintptr_t)&isgraph },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "iswctype", (uintptr_t)&iswctype },
  { "wctype", (uintptr_t)&wctype },
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv_fake },

  // --- stdio ----------------------------------------------------------------
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getwc_fake },
  { "printf", (uintptr_t)&ret0 },
  { "putc", (uintptr_t)&fputc_fake },
  { "putwc", (uintptr_t)&putwc_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "swprintf", (uintptr_t)&swprintf },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "ungetwc", (uintptr_t)&ungetwc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },

  // --- fs / unix io -----------------------------------------------------------
  { "access", (uintptr_t)&access_fake },
  { "chmod", (uintptr_t)&chmod_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir },
  { "lseek", (uintptr_t)&lseek },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "open", (uintptr_t)&open_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "read", (uintptr_t)&read_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "write", (uintptr_t)&write_fake },
  { "writev", (uintptr_t)&writev_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&ret0 },
  { "madvise", (uintptr_t)&ret0 },
  { "mlock", (uintptr_t)&ret0 },
  { "__read_chk", (uintptr_t)&__read_chk_fake },

  // --- time / sched -----------------------------------------------------------
  { "clock", (uintptr_t)&clock_fake },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "difftime", (uintptr_t)&difftime },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },

  // --- GLES2 (native mesa, direct) ---------------------------------------------
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform2f", (uintptr_t)&glUniform2f },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform2i", (uintptr_t)&glUniform2i },
  { "glUniform2iv", (uintptr_t)&glUniform2iv },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform3i", (uintptr_t)&glUniform3i },
  { "glUniform3iv", (uintptr_t)&glUniform3iv },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniform4i", (uintptr_t)&glUniform4i },
  { "glUniform4iv", (uintptr_t)&glUniform4iv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // --- FMOD path rewrite (everything else FMOD resolves cross-module) ---------
  { FMOD_SYM_CREATESOUND, (uintptr_t)&fmod_createSound_hook },
  { FMOD_SYM_CREATESTREAM, (uintptr_t)&fmod_createStream_hook },

  // --- sockets (bionic<->BSD conversion, net_shim.c) ---------------------------
  { "accept", (uintptr_t)&accept_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake },
  { "getpeername", (uintptr_t)&getpeername_fake },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "select", (uintptr_t)&select_fake },
  { "send", (uintptr_t)&send_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "shutdown", (uintptr_t)&shutdown_fake },
  { "socket", (uintptr_t)&socket_fake },
  { "socketpair", (uintptr_t)&socketpair_fake },
  { "pipe", (uintptr_t)&pipe_fake },
  { "dup2", (uintptr_t)&dup2_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },

  // --- pthread (bionic<->newlib wrappers in pthr.c) ----------------------------
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_exit", (uintptr_t)&pthread_exit_soloader },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- SubZero 2.2.147 delta: extra libc imports from its newer NDK ---------
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__signbit", (uintptr_t)&__signbit_fake },
  { "closelog", (uintptr_t)&ret0 },
  { "openlog", (uintptr_t)&ret0 },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "abs", (uintptr_t)&abs },
  { "asinhf", (uintptr_t)&asinhf },
  { "tanhf", (uintptr_t)&tanhf },
  { "isascii", (uintptr_t)&isascii },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftello", (uintptr_t)&ftello },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "isdigit_l", (uintptr_t)&isdigit_l },
  { "islower_l", (uintptr_t)&islower_l },
  { "isupper_l", (uintptr_t)&isupper_l },
  { "isxdigit_l", (uintptr_t)&isxdigit_l },
  { "tolower_l", (uintptr_t)&tolower_l },
  { "toupper_l", (uintptr_t)&toupper_l },
  { "iswalpha_l", (uintptr_t)&iswalpha_l },
  { "iswblank_l", (uintptr_t)&iswblank_l },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l },
  { "iswdigit_l", (uintptr_t)&iswdigit_l },
  { "iswlower_l", (uintptr_t)&iswlower_l },
  { "iswprint_l", (uintptr_t)&iswprint_l },
  { "iswpunct_l", (uintptr_t)&iswpunct_l },
  { "iswspace_l", (uintptr_t)&iswspace_l },
  { "iswupper_l", (uintptr_t)&iswupper_l },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l },
  { "towlower_l", (uintptr_t)&towlower_l },
  { "towupper_l", (uintptr_t)&towupper_l },
  { "strcoll_l", (uintptr_t)&strcoll_l },
  { "strxfrm_l", (uintptr_t)&strxfrm_l },
  { "strftime_l", (uintptr_t)&strftime_l },
  { "wcscoll_l", (uintptr_t)&wcscoll_l },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  fill_ctype_table();
}
