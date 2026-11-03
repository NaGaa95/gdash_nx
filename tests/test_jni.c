#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jni_fake.h"
#include "prefs.h"

typedef struct {
  const char *title, *package, *save_path;
} TestGame;
static const TestGame games[] = {
  {"Full", "com.robtopx.geometryjump", "/switch/gdash_full/save"},
  {"Meltdown", "com.robtopx.geometrydashmeltdown", "/switch/gdash_meltdown/save"},
  {"SubZero", "com.robtopx.geometrydashsubzero", "/switch/gdash_subzero/save"},
  {"World", "com.robtopx.geometrydashworld", "/switch/gdash_world/save"},
  {"Lite", "com.robtopx.geometryjumplite", "/switch/gdash_lite/save"},
};
static const TestGame *active_game = &games[0];

/* Platform dependencies of the production JNI dispatcher. */
const char *path_save(void) { return active_game->save_path; }
const char *game_compat_package_name(void) { return active_game->package; }
const char *config_lang_iso2(void) { return "en"; }
const char *net_user_id(void) { return "test-id"; }
int net_is_available(void) { return 1; }
int nx_audio_init(int rate, int channels) { (void)rate; (void)channels; return 1; }
void nx_audio_write(const void *pcm, int count) { (void)pcm; (void)count; }
void gd_open_ime_keyboard(void) { }
void gd_show_edittext_dialog(const char *title, const char *text, int maxlen) {
  (void)title; (void)text; (void)maxlen;
}
void gd_request_quit(void) { }
int prefs_get_bool(const char *key, int value) { (void)key; return value; }
int prefs_get_int(const char *key, int value) { (void)key; return value; }
float prefs_get_float(const char *key, float value) { (void)key; return value; }
double prefs_get_double(const char *key, double value) { (void)key; return value; }
const char *prefs_get_string(const char *key, const char *value) { (void)key; return value; }
void prefs_set_bool(const char *key, int value) { (void)key; (void)value; }
void prefs_set_int(const char *key, int value) { (void)key; (void)value; }
void prefs_set_float(const char *key, float value) { (void)key; (void)value; }
void prefs_set_double(const char *key, double value) { (void)key; (void)value; }
void prefs_set_string(const char *key, const char *value) { (void)key; (void)value; }

