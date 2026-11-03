#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_compat.h"

static int checks, tls_ready = 1, hook_count;
static uintptr_t decoder_hook;
#define CHECK(condition) do { checks++; if (!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition); return 1; \
} } while (0)

int net_tls_ca_ready(void) { return tls_ready; }
const char *path_ca_bundle(void) { return "/switch/test/cacert.pem"; }
void hook_arm64(uintptr_t address, uintptr_t replacement) {
  if (!address || !replacement) abort();
  decoder_hook = replacement; hook_count++;
}
uintptr_t so_try_find_addr(so_module *module, const char *name) {
  for (size_t i=0;i<module->symbol_count;i++) {
    TestSymbol *symbol=&module->symbols[i];
    if (symbol->section && !strcmp(module->strings+symbol->name,name))
      return (uintptr_t)module->load_base+symbol->value;
  }
  return 0;
}
static uint16_t read16(const unsigned char *p) { uint16_t n;memcpy(&n,p,2);return n; }
static uint64_t read64(const unsigned char *p) { uint64_t n;memcpy(&n,p,8);return n; }

static int load_module(const char *path, so_module *module) {
  memset(module,0,sizeof *module);
  FILE *file=fopen(path,"rb"); if (!file) return 0;
  fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
  module->file=malloc((size_t)size);
  if (!module->file || fread(module->file,1,(size_t)size,file)!=(size_t)size) abort();
  fclose(file);
  const unsigned char *data=module->file;
  if (memcmp(data,"\177ELF\002\001",6) || read16(data+18)!=183) abort();
  module->phnum=read16(data+56);
  if (module->phnum>16 || read16(data+54)!=sizeof(Elf64_Phdr)) abort();
  memcpy(module->phdr,data+read64(data+32),(size_t)module->phnum*sizeof(Elf64_Phdr));
  for (int i=0;i<module->phnum;i++) {
    Elf64_Phdr *p=&module->phdr[i];
    if (p->p_type==PT_LOAD && p->p_vaddr+p->p_memsz>module->load_size)
      module->load_size=p->p_vaddr+p->p_memsz;
  }
  module->load_base=calloc(1,module->load_size);
  if (!module->load_base) abort();
  for (int i=0;i<module->phnum;i++) {
    Elf64_Phdr *p=&module->phdr[i];
    if (p->p_type==PT_LOAD)
      memcpy((unsigned char *)module->load_base+p->p_vaddr,data+p->p_offset,p->p_filesz);
  }
  const TestSection *sections=(const TestSection *)(data+read64(data+40));
  if (read16(data+58)!=sizeof(TestSection)) abort();
  for (unsigned i=0;i<read16(data+60);i++) {
    if (sections[i].type==11) {
      module->symbols=(TestSymbol *)(module->file+sections[i].offset);
      module->symbol_count=sections[i].size/sizeof(TestSymbol);
      module->strings=(const char *)module->file+sections[sections[i].link].offset;
    }
  }
  return 1;
}
static void unload_module(so_module *module) { free(module->load_base);free(module->file); }
static int uses_jni_string_slot(so_module *module, unsigned slot) {
  const char *name = "_ZN7cocos2d9JniHelper14jstring2stringEP8_jstring";
  for (size_t i = 0; i < module->symbol_count; i++) {
    const TestSymbol *symbol = &module->symbols[i];
    if (!symbol->section || strcmp(module->strings + symbol->name, name))
      continue;
    const uint32_t *code = (const uint32_t *)((uintptr_t)module->load_base + symbol->value);
    for (size_t word = 0; word < symbol->size / sizeof(*code); word++) {
      /* AArch64 LDR Xt, [Xn, #slot*8], as used by this JNIEnv caller. */
      if ((code[word] & 0xffc00000u) == 0xf9400000u &&
          ((code[word] >> 10) & 0xfffu) == slot)
        return 1;
    }
  }
  return 0;
}
static uint32_t *leaderboard_branch(so_module *module) {
  uint32_t *code=(uint32_t *)so_try_find_addr(module,
      "_ZN17LeaderboardsLayer17selectLeaderboardE15LeaderboardType15LeaderboardStat");
  if (!code) return NULL;
  for (unsigned i=1;i<95;i++)
    if ((code[i]==0x3948a000u || code[i]==0x394a4000u) &&
        (code[i+1]&0x7f00001fu)==0x34000000u) return &code[i+1];
  return NULL;
}
static int count_verified_peers(so_module *module) {
  int count=0;
  for (int segment=0;segment<module->phnum;segment++) {
    Elf64_Phdr *p=&module->phdr[segment];
    if (p->p_type!=PT_LOAD || !(p->p_flags&PF_X)) continue;
    uint32_t *code=(uint32_t *)((uintptr_t)module->load_base+p->p_vaddr);
    for (size_t i=0;i+1<p->p_filesz/4;i++)
      if (code[i]==0x52800801u && code[i+1]==0xd2800022u) count++;
  }
  return count;
}

