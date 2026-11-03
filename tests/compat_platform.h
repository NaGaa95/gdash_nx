#ifndef TEST_COMPAT_PLATFORM_H
#define TEST_COMPAT_PLATFORM_H
#define __SO_UTIL_H__
#include <stdint.h>
#include <stddef.h>

#define PT_LOAD 1
#define PF_X 1
typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;
typedef struct {
  uint32_t name, type;
  uint64_t flags, address, offset, size;
  uint32_t link, info;
  uint64_t alignment, entry_size;
} TestSection;
typedef struct {
  uint32_t name;
  uint8_t info, other;
  uint16_t section;
  uint64_t value, size;
} TestSymbol;
typedef struct {
  void *load_base;
  size_t load_size;
  Elf64_Phdr phdr[16];
  int phnum;
  unsigned char *file;
  TestSymbol *symbols;
  size_t symbol_count;
  const char *strings;
} so_module;
uintptr_t so_try_find_addr(so_module *, const char *);
void hook_arm64(uintptr_t, uintptr_t);
#endif
