/* jni_fake.c -- fake JNI environment for libcocos2dcpp.so + libfmod.so
 *
 * The game talks to Java through cocos2d's JniHelper (static methods on
 * Cocos2dxHelper / Cocos2dxGLSurfaceView / BaseRobTopActivity, dispatched
 * here BY NAME) plus a couple of constants (SDK_INT, WINDOW_SERVICE).
 * libfmod.so caches the JavaVM from JNI_OnLoad and queries org/fmod/FMOD and
 * org/fmod/AudioDevice; the AudioDevice.init/write calls drive the DAC (audio.c).
 *
 * The touch natives take jintArray/jfloatArray; those are real (fake) array
 * objects with element storage, so nativeTouchesMove's GetArrayLength +
 * Get*ArrayRegion work exactly like on Android (the Vita port had to hook
 * handleTouchesMove instead -- not needed here).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "game_compat.h"
#include "paths.h"
#include "util.h"
#include "jni_fake.h"
#include "prefs.h"
#include "audio.h"
#include "net_shim.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

volatile int g_block_back_button = 0;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT  = 0x4f424a31, // 'OBJ1'
  TAG_STRING  = 0x53545231, // 'STR1'
  TAG_OBJARR  = 0x4f415231, // 'OAR1'
  TAG_PRIMARR = 0x50415231, // 'PAR1'
  TAG_ID      = 0x4d494431, // 'MID1'
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;                // normal UTF-8 for the port's C-string callbacks
  uint16_t *chars;          // immutable UTF-16 storage for GetStringChars
  int length;              // UTF-16 code units, not UTF-8 bytes
  char *mutf;               // JNI modified UTF-8 (NUL and surrogates encoded)
  size_t mutf_length;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

// primitive array with element storage; elem is the element width in bytes
typedef struct {
  uint32_t tag;
  int len;
  int elem;
  void *data;
} FakePrimArray;

// method and field IDs are pointers to these records; calls dispatch by name
typedef struct {
  uint32_t tag;
  char name[80];
  char sig[80];
} FakeID;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  return o;
}

static size_t write_utf8(char *out, uint32_t codepoint) {
  if (codepoint < 0x80) {
    out[0] = (char)codepoint;
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = (char)(0xc0 | (codepoint >> 6));
    out[1] = (char)(0x80 | (codepoint & 0x3f));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = (char)(0xe0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    out[2] = (char)(0x80 | (codepoint & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (codepoint >> 18));
  out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
  out[3] = (char)(0x80 | (codepoint & 0x3f));
  return 4;
}

static size_t write_mutf8(char *out, uint16_t unit) {
  if (!unit) {
    out[0] = (char)0xc0;
    out[1] = (char)0x80;
    return 2;
  }
  return write_utf8(out, unit);
}

static FakeString *make_string_utf16(const uint16_t *chars, int length) {
  if (length < 0 || (!chars && length) || (size_t)length > (SIZE_MAX - 1) / 3)
    return NULL;
  FakeString *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->chars = malloc(((size_t)length + 1) * sizeof(*s->chars));
  s->utf = malloc((size_t)length * 3 + 1);
  s->mutf = malloc((size_t)length * 3 + 1);
  if (!s->chars || !s->utf || !s->mutf) {
    free(s->chars);
    free(s->utf);
    free(s->mutf);
    free(s);
    return NULL;
  }
  s->tag = TAG_STRING;
  s->length = length;
  if (length)
    memcpy(s->chars, chars, (size_t)length * sizeof(*chars));
  s->chars[length] = 0;
  for (int i = 0; i < length; i++)
    s->mutf_length += write_mutf8(s->mutf + s->mutf_length, chars[i]);
  s->mutf[s->mutf_length] = 0;

  size_t bytes = 0;
  for (int i = 0; i < length; i++) {
    uint32_t cp = chars[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < length &&
        chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (chars[++i] - 0xdc00);
    } else if (cp >= 0xd800 && cp <= 0xdfff) {
      cp = 0xfffd; // retain the unit in JNI; C callbacks receive valid UTF-8
    }
    bytes += write_utf8(s->utf + bytes, cp);
  }
  s->utf[bytes] = 0;
  return s;
}

// Accept normal UTF-8 from the port and JNI's modified UTF-8 from the game.
// Modified UTF-8 encodes NUL as C0 80 and each surrogate as three bytes.
static uint32_t read_utf8(const unsigned char **cursor, const unsigned char *end) {
  unsigned char first = *(*cursor)++;
  if (first < 0x80)
    return first;
  if (first == 0xc0 && *cursor < end && **cursor == 0x80) {
    (*cursor)++;
    return 0;
  }
  unsigned count;
  uint32_t cp, minimum;
  if (first >= 0xc2 && first <= 0xdf) {
    count = 1; cp = first & 0x1f; minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    count = 2; cp = first & 0x0f; minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    count = 3; cp = first & 7; minimum = 0x10000;
  } else {
    return 0xfffd;
  }
  if ((size_t)(end - *cursor) < count)
    return 0xfffd;
  for (unsigned i = 0; i < count; i++) {
    if (((*cursor)[i] & 0xc0) != 0x80)
      return 0xfffd;
    cp = (cp << 6) | ((*cursor)[i] & 0x3f);
  }
  *cursor += count;
  return cp < minimum || cp > 0x10ffff ? 0xfffd : cp;
}

void *jni_make_string(const char *utf) {
  if (!utf)
    utf = "";
  size_t bytes = strlen(utf);
  if (bytes > INT_MAX || bytes > SIZE_MAX / sizeof(uint16_t) - 1)
    return NULL;
  uint16_t *chars = malloc((bytes + 1) * sizeof(*chars));
  if (!chars)
    return NULL;
  const unsigned char *cursor = (const unsigned char *)utf;
  const unsigned char *end = cursor + bytes;
  int length = 0;
  while (cursor < end) {
    uint32_t cp = read_utf8(&cursor, end);
    if (cp > 0xffff) {
      cp -= 0x10000;
      chars[length++] = (uint16_t)(0xd800 | (cp >> 10));
      chars[length++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
    } else {
      chars[length++] = (uint16_t)cp;
    }
  }
  FakeString *s = make_string_utf16(chars, length);
  free(chars);
  return s;
}

void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = n;
  a->items = calloc(n ? n : 1, sizeof(void *));
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(strs[i]);
  return a;
}

static void *make_prim_array(int n, int elem, const void *vals) {
  FakePrimArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIMARR;
  a->len = n;
  a->elem = elem;
  a->data = calloc(n ? n : 1, elem);
  if (vals)
    memcpy(a->data, vals, (size_t)n * elem);
  return a;
}

void *jni_make_int_array(int n, const int *vals)     { return make_prim_array(n, 4, vals); }
void *jni_make_float_array(int n, const float *vals) { return make_prim_array(n, 4, vals); }
void *jni_make_byte_array(int n, const void *data)   { return make_prim_array(n, 1, data); }

// refill a reusable primitive array in place (len shrinks to n; capacity is
// whatever the array was created with -- the caller keeps n within it)
void jni_prim_array_fill(void *arr, const void *data, int n, int elem) {
  FakePrimArray *a = arr;
  if (!a || a->tag != TAG_PRIMARR || a->elem != elem)
    return;
  a->len = n;
  memcpy(a->data, data, (size_t)n * elem);
}

// C string behind a fake jstring ("" for anything else)
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING && s->utf)
    return s->utf;
  return "";
}

static const char *obj_str(void *jstr) {
  return jni_string_utf(jstr);
}

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 192
static FakeID id_pool[MAX_IDS];
static int id_count = 0;
static Mutex id_lock; // FMOD threads and the GL thread both resolve IDs

static FakeID *get_id(const char *name, const char *sig) {
  if (!name) name = "";
  if (!sig) sig = "";
  mutexLock(&id_lock);
  for (int i = 0; i < id_count; i++) {
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig)) {
      mutexUnlock(&id_lock);
      return &id_pool[i];
    }
  }
  if (id_count >= MAX_IDS) {
    mutexUnlock(&id_lock);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  mutexUnlock(&id_lock);
  return id;
}

// ---------------------------------------------------------------------------
// method dispatch (by name). Argument extraction relies on C variadic
// promotion: jboolean/jint -> int, jfloat/jdouble -> double, jlong -> long
// long, references -> pointers.
// ---------------------------------------------------------------------------

static juint call_boolean(const char *name, const char *sig, va_list va) {
  // --- FMOD (org/fmod/FMOD) ---
  if (!strcmp(name, "checkInit"))
    return 1; // pretend org.fmod.FMOD.init(context) was called
  if (!strcmp(name, "supportsAAudio"))
    return 0; // no libaaudio.so -> FMOD uses its AudioTrack (org.fmod.AudioDevice) output
  if (!strcmp(name, "supportsLowLatency") || !strcmp(name, "lowLatencyFlag"))
    return 0;
  if (!strcmp(name, "isBluetoothOn"))
    return 0;

  // org.fmod.AudioDevice.init(channels, sampleRate, bufferSize, numBuffers):
  // FMOD's AudioTrack output opens the sink here. Detect rate/channels by value
  // (robust to arg order) and bring up the DAC; 1 = output live.
  if (!strcmp(name, "init") && sig && !strcmp(sig, "(IIII)Z")) {
    int a[4];
    for (int i = 0; i < 4; i++) a[i] = va_arg(va, int);
    int rate = 48000, ch = 2;
    for (int i = 0; i < 4; i++)
      if (a[i] == 8000 || a[i] == 11025 || a[i] == 16000 || a[i] == 22050 ||
          a[i] == 24000 || a[i] == 32000 || a[i] == 44100 || a[i] == 48000)
        rate = a[i];
    for (int i = 0; i < 4; i++)
      if (a[i] == 1 || a[i] == 2) { ch = a[i]; break; }
    return nx_audio_init(rate, ch) ? 1 : 0;
  }

  // --- game ---
  if (!strcmp(name, "getBoolForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    int def = va_arg(va, int);
    return (juint)prefs_get_bool(key, def);
  }
  if (!strcmp(name, "isNetworkAvailable"))
    return net_is_available();
  if (!strcmp(name, "gameServicesIsSignedIn"))
    return 0;
  return 0;
}

static juint call_int(const char *name, va_list va) {
  // --- FMOD ---
  if (!strcmp(name, "getOutputSampleRate"))
    return 48000;
  if (!strcmp(name, "getOutputBlockSize"))
    return 1024;

  // --- game ---
  if (!strcmp(name, "getIntegerForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    int def = va_arg(va, int);
    return (juint)(uint32_t)prefs_get_int(key, def);
  }
  if (!strcmp(name, "getDPI"))
    return 236; // 6.2" 1280x720 handheld panel
  if (!strcmp(name, "getFontSizeAccordingHeight")) {
    int h = va_arg(va, int);
    return (juint)(h > 0 ? h * 3 / 4 : 12);
  }
  if (!strcmp(name, "getAPILevel"))
    return ANDROID_SDK_INT;
  return 0;
}

static int64_t call_long(const char *name, va_list va) {
  (void)va;
  if (!strcmp(name, "uptimeMillis")) {
    static u64 freq = 0;
    if (!freq)
      freq = armGetSystemTickFreq();
    return (int64_t)(armGetSystemTick() / (freq / 1000ull));
  }
  return 0;
}

static float call_float(const char *name, va_list va) {
  if (!strcmp(name, "getDeviceRefreshRate"))
    return 60.0f;
  if (!strcmp(name, "getFloatForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    float def = (float)va_arg(va, double);
    return prefs_get_float(key, def);
  }
  return 0.0f;
}

static double call_double(const char *name, va_list va) {
  if (!strcmp(name, "getDoubleForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    double def = va_arg(va, double);
    return prefs_get_double(key, def);
  }
  return 0.0;
}

static void *call_object(const char *name, va_list va) {
  // nativeInit-era plumbing some builds walk: keep non-NULL
  if (!strcmp(name, "getClassLoader"))
    return jni_make_object("ClassLoader");
  if (!strcmp(name, "loadClass"))
    return jni_make_object("Class");
  if (!strcmp(name, "getContext"))
    return jni_make_object("Context");
  if (!strcmp(name, "getAssets"))
    return jni_make_object("AssetManager");

  if (!strcmp(name, "getUserID"))
    return jni_make_string(net_user_id());
  if (!strcmp(name, "getCocos2dxWritablePath"))
    return jni_make_string(path_save());
  if (!strcmp(name, "getCocos2dxPackageName"))
    return jni_make_string(game_compat_package_name());
  if (!strcmp(name, "getCurrentLanguage"))
    return jni_make_string(config_lang_iso2());
  if (!strcmp(name, "getStringForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    const char *def = obj_str(va_arg(va, void *));
    return jni_make_string(prefs_get_string(key, def));
  }
  if (!strcmp(name, "getStringWithEllipsis")) {
    // (string, width, fontSize) -> string; return the input unmodified
    void *in = va_arg(va, void *);
    return jni_make_string(obj_str(in));
  }
  if (!strcmp(name, "loadAndDecryptFileToString"))
    return NULL; // "no such file": the game falls back to its own save path

  // unknown object-returning method: empty string is the safest non-NULL
  // (JniHelper's jstring2string doesn't always null-check)
  return jni_make_string("");
}

static void call_void(const char *name, const char *sig, va_list va) {
  // org.fmod.AudioDevice.write(short[] data, int lengthInShorts): FMOD's mixed
  // PCM. The short[] is a real fake-array with backing storage (FMOD filled it
  // via Get/ReleaseShortArrayElements); push it to the DAC.
  if (!strcmp(name, "write") && sig && !strcmp(sig, "([SI)V")) {
    void *arr = va_arg(va, void *);
    int len = va_arg(va, int);
    FakePrimArray *pa = arr;
    if (pa && pa->tag == TAG_PRIMARR && pa->data)
      nx_audio_write(pa->data, len);
    return;
  }

  if (!strcmp(name, "setBoolForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    int v = va_arg(va, int);
    prefs_set_bool(key, v);
    return;
  }
  if (!strcmp(name, "setIntegerForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    int v = va_arg(va, int);
    prefs_set_int(key, v);
    return;
  }
  if (!strcmp(name, "setFloatForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    float v = (float)va_arg(va, double);
    prefs_set_float(key, v);
    return;
  }
  if (!strcmp(name, "setDoubleForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    double v = va_arg(va, double);
    prefs_set_double(key, v);
    return;
  }
  if (!strcmp(name, "setStringForKey")) {
    const char *key = obj_str(va_arg(va, void *));
    const char *v = obj_str(va_arg(va, void *));
    prefs_set_string(key, v);
    return;
  }
  if (!strcmp(name, "setBlockBackButton")) {
    g_block_back_button = va_arg(va, int) ? 1 : 0;
    return;
  }
  if (!strcmp(name, "openIMEKeyboard")) {
    gd_open_ime_keyboard();
    return;
  }
  if (!strcmp(name, "showEditTextDialog")) {
    const char *title = obj_str(va_arg(va, void *));
    const char *msg = obj_str(va_arg(va, void *));
    (void)va_arg(va, int); // inputMode
    (void)va_arg(va, int); // inputFlag
    (void)va_arg(va, int); // returnType
    int maxlen = va_arg(va, int);
    gd_show_edittext_dialog(title, msg, maxlen);
    return;
  }
  if (!strcmp(name, "terminateProcess")) {
    gd_request_quit();
    return;
  }
  // setAnimationInterval, loadingFinished, openURL, copyToClipboard,
  // setKeyboardState, closeIMEKeyboard, showDialog, gameServices*,
  // unlockAchievement, banner/ad calls, onToggleKeyboard, ...: no-op
  (void)name; (void)va;
}

static juint get_static_int_field(const char *name) {
  if (!strcmp(name, "SDK_INT"))
    return ANDROID_SDK_INT;
  return 0;
}

static void *get_object_field(const char *name) {
  if (!strcmp(name, "WINDOW_SERVICE"))
    return jni_make_string("window");
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return jni_make_object(name ? name : "class");
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  return get_id(name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  return get_id(name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env; (void)obj;
  return jni_make_object("class");
}

static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

// --- Call<type>Method: instance calls share the static dispatchers ----------

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_boolean(id->name, id->sig, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = call_boolean(id->name, id->sig, va);
  va_end(va); return r;
}

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_object(id->name, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va); return r;
}

static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; call_void(id->name, id->sig, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  call_void(id->name, id->sig, va);
  va_end(va);
}

static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_int(id->name, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = call_int(id->name, va);
  va_end(va); return r;
}

static int64_t j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_long(id->name, va);
}
static int64_t j_CallLongMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  int64_t r = call_long(id->name, va);
  va_end(va); return r;
}

static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_float(id->name, va);
}
static float j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = call_float(id->name, va);
  va_end(va); return r;
}

static double j_CallDoubleMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return call_double(id->name, va);
}
static double j_CallDoubleMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  double r = call_double(id->name, va);
  va_end(va); return r;
}

// --- static variants ---------------------------------------------------------

static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_object(id->name, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = call_object(id->name, va);
  va_end(va); return r;
}
static juint j_CallStaticBooleanMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_boolean(id->name, id->sig, va);
}
static juint j_CallStaticBooleanMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = call_boolean(id->name, id->sig, va);
  va_end(va); return r;
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; call_void(id->name, id->sig, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  call_void(id->name, id->sig, va);
  va_end(va);
}
static juint j_CallStaticIntMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_int(id->name, va);
}
static juint j_CallStaticIntMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = call_int(id->name, va);
  va_end(va); return r;
}
static int64_t j_CallStaticLongMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_long(id->name, va);
}
static int64_t j_CallStaticLongMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  int64_t r = call_long(id->name, va);
  va_end(va); return r;
}
static float j_CallStaticFloatMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_float(id->name, va);
}
static float j_CallStaticFloatMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = call_float(id->name, va);
  va_end(va); return r;
}
static double j_CallStaticDoubleMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return call_double(id->name, va);
}
static double j_CallStaticDoubleMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  double r = call_double(id->name, va);
  va_end(va); return r;
}

// --- fields ------------------------------------------------------------------

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; return get_object_field(id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; return get_static_int_field(id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; (void)id; return 0;
}

// --- strings -----------------------------------------------------------------

static void *j_NewString(void *env, const uint16_t *chars, int length) {
  (void)env; return make_string_utf16(chars, length);
}
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  static const uint16_t empty[] = {0};
  if (is_copy) *is_copy = 0;
  FakeString *s = jstr;
  return s && s->tag == TAG_STRING ? s->chars : empty;
}
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr; (void)chars; // immutable storage belongs to the string
}
static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env; return jni_make_string(utf);
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakeString *s = jstr;
  return s && s->tag == TAG_STRING ? s->mutf : "";
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}
static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  FakeString *s = jstr;
  return s && s->tag == TAG_STRING ? s->mutf_length : 0;
}
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  FakeString *s = jstr;
  return s && s->tag == TAG_STRING ? (juint)s->length : 0;
}

// --- arrays ------------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *oa = arr;
  if (oa && oa->tag == TAG_OBJARR) return oa->len;
  FakePrimArray *pa = arr;
  if (pa && pa->tag == TAG_PRIMARR) return pa->len;
  return 0;
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    return a->items[idx];
  return jni_make_string("");
}
static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return a;
}

static void *j_NewPrimArray1(void *env, int len) { // byte/boolean
  (void)env; return make_prim_array(len, 1, NULL);
}
static void *j_NewPrimArray2(void *env, int len) { // char/short
  (void)env; return make_prim_array(len, 2, NULL);
}
static void *j_NewPrimArray4(void *env, int len) { // int/float
  (void)env; return make_prim_array(len, 4, NULL);
}
static void *j_NewPrimArray8(void *env, int len) { // long/double
  (void)env; return make_prim_array(len, 8, NULL);
}

static void *j_GetPrimArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakePrimArray *a = arr;
  if (a && a->tag == TAG_PRIMARR)
    return a->data;
  return NULL;
}
static void j_ReleasePrimArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode; // elements ARE the storage
}

static void j_GetPrimArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePrimArray *a = arr;
  if (!a || a->tag != TAG_PRIMARR || !buf)
    return;
  if (start < 0 || len < 0 || start + len > a->len)
    return;
  memcpy(buf, (char *)a->data + (size_t)start * a->elem, (size_t)len * a->elem);
}
static void j_SetPrimArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePrimArray *a = arr;
  if (!a || a->tag != TAG_PRIMARR || !buf)
    return;
  if (start < 0 || len < 0 || start + len > a->len)
    return;
  memcpy((char *)a->data + (size_t)start * a->elem, buf, (size_t)len * a->elem);
}

// string regions (some helpers use these instead of Get*Chars)
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  FakeString *s = jstr;
  if (!s || s->tag != TAG_STRING || !buf || start < 0 || len < 0 ||
      start > s->length || len > s->length - start)
    return;
  memcpy(buf, s->chars + start, (size_t)len * sizeof(*buf));
}
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  FakeString *s = jstr;
  if (!s || s->tag != TAG_STRING || !buf || start < 0 || len < 0 ||
      start > s->length || len > s->length - start)
    return;
  size_t bytes = 0;
  for (int i = 0; i < len; i++)
    bytes += write_mutf8(buf + bytes, s->chars[start + i]);
}

// NewObject family: hand out a plain fake object (method calls on it
// dispatch by name like everything else)
static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env; (void)cls; (void)mid;
  return jni_make_object("new");
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; (void)cls; (void)mid; (void)va;
  return jni_make_object("new");
}

// --- misc --------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n;
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) {
  (void)env; *vm = fake_vm; return JNI_OK;
}
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void_1(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

static juint j_unimplemented(void) {
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void_1; // ExceptionDescribe
  env_table[17]  = (void *)j_void_1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef; // DeleteGlobalRef
  env_table[23]  = (void *)j_DeleteRef; // DeleteLocalRef
  env_table[24]  = (void *)j_ret0_3;    // IsSameObject
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_ret0_2;    // EnsureLocalCapacity
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_ret0_3;    // IsInstanceOf
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetBooleanField; // GetLongField
  env_table[113] = (void *)j_GetMethodID; // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID; // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField; // GetStaticObjectField
  env_table[150] = (void *)j_GetIntField;    // GetStaticIntField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_ret0_3;         // SetObjectArrayElement (unused)
  env_table[175] = (void *)j_NewPrimArray1;  // NewBooleanArray
  env_table[176] = (void *)j_NewPrimArray1;  // NewByteArray
  env_table[177] = (void *)j_NewPrimArray2;  // NewCharArray
  env_table[178] = (void *)j_NewPrimArray2;  // NewShortArray
  env_table[179] = (void *)j_NewPrimArray4;  // NewIntArray
  env_table[180] = (void *)j_NewPrimArray8;  // NewLongArray
  env_table[181] = (void *)j_NewPrimArray4;  // NewFloatArray
  env_table[182] = (void *)j_NewPrimArray8;  // NewDoubleArray
  for (int i = 183; i <= 190; i++)
    env_table[i] = (void *)j_GetPrimArrayElements;
  for (int i = 191; i <= 198; i++)
    env_table[i] = (void *)j_ReleasePrimArrayElements;
  for (int i = 199; i <= 206; i++)
    env_table[i] = (void *)j_GetPrimArrayRegion;
  for (int i = 207; i <= 214; i++)
    env_table[i] = (void *)j_SetPrimArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2; // UnregisterNatives
  env_table[217] = (void *)j_ret0_2; // MonitorEnter
  env_table[218] = (void *)j_ret0_2; // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion;
  env_table[222] = (void *)j_GetPrimArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePrimArrayElements; // ReleasePrimitiveArrayCritical
  env_table[224] = (void *)j_GetStringChars;           // GetStringCritical
  env_table[225] = (void *)j_ReleaseStringChars;       // ReleaseStringCritical
  env_table[226] = (void *)j_NewGlobalRef;             // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteRef;                // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
