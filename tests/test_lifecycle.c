#include <stdio.h>
#include <string.h>
typedef enum { AppletHookType_OnFocusState=1,AppletHookType_Other=2 } AppletHookType;
enum { AppletFocusState_InFocus=1 };
static int focus=1,s_app_focused=1,checks,saves,renders;
static char order[32];static int order_count;
static void *fake_env;
static void record(char event) { if(order_count<31) order[order_count++]=event; }
static void pause_game(void *env,void *object) { (void)env;(void)object;record('P'); }
static void resume_game(void *env,void *object) { (void)env;(void)object;record('R'); }
static void *shared_state(void) { return &focus; }
static void quick_save(void *game) { (void)game;saves++;record('S'); }
static void render(void *env,void *object) { (void)env;(void)object;renders++; }
static int appletGetFocusState(void) { return focus; }
static void update_input(void) { }
static void cursor_render(void) { }
static int s_dpy,s_surf;
static void eglSwapBuffers(int display,int surface) { (void)display;(void)surface; }
static struct {
  void *(*gmSharedState)(void);
  void (*gmDoQuickSave)(void *);
  void (*onPause)(void *,void *),(*onResume)(void *,void *),(*render)(void *,void *);
} gd={shared_state,quick_save,pause_game,resume_game,render};
#define CHECK(condition) do { checks++;if (!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while (0)
/* PRODUCTION_LIFECYCLE */
static void render_frames(void) {
  /* Keep the old counter in the fixture so a timer reintroduced in the
   * production loop will compile and fail the zero-save assertion. */
  unsigned frame=0;(void)frame;
  for(unsigned i=0;i<2400;i++) {
    /* PRODUCTION_RENDER_LOOP */
  }
}
int main(void) {
  render_frames();CHECK(renders==2400 && saves==0);
  applet_focus_hook(AppletHookType_Other,NULL);CHECK(saves==0);
  focus=0;applet_focus_hook(AppletHookType_OnFocusState,NULL);
  CHECK(saves==1 && !s_app_focused && !strcmp(order,"PS"));
  applet_focus_hook(AppletHookType_OnFocusState,NULL);CHECK(saves==1);
  focus=1;applet_focus_hook(AppletHookType_OnFocusState,NULL);
  CHECK(s_app_focused && !strcmp(order,"PSR"));
  applet_focus_hook(AppletHookType_OnFocusState,NULL);CHECK(order_count==3);
  printf("PASS: render-loop and focus saves (%d checks)\n",checks);
  return 0;
}
