#pragma once

// HOW TO USE: copy this file to shared/secret_key.h and put a real random key in it.
//   openssl rand -hex 32
// The SAME secret_key.h must be used to build BOTH the node and the gateway.
// secret_key.h is git-ignored. Share it with teammates privately, never in Git or a group chat.
// Anyone with this key can create packets the gateway will accept.

#define SECURELORA_HMAC_KEY_HEX "CHANGE_ME_TO_64_HEX_CHARACTERS_FROM_openssl_rand_hex_32________"
