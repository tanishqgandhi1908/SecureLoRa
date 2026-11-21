#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>

#include "iot_common.h"     // from ../shared: serial, I2C, SPI helpers + kit pin map
#include "lora_settings.h"
#include "lora_diag.h"       // from ../shared: explains a LoRa init failure  // from ../shared: radio settings (must match the gateway)

// SecureLoRa NODE - vertical slice version (UNSIGNED, plain text).
//
// Every SEND_INTERVAL_MS: read the DHT-11, light the LED if too hot,
// and send one text packet over LoRa, e.g.
//   N=1;SEQ=5;T=23.0;H=41.0;A=0
//   N = node id, SEQ = counter, T = temp (C), H = relative humidity (%), A = alert (1 = over threshold)
//
// Sensor change (Nov 20): the BMP280 failed, so we use the DHT-11 from the spare kit.
// The DHT-11 has no pressure sensor, so the packet carries humidity instead of pressure.
//
// Later steps (not here yet): binary packet, ATECC608A signature, session/join, deep sleep.

constexpr uint8_t  NODE_ID              = 1;
constexpr uint32_t SEND_INTERVAL_MS     = 10000;  // 10 s for bench testing; the design uses 60 s
constexpr float    TEMP_THRESHOLD_C     = 35.0;   // design value. For a bench test, set just above room temp (e.g. 30) and hold the sensor
constexpr uint8_t  ALERT_LED            = IoTPins::LED_1;  // GPIO 2 (red LED / onboard LED)

// DHT-11 data pin. Every free GPIO is taken by the lab wiring, so the data wire shares GPIO 33
// with lab Button 2. That's safe: the button only connects the pin to GND while pressed.
// Don't press Button 2 while the project runs (it would corrupt that one reading).
constexpr uint8_t  DHT_PIN              = IoTPins::BUTTON_2;  // GPIO 33

DHT dht(DHT_PIN, DHT11);
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
  const float tempC = dht.readTemperature();
  const float humidityPct = dht.readHumidity();
  if (isnan(tempC) || isnan(humidityPct)) {
    // No reading -> no packet (never send a made-up value). SEQ doesn't advance, so the server
    // sees a time gap, not lost packets, and this line in the node log explains it.
    Serial.println("DHT-11 read FAILED (check wiring: + -> 3V3, - -> GND, S -> GPIO 33). Nothing sent.");
    return;
  }
  const bool alert = tempC > TEMP_THRESHOLD_C;

  // DECIDE + ACT: LED on while temperature is over the threshold
  digitalWrite(ALERT_LED, alert ? HIGH : LOW);

  seq++;
  char packet[64];
  snprintf(packet, sizeof(packet), "N=%u;SEQ=%lu;T=%.1f;H=%.1f;A=%d",
           NODE_ID, (unsigned long)seq, tempC, humidityPct, alert ? 1 : 0);

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

  // Lab parts stay wired on this board but are unused by the project.
  // Hold their pins LOW so a floating pin can't twitch the servo or click the buzzer.
  pinMode(IoTPins::SERVO_PWM, OUTPUT);
  digitalWrite(IoTPins::SERVO_PWM, LOW);
  pinMode(IoTPins::BUZZER, OUTPUT);
  digitalWrite(IoTPins::BUZZER, LOW);
  // Yellow (D4) and green (D15) lab LEDs are unused. D15 is pulled HIGH at boot, so switch them off.
  pinMode(IoTPins::LED_2, OUTPUT);
  digitalWrite(IoTPins::LED_2, LOW);
  pinMode(IoTPins::LED_3, OUTPUT);
  digitalWrite(IoTPins::LED_3, LOW);

  // The OLED (if still wired from the labs) shares the SPI bus: keep it deselected.
  pinMode(IoTPins::OLED_CS, OUTPUT);
  digitalWrite(IoTPins::OLED_CS, HIGH);
  pinMode(IoTPins::LORA_CS, OUTPUT);
  digitalWrite(IoTPins::LORA_CS, HIGH);

  // I2C: still used later for the ATECC608A (0x60). The scan just shows what's on the bus.
  // First check the bus isn't stuck: with nothing talking, SDA and SCL must both read HIGH
  // (pull-ups). A LOW line means a wire/device is holding it down, and a scan would just
  // time out on every address. The DHT-11 and LoRa don't use I2C, so we carry on either way.
  pinMode(IoTPins::I2C_SDA, INPUT_PULLUP);
  pinMode(IoTPins::I2C_SCL, INPUT_PULLUP);
  delay(5);
  const bool sdaHigh = digitalRead(IoTPins::I2C_SDA) == HIGH;
  const bool sclHigh = digitalRead(IoTPins::I2C_SCL) == HIGH;
  if (sdaHigh && sclHigh) {
    iotInitI2C();
    Wire.setTimeOut(50);  // never wait long on one address
    iotI2CScan();
  } else {
    Serial.print("I2C BUS STUCK: ");
    if (!sdaHigh) Serial.print("SDA (GPIO 21) is LOW  ");
    if (!sclHigh) Serial.print("SCL (GPIO 22) is LOW  ");
    Serial.println("-> check wires in rows 21/22 (something touching GND?). Skipping I2C scan.");
  }

  // DHT-11 needs about 1 s after power-up before the first reading
  dht.begin();
  delay(1500);
  const float firstTemp = dht.readTemperature();
  Serial.println(isnan(firstTemp) ? "DHT-11 NOT RESPONDING (check wiring: S -> GPIO 33)"
                                  : "DHT-11 OK (GPIO 33)");

  iotInitSPI();
  loraOk = initLoRa();
  Serial.println(loraOk ? "LoRa OK (915 MHz)" : "LoRa INIT FAILED (check wiring: CS 14, RST 26, DIO0 25)");
  if (!loraOk) loraDiagnose();
  Serial.println("Sending one reading every " + String(SEND_INTERVAL_MS / 1000) + " s.");

  sendReading();  // send one right away so you don't have to wait
  lastSendMs = millis();
}

void loop() {
  if (iotEvery(SEND_INTERVAL_MS, lastSendMs)) {
    sendReading();
  }
}
