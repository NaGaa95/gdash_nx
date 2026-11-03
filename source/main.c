/* main.c
 *
 * Geometry Dash (Android, arm64) on the Switch.
 *
 * The game is RobTop's cocos2d-x 2.2 fork inside libcocos2dcpp.so plus the
 * stock Android libfmod.so; both are ELF aarch64 and load natively through
 * so_util (libfmod first, so the game's FMOD imports cross-resolve from it).
 * The Android Java layer is replaced by jni_fake.c; the GLSurfaceView render
 * thread is this main thread: bring up EGL, call nativeSetApkPath +
 * nativeInit(w, h), then loop input dispatch -> nativeRender -> swap.
 *
 * The game reads every asset from the .apk itself (its own minizip against
 * the path from nativeSetApkPath); FMOD reads music/sfx as loose files under
 * /switch/gdash/assets/ via the createSound/createStream path rewrite.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "saltynx.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "game_compat.h"
#include "paths.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "pthr.h"
#include "net_shim.h"
#include "prefs.h"
#include "save_manager.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module fmod_mod; // libfmod.so   (loaded first: exports feed the game)
so_module game_mod; // libcocos2dcpp.so

static volatile int g_quit = 0;

// ---------------------------------------------------------------------------
// heap split: newlib heap + .so load region (verbatim from the lbbg port)
// ---------------------------------------------------------------------------

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(path_so_game(), &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from the APK's lib/arm64-v8a/\nnext to the .nro.", SO_NAME);
  if (stat(path_so_fmod(), &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from the APK's lib/arm64-v8a/\nnext to the .nro.", FMOD_SO_NAME);
  if (stat(path_assets(), &st) < 0)
    fatal_error("Could not find the assets folder.\nExtract the APK's assets/ into\n%s/.", path_assets());
  // the assets folder must be the FULL apk assets/, not just the audio files
  char probe[600];
  snprintf(probe, sizeof(probe), "%s/GJ_GameSheet.plist", path_assets());
  if (stat(probe, &st) < 0)
    fatal_error("The assets folder is incomplete.\n\nExtract the APK's ENTIRE assets/ folder\n"
                "(sprites, fonts, plists -- not just audio)\ninto\n%s/.", path_assets());
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// The game opens its save files for reading before ever creating them; make
// sure they exist (empty is fine, the game treats empty as "new"). Ported
// from the Vita port's save_files_init().
static void save_files_init(void) {
  static const char *names[] = {
    "CCGameManager.dat",
    "CCGameManager2.dat",
    "CCGameManager.dat.bak",
    "CCLocalLevels.dat",
    "CCLocalLevels.dat.bak",
    "CCLocalLevels2.dat",
  };
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", path_save(), names[i]);
    FILE *f = fopen(path, "r");
    if (f) {
      fclose(f);
      continue;
    }
    f = fopen(path, "w");
    if (f)
      fclose(f);
  }
}

// ---------------------------------------------------------------------------
// cocos2d-x JNI entry points (resolved from libcocos2dcpp.so by name)
// ---------------------------------------------------------------------------

// 2.2 touch natives carry a trailing jdouble timestamp in SECONDS (the Java
// side passes MotionEvent.getEventTime()/1000.0) -- confirmed from the dex
// prototypes (IFFD)V / ([I[F[FD)V and the trailing scvtf/q0 use in the
// disassembly. Passing garbage there breaks the 2.2 input-precision path.
static struct {
  void *(*ccFileUtils)(void);                       // CCFileUtils::sharedFileUtils()
  void (*ccAddSearchPath)(void *self, const char *); // ::addSearchPath(const char*)
  void *(*gmSharedState)(void);                     // GameManager::sharedState()
  void (*gmDoQuickSave)(void *self);                // original working save entry
  void *(*localSharedState)(void);
  int (*managerSave)(void *self, int background);
  void (*managerSaveLegacy)(void *self);
  void (*managerWaitForSave)(void *self, double timeout);
  void *(*ccDirector)(void);
  void (*init)(void *env, void *thiz, int w, int h);
  void (*render)(void *env, void *thiz);
  void (*onPause)(void *env, void *thiz);
  void (*onResume)(void *env, void *thiz);
  void (*touchesBegin)(void *env, void *thiz, int id, float x, float y, double ts);
  void (*touchesEnd)(void *env, void *thiz, int id, float x, float y, double ts);
  void (*touchesMove)(void *env, void *thiz, void *ids, void *xs, void *ys, double ts);
  void (*touchesCancel)(void *env, void *thiz, void *ids, void *xs, void *ys, double ts);
  unsigned char (*keyDown)(void *env, void *thiz, int keycode);
  void (*insertText)(void *env, void *thiz, void *jstr);
  void (*deleteBackward)(void *env, void *thiz);
  void *(*getContentText)(void *env, void *thiz);
  void (*setEditTextDialogResult)(void *env, void *thiz, void *jbytearr);
} gd;

static int (*fmod_JNI_OnLoad)(void *vm, void *reserved);
static int (*game_JNI_OnLoad)(void *vm, void *reserved);

#define RESOLVE(field, sym) \
  gd.field = (void *)so_find_addr_rx(&game_mod, sym)
#define RESOLVE_OPT(field, sym) \
  gd.field = (void *)so_try_find_addr_rx(&game_mod, sym)

static void resolve_gd_exports(void) {
  RESOLVE(ccFileUtils,     "_ZN7cocos2d11CCFileUtils15sharedFileUtilsEv");
  RESOLVE(ccAddSearchPath, "_ZN7cocos2d11CCFileUtils13addSearchPathEPKc");
  RESOLVE(gmSharedState, "_ZN11GameManager11sharedStateEv");
  RESOLVE(gmDoQuickSave, "_ZN11GameManager11doQuickSaveEv");
  RESOLVE(localSharedState, "_ZN17LocalLevelManager11sharedStateEv");
  RESOLVE_OPT(managerSave, "_ZN8GManager4saveEb");
  RESOLVE_OPT(managerSaveLegacy, "_ZN8GManager4saveEv");
  RESOLVE_OPT(managerWaitForSave, "_ZN8GManager11waitForSaveEd");
  RESOLVE(ccDirector, "_ZN7cocos2d10CCDirector14sharedDirectorEv");
  RESOLVE(init,          "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  RESOLVE(render,        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  RESOLVE(onPause,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  RESOLVE(onResume,      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  RESOLVE(touchesBegin,  "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  RESOLVE(touchesEnd,    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  RESOLVE(touchesMove,   "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  RESOLVE_OPT(touchesCancel, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
  RESOLVE(keyDown,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
  RESOLVE_OPT(insertText,     "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
  RESOLVE_OPT(deleteBackward, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
  RESOLVE_OPT(getContentText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText");
  RESOLVE_OPT(setEditTextDialogResult,
              "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");

  fmod_JNI_OnLoad = (void *)so_try_find_addr_rx(&fmod_mod, "JNI_OnLoad");
  game_JNI_OnLoad = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
}

// ---------------------------------------------------------------------------
// EGL (plain: the game imports no EGL at all, GL binds straight to mesa)
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

static void init_egl(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY)
    fatal_error("eglGetDisplay failed.");
  eglInitialize(s_dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE,
  };
  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &ncfg);
  if (ncfg < 1)
    fatal_error("No usable EGL config.");

  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)nw, NULL);
  if (s_surf == EGL_NO_SURFACE)
    fatal_error("eglCreateWindowSurface failed.");

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT)
    fatal_error("eglCreateContext failed.");

  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  eglSwapInterval(s_dpy, 1); // 60 fps
  glViewport(0, 0, screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// software keyboard (JNI callbacks from jni_fake.c land here)
// ---------------------------------------------------------------------------

static int swkbd_get_text(const char *header, const char *initial,
                          char *out, size_t out_len) {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return 0;
  swkbdConfigMakePresetDefault(&kbd);
  if (header && header[0])
    swkbdConfigSetHeaderText(&kbd, header);
  if (initial && initial[0])
    swkbdConfigSetInitialText(&kbd, initial);
  Result rc = swkbdShow(&kbd, out, out_len);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc);
}

void gd_open_ime_keyboard(void) {
  if (!gd.insertText)
    return;
  char initial[256] = "";
  if (gd.getContentText) {
    void *jstr = gd.getContentText(fake_env, NULL);
    if (jstr)
      snprintf(initial, sizeof(initial), "%s", jni_string_utf(jstr));
  }

  char out[256] = "";
  if (!swkbd_get_text("", initial, out, sizeof(out)))
    return;

  // replace the current content: clear it, then insert the new text and a
  // newline (cocos2d text fields treat '\n' as end-of-editing)
  if (gd.deleteBackward) {
    for (size_t i = strlen(initial); i > 0; i--)
      gd.deleteBackward(fake_env, NULL);
  }
  gd.insertText(fake_env, NULL, jni_make_string(out));
  gd.insertText(fake_env, NULL, jni_make_string("\n"));
}

void gd_show_edittext_dialog(const char *title, const char *msg, int maxlen) {
  (void)maxlen;
  if (!gd.setEditTextDialogResult)
    return;
  char out[512] = "";
  if (!swkbd_get_text(title, msg, out, sizeof(out)))
    snprintf(out, sizeof(out), "%s", msg ? msg : "");
  gd.setEditTextDialogResult(fake_env, NULL, jni_make_byte_array((int)strlen(out), out));
}

void gd_request_quit(void) {
  g_quit = 1;
}

// ---------------------------------------------------------------------------
// input pump: touchscreen passthrough + gamepad-to-touch synthesis + cursor
// ---------------------------------------------------------------------------

#define MAX_POINTERS 24 // 0..15 real fingers, 20+ virtual (buttons, cursor)

typedef struct { int active; float x, y; } Pointer;
static Pointer pcur[MAX_POINTERS]; // committed state
static Pointer pnew[MAX_POINTERS]; // desired state this frame

static PadState pad;

// virtual pointer ids
enum {
  VPTR_JUMP   = 20, // shoulder buttons -> jump tap at bottom-right
  VPTR_LEFT   = 21, // dpad/stick left  -> platformer left arrow zone
  VPTR_RIGHT  = 22, // dpad/stick right -> platformer right arrow zone
  VPTR_CURSOR = 23, // A press at the cursor, including when it is hidden
};

// Android keycodes
#define AKEY_BACK 4

// stick-driven cursor for menu navigation (essential when docked)
static float cursor_x, cursor_y;
static u64 cursor_last_move_tick;

static void cursor_update(void) {
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  // either stick moves the cursor; right stick has priority
  float dx = (float)rs.x / 32767.0f, dy = (float)rs.y / 32767.0f;
  if (fabsf(dx) < 0.25f && fabsf(dy) < 0.25f) {
    dx = (float)ls.x / 32767.0f;
    dy = (float)ls.y / 32767.0f;
  }
  if (fabsf(dx) < 0.25f) dx = 0.f;
  if (fabsf(dy) < 0.25f) dy = 0.f;
  if (dx != 0.f || dy != 0.f) {
    const float speed = (float)config.cursor_speed * ((float)screen_height / 720.0f);
    cursor_x += dx * speed / 60.0f;
    cursor_y -= dy * speed / 60.0f; // stick up = cursor up (screen y down)
    if (cursor_x < 0.f) cursor_x = 0.f;
    if (cursor_y < 0.f) cursor_y = 0.f;
    if (cursor_x > (float)screen_width - 1.f)  cursor_x = (float)screen_width - 1.f;
    if (cursor_y > (float)screen_height - 1.f) cursor_y = (float)screen_height - 1.f;
    cursor_last_move_tick = armGetSystemTick();
  }
}

static int cursor_visible(void) {
  if (!cursor_last_move_tick)
    return 0;
  return (armGetSystemTick() - cursor_last_move_tick) <
         armGetSystemTickFreq() * 3; // 3 s after the last stick motion
}

// scissor-clear rectangle painter (no shaders, state saved/restored)
static void draw_rect(int x, int y, int w, int h, float r, float g, float b) {
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void cursor_render(void) {
  if (!cursor_visible())
    return;
  GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLint old_box[4];
  GLfloat old_clear[4];
  glGetIntegerv(GL_SCISSOR_BOX, old_box);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  glEnable(GL_SCISSOR_TEST);

  // GL window coords: origin bottom-left; cursor_y is top-left based
  const int cx = (int)cursor_x;
  const int cy = screen_height - 1 - (int)cursor_y;
  const int s = screen_height / 90; // ~8 px at 720p
  draw_rect(cx - s / 2 - 1, cy - s / 2 - 1, s + 2, s + 2, 0.f, 0.f, 0.f);
  draw_rect(cx - s / 2, cy - s / 2, s, s, 1.f, 1.f, 1.f);

  glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  if (!had_scissor)
    glDisable(GL_SCISSOR_TEST);
}

static void build_touch_pointers(void) {
  HidTouchScreenState st = { 0 };
  if (!hidGetTouchScreenStates(&st, 1))
    return;
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  for (int i = 0; i < (int)st.count; i++) {
    const int id = (int)st.touches[i].finger_id;
    if (id < 0 || id >= 16)
      continue;
    pnew[id].active = 1;
    pnew[id].x = (float)st.touches[i].x * sx;
    pnew[id].y = (float)st.touches[i].y * sy;
  }
}

static void build_virtual_pointers(void) {
  const u64 down = padGetButtons(&pad);
  const float w = (float)screen_width, h = (float)screen_height;

  // Keep dedicated jump buttons separate from A's cursor click. A must never
  // also touch this corner: it contains the exit door in custom-level menus.
  if (down & (HidNpadButton_ZR | HidNpadButton_R |
              HidNpadButton_ZL | HidNpadButton_L)) {
    pnew[VPTR_JUMP].active = 1;
    pnew[VPTR_JUMP].x = w - 4.0f;
    pnew[VPTR_JUMP].y = h - 4.0f;
  }

  // platformer move zones (positions ported from the Vita build: 95/225 x 480
  // in 960x544 view space)
  if (down & HidNpadButton_Left) {
    pnew[VPTR_LEFT].active = 1;
    pnew[VPTR_LEFT].x = (95.0f / 960.0f) * w;
    pnew[VPTR_LEFT].y = (480.0f / 544.0f) * h;
  }
  if (down & HidNpadButton_Right) {
    pnew[VPTR_RIGHT].active = 1;
    pnew[VPTR_RIGHT].x = (225.0f / 960.0f) * w;
    pnew[VPTR_RIGHT].y = (480.0f / 544.0f) * h;
  }

  // Hiding the cursor is visual only. Keep the same contact until A is
  // released, even if the three-second visibility timer expires while held.
  if (down & HidNpadButton_A) {
    pnew[VPTR_CURSOR].active = 1;
    pnew[VPTR_CURSOR].x = cursor_x;
    pnew[VPTR_CURSOR].y = cursor_y;
  }
}

// monotonic seconds for the touch timestamps (same clock across all events)
static double now_seconds(void) {
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq();
  return (double)armGetSystemTick() / (double)freq;
}

// translate the per-frame pointer delta into the cocos touch protocol
static void dispatch_pointers(void) {
  // reusable JNI arrays for the move batch
  static void *move_ids, *move_xs, *move_ys;
  if (!move_ids) {
    move_ids = jni_make_int_array(MAX_POINTERS, NULL);
    move_xs = jni_make_float_array(MAX_POINTERS, NULL);
    move_ys = jni_make_float_array(MAX_POINTERS, NULL);
  }

  const double ts = now_seconds();

  // downs
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (pnew[i].active && !pcur[i].active)
      gd.touchesBegin(fake_env, NULL, i, pnew[i].x, pnew[i].y, ts);
  }

  // moves, batched like the Java GLSurfaceView does
  int nmove = 0;
  int ids[MAX_POINTERS];
  float xs[MAX_POINTERS], ys[MAX_POINTERS];
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (pnew[i].active && pcur[i].active &&
        (pnew[i].x != pcur[i].x || pnew[i].y != pcur[i].y)) {
      ids[nmove] = i;
      xs[nmove] = pnew[i].x;
      ys[nmove] = pnew[i].y;
      nmove++;
    }
  }
  if (nmove > 0) {
    jni_prim_array_fill(move_ids, ids, nmove, sizeof(int));
    jni_prim_array_fill(move_xs, xs, nmove, sizeof(float));
    jni_prim_array_fill(move_ys, ys, nmove, sizeof(float));
    gd.touchesMove(fake_env, NULL, move_ids, move_xs, move_ys, ts);
  }

  // ups
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (!pnew[i].active && pcur[i].active)
      gd.touchesEnd(fake_env, NULL, i, pcur[i].x, pcur[i].y, ts);
  }

  memcpy(pcur, pnew, sizeof(pcur));
}

static void update_input(void) {
  padUpdate(&pad);

  cursor_update();

  memset(pnew, 0, sizeof(pnew));
  build_touch_pointers();
  build_virtual_pointers();
  dispatch_pointers();

  // BACK (pause / dismiss): B or Plus, edge-triggered
  const u64 pressed = padGetButtonsDown(&pad);
  if ((pressed & (HidNpadButton_B | HidNpadButton_Plus)) && !g_block_back_button)
    gd.keyDown(fake_env, NULL, AKEY_BACK);
}

static AppletHookCookie s_applet_hook;
static int s_app_focused = 1;
static int s_play_layer_offset = -1;
static int s_running_scene_offset = -1;
static int s_menu_ready;

static uint64_t save_clock_ms(void) {
  return armGetSystemTick() / (armGetSystemTickFreq() / 1000);
}

static int menu_save_state(void) {
  if (!gd.gmSharedState || !gd.ccDirector || s_play_layer_offset < 0 || s_running_scene_offset < 0)
    return 0;
  void *director = gd.ccDirector(), *scene = NULL;
  if (!director)
    return 0;
  memcpy(&scene, (const char *)director + s_running_scene_offset, sizeof scene);
  if (!scene)
    return 0;
  int tag = -1;
  memcpy(&tag, (const char *)scene + 28, sizeof tag);
  if (tag == 15) // MenuLayer::scene: game loading has finished
    s_menu_ready = 1;
  if (!s_menu_ready || tag < 0)
    return 0;
  if (tag == 5 || tag == 6) // PlayLayer / LevelEditorLayer, including pause overlays
    return -1;
  void *manager = gd.gmSharedState();
  if (!manager)
    return 0;
  void *play = NULL;
  memcpy(&play, (const char *)manager + s_play_layer_offset, sizeof play);
  return play ? -1 : 1;
}

static void save_game_state(void *manager) {
  // Restore the same GameManager::doQuickSave path used by the working
  // periodic saver. Wait before and after so a previous writer cannot make
  // this snapshot get skipped, and the menu checkpoint is durable on return.
  if (gd.managerWaitForSave)
    gd.managerWaitForSave(manager, 30.0);
  gd.gmDoQuickSave(manager);
  if (gd.managerWaitForSave)
    gd.managerWaitForSave(manager, 30.0);
  void *local = gd.localSharedState ? gd.localSharedState() : NULL;
  if (local) {
    if (gd.managerWaitForSave)
      gd.managerWaitForSave(local, 30.0);
    if (gd.managerSave)
      gd.managerSave(local, 1);
    else if (gd.managerSaveLegacy)
      gd.managerSaveLegacy(local);
    if (gd.managerWaitForSave)
      gd.managerWaitForSave(local, 30.0);
  }
}

static int save_commit_storage(void) {
  return (int)fsdevCommitDevice("sdmc");
}

// Best-effort pause/resume notifications. HOME is not a save trigger.
static void applet_focus_hook(AppletHookType type, void *param) {
  (void)param;
  if (type != AppletHookType_OnFocusState)
    return;
  const int focused = (appletGetFocusState() == AppletFocusState_InFocus);
  if (focused && !s_app_focused) {
    s_app_focused = 1;
    gd.onResume(fake_env, NULL);
  } else if (!focused && s_app_focused) {
    s_app_focused = 0;
    gd.onPause(fake_env, NULL);
  }
}

static void save_lifecycle_init(void) {
  SaveBackend backend = {
    gd.gmSharedState, save_game_state, menu_save_state, save_clock_ms,
    save_commit_storage
  };
  unsigned interval = config.menu_autosave_interval_seconds > 0 ?
      (unsigned)config.menu_autosave_interval_seconds : 0;
  save_manager_init(&backend, interval);
  appletHook(&s_applet_hook, applet_focus_hook, NULL);
}

int main(int argc, char **argv) {
  saltynx_start();
  atexit(saltynx_stop);
  pthr_pin_render_core();
  cpu_boost(1);

  paths_init(argc > 0 ? argv[0] : NULL);

  if (read_config(path_config()) < 0)
    write_config(path_config());

  check_syscalls();
  check_data();
  game_compat_detect_package(path_so_game());
  set_screen_size(config.screen_width, config.screen_height);

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));

  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "this port from there.", heap_mb);

  mkdir(path_save(), 0777);
  save_files_init();
  prefs_load(path_prefs());
  net_init();

  // libfmod.so first: the game's FMOD imports resolve from its exports
  const size_t fmod_reserve = 16 * 1024 * 1024;
  if (so_load(&fmod_mod, path_so_fmod(), heap_so_base, fmod_reserve) < 0)
    fatal_error("Could not load\n%s.", FMOD_SO_NAME);
  if (so_load(&game_mod, path_so_game(), (char *)heap_so_base + fmod_reserve,
              heap_so_limit - fmod_reserve) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();

  so_relocate(&fmod_mod);
  so_resolve(&fmod_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  game_compat_apply_network(&game_mod);
  s_play_layer_offset = game_compat_play_layer_offset(&game_mod);
  s_running_scene_offset = game_compat_running_scene_offset(&game_mod);
  fmod_hooks_init(&fmod_mod);
  resolve_gd_exports();

  // OPENSSL_cpuid_setup (a static ctor) probes the CPU with a SIGILL/longjmp
  // harness that faults without signal delivery; neutralize it -> portable C.
  {
    uintptr_t cpuid = so_try_find_addr(&game_mod, "OPENSSL_cpuid_setup");
    if (cpuid)
      hook_arm64(cpuid, (uintptr_t)&ret0);
  }

  // Keep the loose-assets search path alive: purgeFileUtils() (on a texture-
  // quality change) would drop it and the game never re-adds it.
  {
    uintptr_t purge = so_try_find_addr(&game_mod, "_ZN7cocos2d11CCFileUtils14purgeFileUtilsEv");
    if (purge)
      hook_arm64(purge, (uintptr_t)&ret0);
  }

  so_finalize(&fmod_mod);
  so_flush_caches(&fmod_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // fake TLS must exist before any game code runs (static ctors read the
  // stack-guard cookie); init_array below is game code
  pthr_install_fake_tls();
  so_execute_init_array(&fmod_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&fmod_mod);
  so_free_temp(&game_mod);

  jni_init();

  if (fmod_JNI_OnLoad)
    fmod_JNI_OnLoad(fake_vm, NULL);
  if (game_JNI_OnLoad)
    game_JNI_OnLoad(fake_vm, NULL);

  // load assets loose from <base>/assets/: cocos2d fopen()s an absolute
  // ('/'-rooted) search path directly, so no .apk is needed
  gd.ccAddSearchPath(gd.ccFileUtils(), path_assets_search());

  // input before first frame
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  cursor_x = screen_width / 2.0f;
  cursor_y = screen_height / 2.0f;

  // GL up, then boot the engine on this thread (context current)
  init_egl();
  gd.init(fake_env, NULL, screen_width, screen_height);

  cpu_boost(0);

  save_lifecycle_init();

  // Checkpoint only in non-gameplay menus. Do it before presentation, which
  // can block when HOME opens, preventing the next applet-message poll.
  while (appletMainLoop() && !g_quit) {
    if (!s_app_focused) {
      svcSleepThread(16000000);
      continue;
    }
    update_input();
    gd.render(fake_env, NULL);
    cursor_render();
    save_manager_poll();
    eglSwapBuffers(s_dpy, s_surf);
  }

  if (s_app_focused) { // best-effort clean in-game quit
    gd.onPause(fake_env, NULL);
    save_manager_force();
  }

  eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(s_dpy, s_ctx);
  eglDestroySurface(s_dpy, s_surf);
  eglTerminate(s_dpy);
  net_exit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
