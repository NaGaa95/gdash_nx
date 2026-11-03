#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef int PadState;
typedef struct { int active;float x,y; } Pointer;
enum { MAX_POINTERS=24,VPTR_JUMP=20,VPTR_LEFT=21,VPTR_RIGHT=22,VPTR_CURSOR=23 };
enum { HidNpadButton_A=1,HidNpadButton_ZR=2,HidNpadButton_R=4,
       HidNpadButton_ZL=8,HidNpadButton_L=16,HidNpadButton_Left=32,HidNpadButton_Right=64 };
static Pointer pnew[MAX_POINTERS];
static PadState pad;
static u64 buttons,tick=100,frequency=1000,cursor_last_move_tick=100;
static float cursor_x=321,cursor_y=234;
static int screen_width=1280,screen_height=720,checks;
static u64 padGetButtons(PadState *state) { (void)state;return buttons; }
static u64 armGetSystemTick(void) { return tick; }
static u64 armGetSystemTickFreq(void) { return frequency; }
#define CHECK(condition) do { checks++;if (!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while (0)
/* PRODUCTION_INPUT */
static int contacts(void) { int count=0;for(int i=0;i<MAX_POINTERS;i++)count+=pnew[i].active;return count; }
static void update(u64 held) { buttons=held;memset(pnew,0,sizeof pnew);build_virtual_pointers(); }
int main(void) {
  CHECK(cursor_visible());
  update(HidNpadButton_A);
  CHECK(contacts()==1 && pnew[VPTR_CURSOR].active && !pnew[VPTR_JUMP].active);
  CHECK(pnew[VPTR_CURSOR].x==321 && pnew[VPTR_CURSOR].y==234);
  tick=5000;CHECK(!cursor_visible());
  update(HidNpadButton_A);
  CHECK(contacts()==1 && pnew[VPTR_CURSOR].active && !pnew[VPTR_JUMP].active);
  cursor_x=456;cursor_y=123;update(HidNpadButton_A);
  CHECK(pnew[VPTR_CURSOR].x==456 && pnew[VPTR_CURSOR].y==123);
  update(0);CHECK(contacts()==0);
  const u64 shoulders[]={HidNpadButton_L,HidNpadButton_R,HidNpadButton_ZL,HidNpadButton_ZR};
  for(unsigned i=0;i<sizeof shoulders/sizeof shoulders[0];i++) {
    update(shoulders[i]);CHECK(contacts()==1 && pnew[VPTR_JUMP].active);
    CHECK(pnew[VPTR_JUMP].x==1276 && pnew[VPTR_JUMP].y==716);
  }
  update(HidNpadButton_Left|HidNpadButton_A);
  CHECK(contacts()==2 && pnew[VPTR_LEFT].active && pnew[VPTR_CURSOR].active);
  update(HidNpadButton_Right|HidNpadButton_A);
  CHECK(contacts()==2 && pnew[VPTR_RIGHT].active && pnew[VPTR_CURSOR].active);
  cursor_last_move_tick=0;update(HidNpadButton_A);
  CHECK(contacts()==1 && pnew[VPTR_CURSOR].active);
  printf("PASS: controller contacts (%d checks)\n",checks);
  return 0;
}
