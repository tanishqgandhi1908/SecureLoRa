#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>
#include <Preferences.h>    // ESP32 flash key-value store (NVS): keeps SEQ across reboots

#include "iot_common.h"     // from ../shared: serial, I2C, SPI helpers + kit pin map
#include "lora_settings.h"  // from ../shared: radio settings (must match the gateway)
#include "lora_diag.h"      // from ../shared: explains a LoRa init failure
#include "packet_auth.h"    // from ../shared: HMAC-SHA256 tag (needs shared/secret_key.h)

// SecureLoRa NODE.
//
// Every SEND_INTERVAL_MS: read the DHT-11, light the LED if too hot,
// and send one AUTHENTICATED text packet over LoRa, e.g.
//   N=1;SEQ=5;T=23.0;H=41.0;A=0;M=3f9c...(32 hex)
//   N = node id, SEQ = counter, T = temp (C), H = relative humidity (%), A = alert (1 = over threshold)
//   M = HMAC-SHA256 tag over everything before ";M=" (see shared/packet_auth.h)
//
// Replay protection: SEQ is saved in flash BEFORE each send, so it never repeats, even after a
// reboot or power cut. The gateway only accepts SEQ greater than the last one it accepted.
//
// Sensor change (Nov 20): the BMP280 failed, so we use the DHT-11 from the spare kit.
// The DHT-11 has no pressure sensor, so the packet carries humidity instead of pressure.
//
// Not done (on purpose, to keep it simple): ATECC608A signing, join handshake, deep sleep.
//
// TEST-ONLY attack commands (Serial Monitor over USB, for the fault tests / demo):
//   replay  -> re-send the last packet exactly as it was   (gateway should say REPLAY)
//   tamper  -> re-send the last packet with T changed      (gateway should say BAD_SIGNATURE)
//   forge   -> send a new packet with a made-up tag        (gateway should say BAD_SIGNATURE)
// They need physical USB access, and they only send packets the gateway must REJECT.
// Set ENABLE_ATTACK_TESTS to 0 to remove them from the firmware.
#define ENABLE_ATTACK_TESTS 1

constexpr uint8_t  NODE_ID              = 1;
constexpr uint32_t SEND_INTERVAL_MS     = 10000;  // 10 s for bench testing; the design uses 60 s
constexpr float    TEMP_THRESHOLD_C     = 35.0;   // design value. For a bench test, set just above room temp (e.g. 30) and hold the sensor
constexpr uint8_t  ALERT_LED            = IoTPins::LED_1;  // GPIO 2 (red LED / onboard LED)

// DHT-11 data pin. Every free GPIO is taken by the lab wiring, so the data wire shares GPIO 33
// with lab Button 2. That's safe: the button only connects the pin to GND while pressed.
// Don't press Button 2 while the project runs (it would corrupt that one reading).
constexpr uint8_t  DHT_PIN              = IoTPins::BUTTON_2;  // GPIO 33

DHT dht(DHT_PIN, DHT11);
Preferences prefs;
bool loraOk = false;
uint32_t seq = 0;          // loaded from flash at boot
uint32_t lastSendMs = 0;
String lastPacket;         // kept only for the replay/tamper tests

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

// Sends one packet over LoRa and prints it.
void transmit(const String& packet) {
  bool sent = false;
  if (loraOk) {
    LoRa.beginPacket();
    LoRa.print(packet);
    sent = (LoRa.endPacket() == 1);  // blocks until the packet is on air (~110 ms at SF7 for ~65 bytes)
  }
  lastPacket = packet;
  Serial.print("Packet: ");
  Serial.print(packet);
  Serial.print("  (");
  Serial.print(packet.length());
  Serial.print(" bytes)  ");
  Serial.println(!loraOk ? "NOT SENT (LoRa not initialized)" : sent ? "SENT" : "SEND FAILED");
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

  // Save the new SEQ BEFORE sending: if power dies right after, the next boot still uses a
  // higher number, so a SEQ is never sent twice.
  seq++;
  prefs.putULong("seq", seq);

  char body[64];
  snprintf(body, sizeof(body), "N=%u;SEQ=%lu;T=%.1f;H=%.1f;A=%d",
           NODE_ID, (unsigned long)seq, tempC, humidityPct, alert ? 1 : 0);
  char tag[PacketAuth::TAG_LEN * 2 + 1];
  PacketAuth::computeTag(body, tag);

  transmit(String(body) + PacketAuth::TAG_FIELD + tag);
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

  // HMAC key (shared/secret_key.h). Without a valid key we refuse to send anything.
  if (!PacketAuth::loadKey()) {
    Serial.println("HMAC KEY MISSING/INVALID: copy shared/secret_key_example.h to shared/secret_key.h "
                   "and put 64 hex chars in it (openssl rand -hex 32). Not sending.");
  }

  // Last SEQ from flash (survives reboot and re-flashing; only a full flash erase resets it)
  prefs.begin("securelora", false);
  seq = prefs.getULong("seq", 0);
  Serial.println("Last SEQ from flash: " + String(seq));

  iotInitSPI();
  loraOk = initLoRa();
  Serial.println(loraOk ? "LoRa OK (915 MHz)" : "LoRa INIT FAILED (check wiring: CS 14, RST 26, DIO0 25)");
  if (!loraOk) loraDiagnose();
  Serial.println("Sending one reading every " + String(SEND_INTERVAL_MS / 1000) + " s.");

  if (PacketAuth::keyOk) sendReading();  // send one right away so you don't have to wait
  lastSendMs = millis();
}

#if ENABLE_ATTACK_TESTS
// Fault-test commands. Each one sends a packet the gateway MUST reject.
void checkSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;
  if (lastPacket.length() == 0 && cmd != "forge") {
    Serial.println("No packet sent yet, wait for the first reading.");
    return;
  }
  if (cmd == "replay") {
    Serial.println("[TEST] Replaying the last packet unchanged (expect REPLAY at the gateway):");
    transmit(lastPacket);
  } else if (cmd == "tamper") {
    // Change the temperature to 99.9 but keep the old tag
    String body, tag;
    PacketAuth::split(lastPacket, body, tag);
    const int t = body.indexOf(";T=");
    const int end = body.indexOf(';', t + 1);
    String tampered = body.substring(0, t) + ";T=99.9" + body.substring(end);
    Serial.println("[TEST] Sending the last packet with T changed to 99.9, old tag (expect BAD_SIGNATURE):");
    transmit(tampered + PacketAuth::TAG_FIELD + tag);
  } else if (cmd == "forge") {
    // A new, never-used SEQ but no key: the attacker has to guess the tag
    char body[64];
    snprintf(body, sizeof(body), "N=%u;SEQ=%lu;T=21.0;H=40.0;A=0", NODE_ID, (unsigned long)(seq + 1000));
    Serial.println("[TEST] Sending a forged packet with a made-up tag (expect BAD_SIGNATURE):");
    transmit(String(body) + PacketAuth::TAG_FIELD + "00112233445566778899aabbccddeeff");
  } else {
    Serial.println("Unknown command. Test commands: replay, tamper, forge");
  }
}
#endif

void loop() {
#if ENABLE_ATTACK_TESTS
  checkSerialCommands();
#endif
  if (iotEvery(SEND_INTERVAL_MS, lastSendMs)) {
    if (PacketAuth::keyOk) sendReading();
  }
}
