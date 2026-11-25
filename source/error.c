/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"
#include "paths.h"

void debug_log(const char *fmt, ...) {
  char path[600];
  snprintf(path, sizeof(path), "%s/debug.log", path_base());
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  va_list list;
  va_start(list, fmt);
  vfprintf(f, fmt, list);
  va_end(list);
  fclose(f);
}

void fatal_error(const char *fmt, ...) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  // keep a copy on the SD card: on hardware it is the easiest thing to share
  char msg[1024];
  va_start(list, fmt);
  vsnprintf(msg, sizeof(msg), fmt, list);
  va_end(list);
  debug_log("fatal: %s\n", msg);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
