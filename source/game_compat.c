/* game_compat.c -- support for the paid game and its four free variants.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "game_compat.h"
#include "net_shim.h"
#include "paths.h"

#define SCAN_SIZE 65536
#define SCAN_OVERLAP 96

typedef struct {
  const char *package_name;
  const char *data_marker;
} GamePackage;

static const GamePackage packages[] = {
  { "com.robtopx.geometryjump",         "/data/data/com.robtopx.geometryjump/" },
  { "com.robtopx.geometryjumplite",     "/data/data/com.robtopx.geometryjumplite/" },
  { "com.robtopx.geometrydashmeltdown", "/data/data/com.robtopx.geometrydashmeltdown/" },
  { "com.robtopx.geometrydashsubzero",  "/data/data/com.robtopx.geometrydashsubzero/" },
  { "com.robtopx.geometrydashworld",    "/data/data/com.robtopx.geometrydashworld/" },
};

static const char *detected_package = GD_PACKAGE_NAME;

static int contains_bytes(const unsigned char *buf, size_t len,
                          const char *needle) {
  const size_t needle_len = strlen(needle);
  if (needle_len == 0 || needle_len > len)
    return 0;
  for (size_t i = 0; i <= len - needle_len; i++)
    if (buf[i] == (unsigned char)needle[0] &&
        memcmp(buf + i, needle, needle_len) == 0)
      return 1;
  return 0;
}

void game_compat_detect_package(const char *so_path) {
  detected_package = GD_PACKAGE_NAME;
  FILE *f = fopen(so_path, "rb");
  if (!f)
    return;

  unsigned char buf[SCAN_SIZE + SCAN_OVERLAP];
  size_t carry = 0;
  while (!feof(f)) {
    const size_t got = fread(buf + carry, 1, SCAN_SIZE, f);
    const size_t total = carry + got;
    for (unsigned i = 0; i < sizeof(packages) / sizeof(*packages); i++) {
      if (contains_bytes(buf, total, packages[i].data_marker)) {
        detected_package = packages[i].package_name;
        fclose(f);
        return;
      }
    }
    if (got == 0)
      break;
    carry = total < SCAN_OVERLAP ? total : SCAN_OVERLAP;
    memmove(buf, buf + total - carry, carry);
  }
  fclose(f);
}

const char *game_compat_package_name(void) {
  return detected_package;
}

static int is_bl(uint32_t instruction) {
  return (instruction & 0xfc000000u) == 0x94000000u;
}

static uintptr_t bl_target(uintptr_t pc, uint32_t instruction) {
  int64_t immediate = instruction & 0x03ffffffu;
  if (immediate & 0x02000000)
    immediate -= 0x04000000;
  return (uintptr_t)((int64_t)pc + immediate * 4);
}

int game_compat_play_layer_offset(so_module *mod) {
  uintptr_t address = so_try_find_addr(mod, "_ZN11AppDelegate9pauseGameEv");
  uintptr_t shared = so_try_find_addr(mod, "_ZN11GameManager11sharedStateEv");
  if (!address || !shared)
    return -1;
  const uint32_t *code = (const uint32_t *)address;
  for (unsigned i = 0; i + 1 < 24; i++) {
    if (!is_bl(code[i]) || bl_target((uintptr_t)&code[i], code[i]) != shared)
      continue;
    // ldr x0, [x0, #offset], immediately after sharedState(). Both known
    // layouts use the same pointer repeatedly in this bounded routine.
    if ((code[i + 1] & 0xffc003ffu) != 0xf9400000u)
      continue;
    unsigned offset = ((code[i + 1] >> 10) & 0xfffu) * 8;
    if (offset == 472 || offset == 544)
      return (int)offset;
  }
  return -1;
}

int game_compat_running_scene_offset(so_module *mod) {
  uintptr_t address = so_try_find_addr(mod, "_ZN11AppDelegate9pauseGameEv");
  uintptr_t shared = so_try_find_addr(mod, "_ZN7cocos2d10CCDirector14sharedDirectorEv");
  if (!address || !shared)
    return -1;
  const uint32_t *code = (const uint32_t *)address;
  for (unsigned i = 0; i + 3 < 10; i++) {
    if (!is_bl(code[i]) || bl_target((uintptr_t)&code[i], code[i]) != shared ||
        (code[i + 1] & 0xffc003ffu) != 0xf9400000u ||
        (code[i + 2] & 0xff00001fu) != 0xb4000000u ||
        code[i + 3] != 0xb9401c00u) // ldr w0, [scene, #28] (CCNode tag)
      continue;
    unsigned offset = ((code[i + 1] >> 10) & 0xfffu) * 8;
    if (offset == 264 || offset == 272)
      return (int)offset;
  }
  return -1;
}

static int hex_digit_value(unsigned char digit) {
  if (digit >= '0' && digit <= '9')
    return digit - '0';
  if (digit >= 'A' && digit <= 'F')
    return digit - 'A' + 10;
  if (digit >= 'a' && digit <= 'f')
    return digit - 'a' + 10;
  return -1;
}

// Geometry Dash 2.2.147's ZipUtils::hexToChar uses a C++ stringstream for
// each percent-escaped byte. Its cached std::ctype table is not compatible
// with this runtime and crashes while parsing otherwise valid online-level
// song metadata. The affected build uses the old GNU std::string ABI: a
// const reference points to an object whose first word is the character data.
static unsigned char decode_hex_byte(const unsigned char *data) {
  if (!data)
    return 0;

  const int high = hex_digit_value(data[0]);
  if (high < 0)
    return 0;
  const int low = hex_digit_value(data[1]);
  if (low < 0)
    return (unsigned char)high;
  return (unsigned char)((high << 4) | low);
}

static unsigned char hex_to_char_compat(const void *string_ref) {
  if (!string_ref)
    return 0;
  const unsigned char *data = NULL;
  memcpy(&data, string_ref, sizeof(data));
  return decode_hex_byte(data);
}

// 2.2.148 uses Android libc++ strings instead. Its disassembly tests bit 0
// of byte 0: short strings start at byte 1, long strings store data at +16.
// Keep the two entry points separate: interpreting one ABI as the other can
// turn a two-byte percent escape into an invalid pointer.
static unsigned char hex_to_char_libcpp_compat(const void *string_ref) {
  if (!string_ref)
    return 0;
  const unsigned char *object = string_ref;
  const unsigned char *data = object + 1;
  if (object[0] & 1)
    memcpy(&data, object + 2 * sizeof(uintptr_t), sizeof(data));
  return decode_hex_byte(data);
}

static int patch_url_decoder(so_module *mod) {
  int patched = 0;
  uintptr_t address = so_try_find_addr(
      mod, "_ZN7cocos2d8ZipUtils9hexToCharERKSs");
  if (address) {
    hook_arm64(address, (uintptr_t)&hex_to_char_compat);
    patched++;
  }
  address = so_try_find_addr(mod,
      "_ZN7cocos2d8ZipUtils9hexToCharERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
  if (address) {
    hook_arm64(address, (uintptr_t)&hex_to_char_libcpp_compat);
    patched++;
  }
  return patched;
}

static int patch_public_leaderboard_bootstrap(so_module *mod) {
  const uintptr_t address = so_try_find_addr(
      mod,
      "_ZN17LeaderboardsLayer17selectLeaderboardE15LeaderboardType15LeaderboardStat");
  if (!address)
    return 0;

  const uintptr_t shared_state = so_try_find_addr(
      mod, "_ZN11GameManager11sharedStateEv");
  if (!shared_state)
    return 0;

  // On a fresh/anonymous profile the stock client tries updateUserScore()
  // before its first getGJScores20 request. Boomlings rejects that upload
  // without an account, and the failure callback never starts the public
  // leaderboard download. Skip only that decision branch; the existing path
  // immediately below still performs the normal cached/network lookup.
  uint32_t *code = (uint32_t *)address;
  const uint32_t ldrb_score_uploaded_legacy = 0x3948a000u; // [x0, #552]
  const uint32_t ldrb_score_uploaded_148 = 0x394a4000u;    // [x0, #656]
  const uint32_t cbz_w0_mask = 0x7f00001fu;
  const uint32_t cbz_w0 = 0x34000000u;
  const uint32_t nop = 0xd503201fu;
  for (unsigned i = 1; i + 1 < 96; i++) {
    if ((code[i] != ldrb_score_uploaded_legacy &&
         code[i] != ldrb_score_uploaded_148) ||
        (code[i + 1] & cbz_w0_mask) != cbz_w0)
      continue;
    // Both supported layouts read this flag immediately after sharedState().
    // Check the call target as well as the offset before changing a branch.
    if (!is_bl(code[i - 1]) ||
        bl_target((uintptr_t)&code[i - 1], code[i - 1]) != shared_state)
      continue;
    code[i + 1] = nop;
    return 1;
  }

  return 0;
}

int game_compat_apply_network(so_module *mod) {
  // This parser fix is independent of TLS setup and must remain active even
  // if CA export failed, so a later retry cannot enter the broken decoder.
  patch_url_decoder(mod);
  patch_public_leaderboard_bootstrap(mod);

  if (!net_tls_ca_ready())
    return 0;

  // CCHttpClient sets libcurl options 64 (SSL_VERIFYPEER) and 81
  // (SSL_VERIFYHOST) to zero in every supported 2.2.14x arm64 build. It also
  // never sets CURLOPT_CAINFO, so enabling verification alone makes curl fail
  // before the TLS handshake. Match the complete option sequence and replace
  // the nonessential TCP keepalive / DNS-cache-timeout setters with one
  // CURLOPT_CAINFO setter that points at the firmware CA bundle. The exact
  // sequence and shared curl_easy_setopt destination keep this constrained to
  // the known CCHttpClient routine across GD variants.
  const uint32_t mov_x0_x19 = 0xaa1303e0u;
  const uint32_t mov_w1_verify_peer = 0x52800801u; // mov w1, #64
  const uint32_t mov_w1_verify_host = 0x52800a21u; // mov w1, #81
  const uint32_t mov_w1_tcp_keepalive = 0x52801aa1u; // mov w1, #213
  const uint32_t mov_w1_dns_cache_timeout = 0x52800b81u; // mov w1, #92
  const uint32_t mov_w1_cainfo = 0x5284ea21u; // mov w1, #10065
  const uint32_t mov_x2_zero = 0xd2800002u;
  const uint32_t mov_x2_one = 0xd2800022u;
  const uint32_t mov_x2_two = 0xd2800042u;
  const uint32_t mov_x2_sixty = 0xd2800782u;
  const uint32_t ldr_x2_literal_16 = 0x58000082u; // ldr x2, PC + 16
  const uint32_t branch_forward_16 = 0x14000004u; // b PC + 16
  const uint32_t nop = 0xd503201fu;
  const uintptr_t ca_path = (uintptr_t)path_ca_bundle();
  int patched = 0;

  for (int segment = 0; segment < mod->phnum; segment++) {
    const Elf64_Phdr *p = &mod->phdr[segment];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz < 24)
      continue;
    uint32_t *code = (uint32_t *)((uintptr_t)mod->load_base + p->p_vaddr);
    const size_t words = p->p_filesz / sizeof(*code);
    for (size_t i = 0; i + 5 < words; i++) {
      if (code[i] != mov_w1_verify_peer || code[i + 1] != mov_x2_zero ||
          !is_bl(code[i + 2]))
        continue;
      const uintptr_t peer_target =
          bl_target((uintptr_t)&code[i + 2], code[i + 2]);
      const size_t limit = i + 12 < words ? i + 12 : words - 2;
      for (size_t j = i + 3; j < limit; j++) {
        if (code[j] != mov_w1_verify_host || code[j + 1] != mov_x2_zero ||
            !is_bl(code[j + 2]))
          continue;
        if (bl_target((uintptr_t)&code[j + 2], code[j + 2]) != peer_target)
          continue;

        // The keepalive and DNS-cache setters immediately following the
        // verification pair provide an eight-instruction slot. Use a literal
        // load so the CA path may live anywhere in the NRO address space, then
        // branch over the embedded 64-bit pointer into the original epilogue.
        for (size_t k = j + 3; k < j + 24 && k + 7 < words; k++) {
          if (code[k] != mov_x0_x19 ||
              code[k + 1] != mov_w1_tcp_keepalive ||
              code[k + 2] != mov_x2_one || !is_bl(code[k + 3]) ||
              code[k + 4] != mov_x0_x19 ||
              code[k + 5] != mov_w1_dns_cache_timeout ||
              code[k + 6] != mov_x2_sixty || !is_bl(code[k + 7]))
            continue;
          if (bl_target((uintptr_t)&code[k + 3], code[k + 3]) != peer_target ||
              bl_target((uintptr_t)&code[k + 7], code[k + 7]) != peer_target)
            continue;

          code[i + 1] = mov_x2_one;
          code[j + 1] = mov_x2_two;
          code[k + 1] = mov_w1_cainfo;
          code[k + 2] = ldr_x2_literal_16;
          // code[k + 3] remains the original curl_easy_setopt BL.
          code[k + 4] = branch_forward_16;
          code[k + 5] = nop;
          memcpy(&code[k + 6], &ca_path, sizeof(ca_path));
          patched++;
          break;
        }
        break;
      }
    }
  }

  return patched;
}
