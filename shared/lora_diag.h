#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "iot_pins.h"

// Called when LoRa.begin() fails. Reads the radio's version register (0x42) directly over SPI.
// A working RFM95W / SX1276 answers 0x12. Any other value tells you what kind of wiring problem it is.
inline void loraDiagnose() {
  // Pulse reset so the chip is in a known state
  pinMode(IoTPins::LORA_RST, OUTPUT);
  digitalWrite(IoTPins::LORA_RST, LOW);
  delay(10);
  digitalWrite(IoTPins::LORA_RST, HIGH);
  delay(10);

  pinMode(IoTPins::LORA_CS, OUTPUT);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(IoTPins::LORA_CS, LOW);
  SPI.transfer(0x42 & 0x7F);              // read register 0x42 (version)
  const uint8_t version = SPI.transfer(0x00);
  digitalWrite(IoTPins::LORA_CS, HIGH);
  SPI.endTransaction();

  Serial.print("  LoRa diagnostic: version register = 0x");
  Serial.println(version, HEX);
  if (version == 0x12) {
    Serial.println("  -> chip answers correctly now (loose contact?). Press RST to retry.");
  } else if (version == 0x00) {
    Serial.println("  -> reads 0x00: radio has no power or MISO is held low.");
    Serial.println("     Check VIN -> 3V3 and GND -> GND, then MISO -> GPIO 19.");
  } else if (version == 0xFF) {
    Serial.println("  -> reads 0xFF: nothing is answering. Usually CS (GPIO 14), SCK (GPIO 18)");
    Serial.println("     or MISO (GPIO 19) is unplugged / in the wrong row, or the radio has no power.");
  } else {
    Serial.println("  -> random value: a data wire is loose or swapped (SCK 18 / MISO 19 / MOSI 23).");
  }
}