static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
  jni_init();
  void **table = *(void ***)fake_env;
  void *(*find_class)(void *, const char *) = (void *)table[6];
  void *(*method_id)(void *, void *, const char *, const char *) = (void *)table[113];
  void *(*call_object)(void *, void *, void *, ...) = (void *)table[114];
  int (*length)(void *, void *) = (void *)table[164];
  const uint16_t *(*chars)(void *, void *, uint8_t *) = (void *)table[165];
  void (*release)(void *, void *, const uint16_t *) = (void *)table[166];
  void *(*new_string)(void *, const uint16_t *, int) = (void *)table[163];
  void *(*new_utf)(void *, const char *) = (void *)table[167];
  int (*utf_length)(void *, void *) = (void *)table[168];
  const char *(*utf_chars)(void *, void *, uint8_t *) = (void *)table[169];
  void (*utf_release)(void *, void *, const char *) = (void *)table[170];
  void (*region)(void *, void *, int, int, uint16_t *) = (void *)table[220];
  void (*utf_region)(void *, void *, int, int, char *) = (void *)table[221];
  const uint16_t *(*critical)(void *, void *, uint8_t *) = (void *)table[224];
  void (*release_critical)(void *, void *, const uint16_t *) = (void *)table[225];

  /* Reproduce getFileDirectoryJNI -> jstring2string from the crash stack. */
  void *cls = find_class(fake_env, "org/cocos2dx/lib/Cocos2dxHelper");
  void *method = method_id(fake_env, cls, "getCocos2dxWritablePath", "()Ljava/lang/String;");
  void *path = call_object(fake_env, cls, method);
  CHECK(path != NULL);
  int count = length(fake_env, path);
  CHECK(count == (int)strlen(path_save()));
  uint8_t copied = 99;
  const uint16_t *text = chars(fake_env, path, &copied);
  CHECK(text != NULL); /* The unfixed dispatcher returned NULL from JNI slot 165. */
  CHECK(copied == 0);
  for (int i = 0; i < count; i++)
    CHECK(text[i] == (unsigned char)path_save()[i]);
  release(fake_env, path, text);
  CHECK(!strcmp(utf_chars(fake_env, path, NULL), path_save()));

  /* Register and exercise the same JNI implementation for every game. */
  for (unsigned game = 0; game < sizeof games / sizeof games[0]; game++) {
    active_game = &games[game];
    jni_init();
    void *save_method = method_id(fake_env, cls, "getCocos2dxWritablePath", "()Ljava/lang/String;");
    void *save = call_object(fake_env, cls, save_method);
    CHECK(save != NULL);
    CHECK(length(fake_env, save) == (int)strlen(path_save()));
    uint8_t save_copy = 99;
    const uint16_t *save_chars = chars(fake_env, save, &save_copy);
    CHECK(save_chars != NULL);
    CHECK(save_copy == 0);
    uint16_t expected[128];
    size_t path_length = strlen(path_save());
    CHECK(path_length < sizeof expected / sizeof expected[0]);
    for (size_t i = 0; i < path_length; i++)
      expected[i] = (unsigned char)path_save()[i];
    CHECK(!memcmp(save_chars, expected, path_length * sizeof(uint16_t)));
    CHECK(!strcmp(utf_chars(fake_env, save, NULL), path_save()));
    CHECK(critical(fake_env, save, NULL) == save_chars);
    release_critical(fake_env, save, save_chars);
    release(fake_env, save, save_chars);
    void *package_method = method_id(fake_env, cls, "getCocos2dxPackageName", "()Ljava/lang/String;");
    void *package = call_object(fake_env, cls, package_method);
    CHECK(package != NULL);
    CHECK(!strcmp(utf_chars(fake_env, package, NULL), active_game->package));
    printf("PASS: shared JNI startup for %s\n", active_game->title);
  }
  active_game = &games[0];

  void *empty = new_string(fake_env, NULL, 0);
  CHECK(empty != NULL && length(fake_env, empty) == 0);
  CHECK(chars(fake_env, empty, NULL) != NULL);
  CHECK(utf_length(fake_env, empty) == 0);
  CHECK(!strcmp(utf_chars(fake_env, empty, NULL), ""));
  CHECK(new_string(fake_env, NULL, 1) == NULL);
  CHECK(new_string(fake_env, NULL, -1) == NULL);

  /* e-acute, Korean, and a supplementary-plane character, followed by NUL. */
  const uint16_t units[] = {'A', 0xe9, 0xd55c, 0xd83d, 0xde00, 0, 'Z'};
  const char modified[] = "A\xc3\xa9\xed\x95\x9c\xed\xa0\xbd\xed\xb8\x80\xc0\x80Z";
  void *unicode = new_string(fake_env, units, 7);
  CHECK(unicode != NULL && length(fake_env, unicode) == 7);
  text = chars(fake_env, unicode, &copied);
  CHECK(text != NULL && copied == 0 && !memcmp(text, units, sizeof units));
  CHECK(utf_length(fake_env, unicode) == (int)sizeof(modified) - 1);
  const char *modified_chars = utf_chars(fake_env, unicode, &copied);
  CHECK(copied == 0 && !strcmp(modified_chars, modified));
  void *roundtrip = new_utf(fake_env, modified_chars);
  CHECK(length(fake_env, roundtrip) == 7);
  CHECK(!memcmp(chars(fake_env, roundtrip, NULL), units, sizeof units));
  utf_release(fake_env, unicode, modified_chars);
  CHECK(!strcmp(utf_chars(fake_env, unicode, NULL), modified));
  copied = 99;
  const uint16_t *pinned = critical(fake_env, unicode, &copied);
  CHECK(pinned == text && copied == 0);
  release_critical(fake_env, unicode, pinned);
  CHECK(!memcmp(chars(fake_env, unicode, NULL), units, sizeof units));

  uint16_t slice[6] = {0xaaaa, 0, 0, 0, 0, 0xbbbb};
  region(fake_env, unicode, 1, 4, slice + 1);
  CHECK(slice[0] == 0xaaaa && slice[5] == 0xbbbb);
  CHECK(!memcmp(slice + 1, units + 1, 4 * sizeof(uint16_t)));
  uint16_t guard = 0x1234;
  region(fake_env, unicode, -1, 1, &guard);
  CHECK(guard == 0x1234);
  region(fake_env, unicode, 7, 1, &guard);
  CHECK(guard == 0x1234);
  region(fake_env, unicode, 1, 0x7fffffff, &guard);
  CHECK(guard == 0x1234);
  region(fake_env, unicode, 7, 0, &guard);
  CHECK(guard == 0x1234);

  unsigned char encoded[8];
  memset(encoded, 0xaa, sizeof encoded);
  utf_region(fake_env, unicode, 3, 2, (char *)encoded + 1);
  CHECK(encoded[0] == 0xaa && encoded[7] == 0xaa);
  CHECK(!memcmp(encoded + 1, "\xed\xa0\xbd\xed\xb8\x80", 6));
  memset(encoded, 0xaa, sizeof encoded);
  utf_region(fake_env, unicode, 5, 1, (char *)encoded + 1);
  CHECK(encoded[0] == 0xaa && encoded[3] == 0xaa);
  CHECK(encoded[1] == 0xc0 && encoded[2] == 0x80);
  utf_region(fake_env, unicode, -1, 1, (char *)encoded);
  CHECK(encoded[0] == 0xaa);

  /* Native callbacks use standard UTF-8, while JNI returns modified UTF-8. */
  const char native[] = "A\xc3\xa9\xed\x95\x9c\xf0\x9f\x98\x80";
  void *native_string = jni_make_string(native);
  CHECK(length(fake_env, native_string) == 5);
  CHECK(!memcmp(chars(fake_env, native_string, NULL), units, 5 * sizeof(uint16_t)));
  CHECK(!strcmp(jni_string_utf(native_string), native));
  CHECK(utf_length(fake_env, native_string) == 12);

  const uint16_t unpaired[] = {0xd800, 'x', 0xdc00};
  void *surrogates = new_string(fake_env, unpaired, 3);
  CHECK(!memcmp(chars(fake_env, surrogates, NULL), unpaired, sizeof unpaired));
  CHECK(!strcmp(utf_chars(fake_env, surrogates, NULL), "\xed\xa0\x80x\xed\xb0\x80"));
  CHECK(!strcmp(jni_string_utf(surrogates), "\xef\xbf\xbdx\xef\xbf\xbd"));
  void *truncated = jni_make_string("\xe2");
  CHECK(length(fake_env, truncated) == 1);
  CHECK(chars(fake_env, truncated, NULL)[0] == 0xfffd);
  CHECK(length(fake_env, NULL) == 0);
  CHECK(chars(fake_env, NULL, NULL) != NULL);

  /* Cover all 16-bit code units, including embedded NUL and unpaired halves. */
  uint16_t *all = malloc(65536 * sizeof(*all));
  CHECK(all != NULL);
  for (unsigned i = 0; i < 65536; i++) all[i] = (uint16_t)i;
  void *all_string = new_string(fake_env, all, 65536);
  CHECK(all_string != NULL && length(fake_env, all_string) == 65536);
  const char *all_utf = utf_chars(fake_env, all_string, NULL);
  CHECK((int)strlen(all_utf) == utf_length(fake_env, all_string));
  void *all_roundtrip = new_utf(fake_env, all_utf);
  CHECK(length(fake_env, all_roundtrip) == 65536);
  CHECK(!memcmp(chars(fake_env, all_roundtrip, NULL), all, 65536 * sizeof(*all)));
  free(all);
  printf("PASS: JNI boot path and strings (%d checks)\n", checks);
  return 0;
}
