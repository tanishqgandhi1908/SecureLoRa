#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_BMP280.h>

#include "iot_common.h"     // from ../shared: serial, I2C, SPI helpers + kit pin map
#include "lora_settings.h"  // from ../shared: radio settings (must match the gateway)

// SecureLoRa NODE - vertical slice version (UNSIGNED, plain text).
//
// Every SEND_INTERVAL_MS: read the BMP280, light the LED if too hot,
// and send one text packet over LoRa, e.g.
//   N=1;SEQ=5;T=23.45;P=1012.6;A=0
//   N = node id, SEQ = counter, T = temp (C), P = pressure (hPa), A = alert (1 = over threshold)
//
// Later steps (not here yet): binary packet, ATECC608A signature, session/join, deep sleep.

constexpr uint8_t  NODE_ID              = 1;
constexpr uint32_t SEND_INTERVAL_MS     = 10000;  // 10 s for bench testing; the design uses 60 s
constexpr float    TEMP_THRESHOLD_C     = 35.0;   // design value. Set to ~25 to test the LED by touching the sensor
constexpr uint8_t  ALERT_LED            = IoTPins::LED_1;  // GPIO 2 (red LED / onboard LED)

Adafruit_BMP280 bmp(&Wire);
bool bmpOk = false;
bool loraOk = false;
uint32_t seq = 0;
uint32_t lastSendMs = 0;

bool initLoRa() {
  LoRa.setPins(IoTPins::LORA_CS, IoTPins::LORA_RST, IoTPins::LORA_DIO0);
  if (!LoRa.begin(LoRaSettings::FREQUENCY_HZ)) return false;
  LoRa.setSpreadingFactor(LoRaSettings::SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LoRaSettings::BANDWIDTH_HZ);
  LoRa.setCodingRate4(LoRaSettings::CODING_RATE_4_X);
  LoRa.setTxPower(LoRaSettings::TX_POWER_DBM);
  LoRa.setSyncWord(LoRaSettings::SYNC_WORD);
  LoRa.enableCrc();  // receiver drops corrupted packets
  return true;
}

void sendReading() {
  if (!bmpOk) {
    Serial.println("Skipping send: BMP280 not found.");
    return;
  }

  const float tempC = bmp.readTemperature();
  const float pressureHpa = bmp.readPressure() / 100.0;
  const bool alert = tempC > TEMP_THRESHOLD_C;

  // DECIDE + ACT: LED on while temperature is over the threshold
  digitalWrite(ALERT_LED, alert ? HIGH : LOW);

  seq++;
  char packet[64];
  snprintf(packet, sizeof(packet), "N=%u;SEQ=%lu;T=%.2f;P=%.1f;A=%d",
           NODE_ID, (unsigned long)seq, tempC, pressureHpa, alert ? 1 : 0);

  bool sent = false;
  if (loraOk) {
    LoRa.beginPacket();
    LoRa.print(packet);
    sent = (LoRa.endPacket() == 1);  // blocks until the packet is on air (~50 ms at SF7)
  }

  Serial.print("Packet: ");
  Serial.print(packet);
  Serial.print("  (");
  Serial.print(strlen(packet));
  Serial.print(" bytes)  ");
  Serial.println(!loraOk ? "NOT SENT (LoRa not initialized)" : sent ? "SENT" : "SEND FAILED");
}

void setup() {
  iotStartSerial("SecureLoRa NODE");
  iotPrintDeviceId();

  pinMode(ALERT_LED, OUTPUT);
  digitalWrite(ALERT_LED, LOW);

  // The OLED (if still wired from the labs) shares the SPI bus: keep it deselected.
  pinMode(IoTPins::OLED_CS, OUTPUT);
  digitalWrite(IoTPins::OLED_CS, HIGH);
  pinMode(IoTPins::LORA_CS, OUTPUT);
  digitalWrite(IoTPins::LORA_CS, HIGH);

  // I2C: BMP280 (0x76) and ATECC608A (0x60) share SDA 21 / SCL 22
  iotInitI2C();
  iotI2CScan();
  bmpOk = bmp.begin(IoTPins::BMP280_ADDR);
  Serial.println(bmpOk ? "BMP280 OK" : "BMP280 NOT FOUND (check wiring, SDO->GND, CSB->3V3)");

  iotInitSPI();
  loraOk = initLoRa();
  Serial.println(loraOk ? "LoRa OK (915 MHz)" : "LoRa INIT FAILED (check wiring: CS 14, RST 26, DIO0 25)");
  Serial.println("Sending one reading every " + String(SEND_INTERVAL_MS / 1000) + " s.");

  sendReading();  // send one right away so you don't have to wait
  lastSendMs = millis();
}

void loop() {
  if (iotEvery(SEND_INTERVAL_MS, lastSendMs)) {
    sendReading();
  }
}
