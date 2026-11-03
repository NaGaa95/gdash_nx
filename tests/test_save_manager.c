#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "save_manager.h"

static char base[1024], directory[1100];
static uint64_t now;
static int menu, available=1, calls, commits, bad_commit, checks;
static int progress=11, levels=22;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while (0)
const char *path_base(void) { return base; }
const char *path_save(void) { return directory; }
static void file_path(char *out,size_t size,const char *name) { snprintf(out,size,"%s/%s",directory,name); }
static void write_value(const char *name,int value) {
  char path[1200];file_path(path,sizeof path,name);FILE *file=fopen(path,"wb");
  if(!file || fwrite(&value,sizeof value,1,file)!=1 || fclose(file))abort();
}
static int read_value(const char *name) {
  char path[1200];file_path(path,sizeof path,name);FILE *file=fopen(path,"rb");int value=-1;
  if(file) { if(fread(&value,sizeof value,1,file)!=1)value=-1;fclose(file); }return value;
}
static void *get_state(void) { return available?&progress:NULL; }
static void native_save(void *state) {
  (void)state;calls++;
  write_value("CCGameManager.dat",progress);write_value("CCLocalLevels.dat",levels);now+=5;
}
static int menu_state(void) { return menu; }
static uint64_t clock_ms(void) { return now; }
static int commit_storage(void) {
  commits++;
  if(read_value("CCGameManager.dat")!=progress || read_value("CCLocalLevels.dat")!=levels)bad_commit=1;
  return 0;
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  snprintf(base,sizeof base,"%s",argv[1]);snprintf(directory,sizeof directory,"%s/save",base);
  SaveBackend backend={get_state,native_save,menu_state,clock_ms,commit_storage};
  save_manager_init(&backend,30);
  menu=1;now=4000;save_manager_poll();CHECK(calls==0); // Never save at the first boot menu.
  now=29999;save_manager_poll();CHECK(calls==0);
  menu=0;now=100000;save_manager_poll();CHECK(calls==0);
  menu=1;save_manager_poll();CHECK(calls==1 && commits==1 && !bad_commit);
  CHECK(read_value("CCGameManager.dat")==11 && read_value("CCLocalLevels.dat")==22);
  uint64_t finished=now;
  menu=0;now+=100;save_manager_poll();menu=1;save_manager_poll();CHECK(calls==1);
  menu=-1;now=finished+30000;save_manager_poll();CHECK(calls==1);
  now+=100000;save_manager_poll();CHECK(calls==1);
  progress=33;levels=44;menu=0;save_manager_poll();CHECK(calls==1);
  menu=1;save_manager_poll();CHECK(calls==2 && commits==2 && !bad_commit);
  finished=now;now=finished+29999;save_manager_poll();CHECK(calls==2);
  now++;progress=55;levels=66;save_manager_poll();CHECK(calls==3 && commits==3);
  CHECK(read_value("CCGameManager.dat")==55 && read_value("CCLocalLevels.dat")==66);
  finished=now;menu=-1;now+=10;save_manager_poll();menu=1;save_manager_poll();CHECK(calls==3);
  now=finished+30000;save_manager_poll();CHECK(calls==4);
  menu=-1;progress=77;levels=88;save_manager_force();CHECK(calls==5 && commits==5 && !bad_commit);
  CHECK(read_value("CCGameManager.dat")==77 && read_value("CCLocalLevels.dat")==88);
  save_manager_init(&backend,0);
  menu=1;now+=1000000000;save_manager_poll();CHECK(calls==5);
  progress=99;levels=100;save_manager_force();CHECK(calls==6 && commits==6 && !bad_commit);
  save_manager_init(&backend,30);
  available=0;save_manager_force();CHECK(calls==6);
  available=1;
  menu=1;now+=30000;save_manager_poll();CHECK(calls==7 && commits==7);
  CHECK(read_value("CCGameManager.dat")==99 && read_value("CCLocalLevels.dat")==100);
  char path[1200];struct stat st;
  snprintf(path,sizeof path,"%s/save.log",base);CHECK(stat(path,&st)!=0);
  snprintf(path,sizeof path,"%s/save_previous.log",base);CHECK(stat(path,&st)!=0);
  printf("PASS: menu scheduling and persistence (%d checks)\n",checks);return 0;
}
