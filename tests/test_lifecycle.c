#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "save_manager.h"
typedef enum { AppletHookType_OnFocusState=1,AppletHookType_Other=2 } AppletHookType;
enum { AppletFocusState_InFocus=1 };
typedef struct { int unused; } AppletHookCookie;
static AppletHookCookie s_applet_hook;
static int focus=1,s_app_focused=1,checks,saves,renders;
static int s_play_layer_offset=544,s_running_scene_offset=272,s_menu_ready;
static int progress=123,local_levels=456,committed,pending_game=-1,pending_local=-1;
static char order[64],base[1024],directory[1100];
static int order_count;
static uint64_t now;
static unsigned char manager_storage[600],director_storage[320],scene_storage[64];
static int local_manager,gameplay_layer;
static void *fake_env;
static void (*registered_hook)(AppletHookType,void *);
static void record(char event) { if(order_count<63)order[order_count++]=event; }
const char *path_base(void) { return base; }
const char *path_save(void) { return directory; }
static void pause_game(void *env,void *object) { (void)env;(void)object;record('P'); }
static void resume_game(void *env,void *object) { (void)env;(void)object;record('R'); }
static void write_value(const char *name,int value) {
  char path[1200];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *file=fopen(path,"wb");
  if(!file || fwrite(&value,sizeof value,1,file)!=1 || fclose(file))abort();
}
static int load(const char *name) {
  char path[1200];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *file=fopen(path,"rb");int value=-1;
  if(file) { if(fread(&value,sizeof value,1,file)!=1)value=-1;fclose(file); }return value;
}
static void wait_save(void *state,double timeout) {
  if(timeout!=30.0)abort();
  if(state==manager_storage) {
    record('W');if(pending_game>=0)write_value("CCGameManager.dat",pending_game);pending_game=-1;
  } else {
    record('w');if(pending_local>=0)write_value("CCLocalLevels.dat",pending_local);pending_local=-1;
  }
}
static void quick_save(void *state);
static int local_save(void *state,int background) {
  if(state!=&local_manager || !background || pending_local>=0)abort();
  record('L');pending_local=local_levels;return 1;
}
static void local_save_legacy(void *state) {
  if(state!=&local_manager)abort();record('l');write_value("CCLocalLevels.dat",local_levels);
}
static uint64_t save_clock_ms(void) { return now; }
static void *get_manager(void) { return manager_storage; }
static void *get_local(void) { return &local_manager; }
static void *get_director(void) { return director_storage; }
static int save_commit_storage(void) { committed++;return 0; }
Config config={.menu_autosave_interval_seconds=30};
static void render(void *env,void *object) { (void)env;(void)object;renders++; }
static int appletGetFocusState(void) { return focus; }
static void appletHook(AppletHookCookie *cookie,void (*callback)(AppletHookType,void *),void *arg) {
  (void)cookie;(void)arg;registered_hook=callback;
}
static void update_input(void) { }
static void cursor_render(void) { }
static int s_dpy,s_surf;
static void svcSleepThread(uint64_t ns) { (void)ns; }
static void eglSwapBuffers(int display,int surface) {
  (void)display;(void)surface;
  if(pending_game>=0 || pending_local>=0)abort();
}
static struct {
  void *(*gmSharedState)(void);
  void (*gmDoQuickSave)(void *);
  void *(*localSharedState)(void);
  int (*managerSave)(void *,int);
  void (*managerSaveLegacy)(void *);
  void (*managerWaitForSave)(void *,double);
  void *(*ccDirector)(void);
  void (*onPause)(void *,void *),(*onResume)(void *,void *),(*render)(void *,void *);
} gd={get_manager,quick_save,get_local,local_save,NULL,wait_save,get_director,pause_game,resume_game,render};
static void quick_save(void *state) {
  if(state!=manager_storage || pending_game>=0)abort();saves++;record('Q');
  if(gd.managerWaitForSave)pending_game=progress;
  else write_value("CCGameManager.dat",progress);
}
#define CHECK(condition) do { checks++;if(!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)
/* PRODUCTION_LIFECYCLE */
static void render_frames(void) {
  for(unsigned i=0;i<2400;i++) {
    /* PRODUCTION_RENDER_LOOP */
  }
}
static void emit_focus(int state) { focus=state;registered_hook(AppletHookType_OnFocusState,NULL); }
static void set_scene(int tag) {
  void *scene=scene_storage;memcpy(director_storage+s_running_scene_offset,&scene,sizeof scene);
  memcpy(scene_storage+28,&tag,sizeof tag);
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  snprintf(base,sizeof base,"%s",argv[1]);snprintf(directory,sizeof directory,"%s/save",base);
  const int play_layouts[]={472,544},scene_layouts[]={264,272};
  for(unsigned i=0;i<2;i++) {
    s_play_layer_offset=play_layouts[i];s_running_scene_offset=scene_layouts[i];s_menu_ready=0;
    memset(manager_storage,0,sizeof manager_storage);memset(director_storage,0,sizeof director_storage);
    CHECK(menu_save_state()==0);
    set_scene(-1);CHECK(menu_save_state()==0);
    set_scene(5);CHECK(menu_save_state()==0);
    set_scene(15);CHECK(menu_save_state()==1);
    set_scene(5);CHECK(menu_save_state()==-1);
    set_scene(6);CHECK(menu_save_state()==-1);
    set_scene(-1);CHECK(menu_save_state()==0);
    set_scene(7);CHECK(menu_save_state()==1);
    void *layer=&gameplay_layer;memcpy(manager_storage+s_play_layer_offset,&layer,sizeof layer);
    CHECK(menu_save_state()==-1);
    layer=NULL;memcpy(manager_storage+s_play_layer_offset,&layer,sizeof layer);CHECK(menu_save_state()==1);
  }
  s_play_layer_offset=-1;CHECK(menu_save_state()==0);s_play_layer_offset=544;
  s_running_scene_offset=-1;CHECK(menu_save_state()==0);s_running_scene_offset=272;
  gd.gmSharedState=NULL;CHECK(menu_save_state()==0);gd.gmSharedState=get_manager;
  gd.ccDirector=NULL;CHECK(menu_save_state()==0);gd.ccDirector=get_director;
  set_scene(-1);s_menu_ready=0;
  save_lifecycle_init();CHECK(registered_hook!=NULL);
  render_frames();CHECK(renders==2400 && saves==0);
  registered_hook(AppletHookType_Other,NULL);CHECK(saves==0);
  emit_focus(3);CHECK(saves==0 && !s_app_focused && !strcmp(order,"P") && committed==0);
  render_frames();CHECK(renders==2400);
  emit_focus(3);CHECK(order_count==1);
  emit_focus(1);CHECK(s_app_focused && !strcmp(order,"PR"));
  emit_focus(1);CHECK(order_count==2);
  memset(order,0,sizeof order);order_count=0;pending_game=11;pending_local=22;
  save_manager_force();CHECK(saves==1 && committed==1 && !strcmp(order,"WQWwLw"));
  CHECK(load("CCGameManager.dat")==123 && load("CCLocalLevels.dat")==456);
  CHECK(pending_game==-1 && pending_local==-1);
  memset(order,0,sizeof order);order_count=0;progress=789;local_levels=987;
  gd.managerSave=NULL;gd.managerSaveLegacy=local_save_legacy;gd.managerWaitForSave=NULL;
  save_manager_force();CHECK(saves==2 && committed==2 && !strcmp(order,"Ql"));
  CHECK(load("CCGameManager.dat")==789 && load("CCLocalLevels.dat")==987);
  set_scene(15);now=30000;progress=55;local_levels=66;render_frames();
  CHECK(saves==3 && committed==3 && renders==4800);
  CHECK(load("CCGameManager.dat")==55 && load("CCLocalLevels.dat")==66);
  printf("PASS: menu guards, native quick-save bridge and focus handling (%d checks)\n",checks);return 0;
}
