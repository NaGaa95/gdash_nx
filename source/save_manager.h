#ifndef SAVE_MANAGER_H
#define SAVE_MANAGER_H

#include <stdint.h>

typedef struct {
  void *(*get_state)(void);
  void (*save_state)(void *state);
  int (*menu_state)(void); // 1: menu, 0: loading/transition/unknown, -1: level/editor
  uint64_t (*now_ms)(void);
  int (*commit_storage)(void);
} SaveBackend;

void save_manager_init(const SaveBackend *backend, unsigned interval_seconds);
void save_manager_poll(void);
void save_manager_force(void);

#endif
