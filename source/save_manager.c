#include "save_manager.h"

static SaveBackend s_backend;
static uint64_t s_last_save, s_interval;
static int s_ready;

void save_manager_init(const SaveBackend *backend, unsigned interval_seconds) {
  s_backend = *backend;
  s_ready = backend->get_state && backend->save_state && backend->now_ms;
  s_interval = (uint64_t)interval_seconds * 1000;
  s_last_save = s_ready ? backend->now_ms() : 0;
}

void save_manager_force(void) {
  if (!s_ready)
    return;
  void *state = s_backend.get_state();
  if (!state)
    return;
  s_backend.save_state(state);
  s_last_save = s_backend.now_ms();
  if (s_backend.commit_storage)
    s_backend.commit_storage();
}

void save_manager_poll(void) {
  if (!s_ready || !s_interval || !s_backend.menu_state)
    return;
  // Poll even before the deadline so startup's first fully loaded menu is
  // observed. Loading/transition scenes, levels and the editor defer saves.
  if (s_backend.menu_state() <= 0)
    return;
  if (s_backend.now_ms() - s_last_save >= s_interval)
    save_manager_force();
}
