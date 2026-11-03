#include <stdio.h>
#include <string.h>
#include "config.h"
static int checks;
#define CHECK(condition) do { checks++;if(!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  CHECK(read_config(argv[1])<0);
  CHECK(config.menu_autosave_interval_seconds==30);
  CHECK(config.screen_width==-1 && config.screen_height==-1 && config.cursor_speed==900);
  FILE *file=fopen(argv[1],"w");CHECK(file!=NULL);
  fputs("screen_width 1280\nscreen_height 720\ncursor_speed 450\nautosave_interval_seconds 300\nsave_debug 1\n",file);CHECK(fclose(file)==0);
  CHECK(read_config(argv[1])==0);
  CHECK(config.screen_width==1280 && config.screen_height==720 && config.cursor_speed==450);
  CHECK(config.menu_autosave_interval_seconds==30);
  config.menu_autosave_interval_seconds=60;CHECK(write_config(argv[1])==0);
  CHECK(read_config(argv[1])==0 && config.menu_autosave_interval_seconds==60);
  char contents[512]={0};file=fopen(argv[1],"r");CHECK(file!=NULL);
  CHECK(fread(contents,1,sizeof contents-1,file)>0);CHECK(fclose(file)==0);
  CHECK(strstr(contents,"save_debug")==NULL);
  config.menu_autosave_interval_seconds=0;CHECK(write_config(argv[1])==0);
  CHECK(read_config(argv[1])==0 && config.menu_autosave_interval_seconds==0);
  printf("PASS: autosave configuration and legacy defaults (%d checks)\n",checks);return 0;
}
