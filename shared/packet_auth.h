#pragma once
#include <Arduino.h>
#include "mbedtls/md.h"     // HMAC-SHA256, built into the ESP32 core (no extra library)
#include "secret_key.h"     // YOUR copy of secret_key_example.h (git-ignored). Same file on node + gateway.

// Packet authentication with HMAC-SHA256 and a pre-shared key.
//
// The node sends:   <body>;M=<tag>
//   body = "N=1;SEQ=42;T=20.7;H=42.5;A=0"
//   tag  = first 16 bytes of HMAC-SHA256(key, body), as 32 hex characters
//
// Why it works: only someone with the key can compute a tag that matches the body.
// Changing ANY character of the body (value, SEQ, node id) changes the correct tag.
// What it does NOT do: hide the data (readings are readable by anyone), or stop someone who
// steals the node and reads the key from its flash. Replay protection comes from SEQ (see gateway).
//
// This is the proposal's documented fallback ("authenticated-message design") instead of the
// ATECC608A ECDSA signature.

namespace PacketAuth {

constexpr size_t KEY_LEN = 32;   // 256-bit key
constexpr size_t TAG_LEN = 16;   // 128-bit tag (truncated HMAC, common practice)
constexpr const char* TAG_FIELD = ";M=";

static uint8_t key[KEY_LEN];
static bool keyOk = false;

// Parse SECURELORA_HMAC_KEY_HEX (64 hex chars) into bytes. Returns false if missing/invalid.
static bool loadKey() {
  const char* hex = SECURELORA_HMAC_KEY_HEX;
  if (strlen(hex) != KEY_LEN * 2) return keyOk = false;
  for (size_t i = 0; i < KEY_LEN; i++) {
    char byteStr[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end = nullptr;
    key[i] = (uint8_t)strtoul(byteStr, &end, 16);
    if (*end != 0) return keyOk = false;
  }
  return keyOk = true;
}

// tagHex must hold TAG_LEN*2 + 1 chars.
static void computeTag(const String& body, char* tagHex) {
  uint8_t full[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, KEY_LEN,
                  (const uint8_t*)body.c_str(), body.length(), full);
  for (size_t i = 0; i < TAG_LEN; i++) sprintf(tagHex + 2 * i, "%02x", full[i]);
  tagHex[TAG_LEN * 2] = 0;
}

// Constant-time compare, so an attacker can't learn how many leading characters were right.
static bool tagsEqual(const char* a, const char* b) {
  if (strlen(a) != TAG_LEN * 2 || strlen(b) != TAG_LEN * 2) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < TAG_LEN * 2; i++) diff |= (uint8_t)(tolower(a[i]) ^ tolower(b[i]));
  return diff == 0;
}

// Splits "<body>;M=<tag>". Returns false if there is no tag field.
static bool split(const String& packet, String& body, String& tag) {
  const int pos = packet.lastIndexOf(TAG_FIELD);
  if (pos < 0) return false;
  body = packet.substring(0, pos);
  tag = packet.substring(pos + strlen(TAG_FIELD));
  return true;
}

// True if the packet's tag matches its body.
static bool verify(const String& packet, String& bodyOut) {
  String tag;
  if (!split(packet, bodyOut, tag)) return false;
  char expected[TAG_LEN * 2 + 1];
  computeTag(bodyOut, expected);
  return tagsEqual(expected, tag.c_str());
}

}  // namespace PacketAuth