int main(int argc, char **argv) {
  if (argc!=3) return 2;
  so_module module;
  CHECK(load_module(argv[1],&module));
  game_compat_detect_package(argv[1]);
  CHECK(!strcmp(game_compat_package_name(),argv[2]));
  const int legacy=so_try_find_addr(&module,"_ZN7cocos2d8ZipUtils9hexToCharERKSs")!=0;
  if (legacy) {
    CHECK(uses_jni_string_slot(&module,169)); // GetStringUTFChars
    CHECK(uses_jni_string_slot(&module,170)); // ReleaseStringUTFChars
  } else {
    CHECK(uses_jni_string_slot(&module,164)); // GetStringLength
    CHECK(uses_jni_string_slot(&module,165)); // GetStringChars: shared boot fix
    CHECK(uses_jni_string_slot(&module,166)); // ReleaseStringChars
  }
  uint32_t *branch=leaderboard_branch(&module);
  CHECK(branch!=NULL);
  CHECK(count_verified_peers(&module)==0);
  int tls_patches=game_compat_apply_network(&module);
  printf("TLS patches: %d, verified peers: %d\n",tls_patches,count_verified_peers(&module));
  CHECK(tls_patches==1); // CCHttpClient; AssetsManager's separate updater is not used.
  CHECK(hook_count==1 && decoder_hook!=0);
  CHECK(*branch==0xd503201fu);
  CHECK(count_verified_peers(&module)==1);
  unsigned char (*decode)(const void *)=(void *)decoder_hook;
  CHECK(decode(NULL)==0);
  if (legacy) {
    const char *digits="41";CHECK(decode(&digits)==0x41);
    digits="fF";CHECK(decode(&digits)==0xff);
    digits="A";CHECK(decode(&digits)==10);
    digits="z0";CHECK(decode(&digits)==0);
    digits=NULL;CHECK(decode(&digits)==0);
  } else {
    unsigned char short_string[24]={4,'4','1',0};
    CHECK(decode(short_string)==0x41);
    short_string[1]='f';short_string[2]='F';CHECK(decode(short_string)==0xff);
    short_string[0]=2;short_string[1]='A';short_string[2]=0;CHECK(decode(short_string)==10);
    memset(short_string,0,sizeof short_string);CHECK(decode(short_string)==0);
    const char *digits="E2";uint64_t long_string[3]={33,2,0};
    memcpy(&long_string[2],&digits,sizeof digits);CHECK(decode(long_string)==0xe2);
    digits=NULL;memcpy(&long_string[2],&digits,sizeof digits);CHECK(decode(long_string)==0);
  }
  unload_module(&module);

  CHECK(load_module(argv[1],&module));
  tls_ready=0;hook_count=0;
  CHECK(game_compat_apply_network(&module)==0);
  CHECK(hook_count==1);
  CHECK(count_verified_peers(&module)==0);
  unload_module(&module);

  CHECK(load_module(argv[1],&module));
  tls_ready=1;branch=leaderboard_branch(&module);CHECK(branch!=NULL);
  const uint32_t original_branch=*branch;
  branch[-2]=0xd503201fu; // Wrong sharedState call: branch must be left alone.
  CHECK(game_compat_apply_network(&module)==1);
  CHECK(*branch==original_branch);
  unload_module(&module);
  printf("PASS: %s (%s string ABI, %d checks)\n",argv[2],legacy?"GNU":"libc++",checks);
  return 0;
}
