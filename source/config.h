/* config.h -- global configuration and config file handling.
 *
 * Geometry Dash and its free variants (Android 2.2.14x arm64) on Switch:
 * RobTop's cocos2d-x 2.2 fork in libcocos2dcpp.so plus the stock arm64
 * libfmod.so, both loaded natively. File paths are resolved at runtime from
 * the .nro location (see paths.h).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The .so images get this reserve; the newlib heap gets the rest of the heap.
#define SO_REGION_MB 96

#define SO_NAME      "libcocos2dcpp.so"
#define FMOD_SO_NAME "libfmod.so"
#define CONFIG_NAME  "config.txt"

// android.os.Build.VERSION.SDK_INT reported through the fake JNI.
#define ANDROID_SDK_INT 21

// The game asks FMOD for "file:///android_asset/<rel>"; the createSound/
// createStream wrappers rewrite that prefix to the assets dir.
#define ANDROID_ASSET_URI     "file:///android_asset/"
#define ANDROID_ASSET_URI_LEN 22

// Fallback when the package marker cannot be detected in libcocos2dcpp.so.
#define GD_PACKAGE_NAME "com.robtopx.geometryjump"

// actual render/surface size (picked at runtime from docked state)
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int cursor_speed;    // pointer speed for the stick-driven cursor, px/s at 720p
  int menu_autosave_interval_seconds; // 0 disables menu autosave; default 30
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

// two-letter language code for Cocos2dxHelper.getCurrentLanguage ("en", ...)
const char *config_lang_iso2(void);

#endif
