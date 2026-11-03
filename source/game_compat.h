/* game_compat.h -- Geometry Dash variant detection and optional patches. */

#ifndef GAME_COMPAT_H
#define GAME_COMPAT_H

#include "so_util.h"

// Detect the package-specific marker embedded in libcocos2dcpp.so. Unknown
// builds safely fall back to the paid game's package name.
void game_compat_detect_package(const char *so_path);
const char *game_compat_package_name(void);

// Turn the bundled curl client's CURLOPT_SSL_VERIFYPEER/VERIFYHOST values
// back on once a trusted CA bundle is available. Returns patched call sites.
int game_compat_apply_network(so_module *mod);

// Locate the PlayLayer field through the verified AppDelegate pause routine.
// Returns -1 for an unknown layout, so automatic saving can safely defer.
int game_compat_play_layer_offset(so_module *mod);
int game_compat_running_scene_offset(so_module *mod);

#endif
