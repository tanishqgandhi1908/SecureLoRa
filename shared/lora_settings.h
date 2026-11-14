#pragma once
#include <Arduino.h>

// Radio settings shared by the node and the gateway.
// BOTH boards must use exactly the same values, or they will not hear each other.

namespace LoRaSettings {
  constexpr long     FREQUENCY_HZ     = 915E6;  // US ISM band (same as Activity 6)
  constexpr int      SPREADING_FACTOR = 7;      // SF7: shortest airtime (design doc: SF7 / 125 kHz)
  constexpr long     BANDWIDTH_HZ     = 125E3;
  constexpr int      CODING_RATE_4_X  = 5;      // coding rate 4/5
  constexpr int      TX_POWER_DBM     = 17;     // RFM95W PA_BOOST, 2-17 dBm (20 = max, more current)

  // Sync word: radios with a different sync word ignore each other's packets.
  // We use our own value so we don't receive other teams' Activity 6 beacons (default 0x12).
  // NOTE: this is NOT security. Anyone can set the same sync word. Security comes from signatures (later).
  constexpr uint8_t  SYNC_WORD        = 0xF3;
}
