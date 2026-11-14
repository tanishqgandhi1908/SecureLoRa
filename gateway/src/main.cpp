#include <Arduino.h>
#include <Wire.h>   // not used directly, but iot_common.h needs it (PlatformIO only links what main.cpp includes)
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "iot_common.h"     // from ../shared: serial, SPI helpers + kit pin map
#include "lora_settings.h"  // from ../shared: radio settings (must match the node)
#include "secrets.h"        // YOUR copy of secrets_example.h (not in Git)

// SecureLoRa GATEWAY - vertical slice version.
//
//   1. Listen for LoRa packets from the node:  N=1;SEQ=5;T=23.45;P=1012.6;A=0
//   2. Parse them and log RSSI/SNR.
//   3. POST each reading as JSON to the server over Wi-Fi.
//
// Packets are NOT signed yet, so every reading is marked "verdict":"unverified".
// Later steps: ATECC608A signature check, session + sequence (replay) checks,
// flash ring buffer for Wi-Fi/server outages.
//
// Serial Monitor command (for testing without a second radio):
//   fake  -> POST one clearly-labelled TEST reading to the server.

constexpr uint32_t WIFI_RETRY_MS = 10000;  // try to reconnect every 10 s if Wi-Fi drops

bool loraOk = false;
uint32_t lastWifiAttemptMs = 0;

// Simple counters, printed with every packet (useful for the 72 h reliability report)
uint32_t packetsReceived = 0;
uint32_t packetsMalformed = 0;
uint32_t postsOk = 0;
uint32_t postsFailed = 0;
long lastSeqFromNode = -1;     // to spot missing packets (gaps in SEQ)
uint32_t packetsMissed = 0;

// ---------------------------------------------------------------- LoRa

bool initLoRa() {
  LoRa.setPins(IoTPins::LORA_CS, IoTPins::LORA_RST, IoTPins::LORA_DIO0);
  if (!LoRa.begin(LoRaSettings::FREQUENCY_HZ)) return false;
  LoRa.setSpreadingFactor(LoRaSettings::SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LoRaSettings::BANDWIDTH_HZ);
  LoRa.setCodingRate4(LoRaSettings::CODING_RATE_4_X);
  LoRa.setSyncWord(LoRaSettings::SYNC_WORD);
  LoRa.enableCrc();  // corrupted packets are dropped by the radio
  return true;
}

// Finds "KEY=value" in a packet like "N=1;SEQ=5;T=23.45". Returns false if KEY is missing.
bool getField(const String& packet, const String& key, String& value) {
  const String search = key + "=";
  int start = -1;
  // match the key only at the start of the packet or right after a ';'
  if (packet.startsWith(search)) {
    start = 0;
  } else {
    const int pos = packet.indexOf(";" + search);
    if (pos >= 0) start = pos + 1;
  }
  if (start < 0) return false;
  start += search.length();
  int end = packet.indexOf(';', start);
  if (end < 0) end = packet.length();
  value = packet.substring(start, end);
  return value.length() > 0;
}

// ---------------------------------------------------------------- Wi-Fi + HTTP

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // non-blocking: connects in the background
  lastWifiAttemptMs = millis();
  Serial.println("Wi-Fi: connecting...");
}

// Called every loop: if Wi-Fi dropped, retry every WIFI_RETRY_MS without blocking LoRa.
void keepWiFiAlive() {
  static bool wasConnected = false;
  const bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !wasConnected) {
    Serial.print("Wi-Fi: connected, IP ");
    Serial.println(WiFi.localIP());
  } else if (!connected && wasConnected) {
    Serial.println("Wi-Fi: LOST connection");
  }
  wasConnected = connected;

  if (!connected && millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) {
    Serial.println("Wi-Fi: retrying...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttemptMs = millis();
  }
}

// Sends one JSON body to SERVER_URL. Returns the HTTP status code (negative = no connection).
int postJson(const String& body) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  const bool useHttps = String(SERVER_URL).startsWith("https://");
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setTimeout(5000);

  bool started;
  if (useHttps) {
    if (strlen(SERVER_ROOT_CA) < 10) {
      Serial.println("HTTPS needs SERVER_ROOT_CA in secrets.h. Not sending.");
      return -2;
    }
    secureClient.setCACert(SERVER_ROOT_CA);  // check the server's certificate (never setInsecure)
    started = http.begin(secureClient, SERVER_URL);
  } else {
    started = http.begin(plainClient, SERVER_URL);
  }
  if (!started) return -3;

  http.addHeader("Content-Type", "application/json");
  if (strlen(API_TOKEN) > 0) {
    http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  }

  const int code = http.POST(body);
  http.end();
  return code;
}

void forwardToServer(const String& json) {
  const int code = postJson(json);
  const bool ok = (code >= 200 && code < 300);
  if (ok) postsOk++; else postsFailed++;

  Serial.print("  -> POST ");
  Serial.print(ok ? "OK " : "FAILED ");
  Serial.print(code);
  if (code == -1) Serial.print(" (no Wi-Fi; reading dropped, buffering comes later)");
  Serial.println();
}

// ---------------------------------------------------------------- handling packets

void handlePacket(const String& packet, int rssi, float snr) {
  packetsReceived++;
  Serial.println();
  Serial.print("RX: ");
  Serial.print(packet);
  Serial.print("  RSSI ");
  Serial.print(rssi);
  Serial.print(" dBm  SNR ");
  Serial.println(snr, 1);

  String nodeStr, seqStr, tempStr, presStr, alertStr;
  if (!getField(packet, "N", nodeStr) || !getField(packet, "SEQ", seqStr) ||
      !getField(packet, "T", tempStr) || !getField(packet, "P", presStr)) {
    packetsMalformed++;
    Serial.println("  REJECTED: malformed packet");
    return;
  }
  getField(packet, "A", alertStr);  // optional

  const int nodeId = nodeStr.toInt();
  const long seq = seqStr.toInt();
  const float tempC = tempStr.toFloat();
  const float pressureHpa = presStr.toFloat();
  const bool alert = (alertStr == "1");

  // Gap detection only (NOT replay protection yet): if SEQ jumps, packets were lost.
  // A lower SEQ usually means the node rebooted, so we just start counting again.
  if (lastSeqFromNode >= 0 && seq > lastSeqFromNode + 1) {
    const uint32_t gap = seq - lastSeqFromNode - 1;
    packetsMissed += gap;
    Serial.print("  NOTE: ");
    Serial.print(gap);
    Serial.println(" packet(s) missed");
  } else if (lastSeqFromNode >= 0 && seq <= lastSeqFromNode) {
    Serial.println("  NOTE: SEQ went backwards (node reboot or duplicate)");
  }
  lastSeqFromNode = seq;

  char json[256];
  snprintf(json, sizeof(json),
           "{\"node_id\":%d,\"seq\":%ld,\"type\":\"reading\",\"temp_c\":%.2f,"
           "\"pressure_hpa\":%.1f,\"alert\":%s,\"rssi_dbm\":%d,\"snr_db\":%.1f,"
           "\"verdict\":\"unverified\"}",
           nodeId, seq, tempC, pressureHpa, alert ? "true" : "false", rssi, snr);
  forwardToServer(json);

  Serial.printf("  totals: rx=%lu malformed=%lu missed=%lu postOK=%lu postFail=%lu\n",
                (unsigned long)packetsReceived, (unsigned long)packetsMalformed,
                (unsigned long)packetsMissed, (unsigned long)postsOk, (unsigned long)postsFailed);
}

// "fake" command: tests the Wi-Fi -> server -> Grafana path before the 2nd radio arrives.
// The reading is clearly labelled as TEST data so nobody mistakes it for a real measurement.
void sendFakeTestReading() {
  Serial.println("Sending a TEST reading (not real sensor data)...");
  forwardToServer("{\"node_id\":0,\"seq\":0,\"type\":\"test\",\"temp_c\":0.0,"
                  "\"pressure_hpa\":0.0,\"alert\":false,\"verdict\":\"test\"}");
}

void checkSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "fake") {
    sendFakeTestReading();
  } else if (cmd.length() > 0) {
    Serial.println("Unknown command. Available: fake");
  }
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  iotStartSerial("SecureLoRa GATEWAY");
  iotPrintDeviceId();

  // The OLED (if still wired from the labs) shares the SPI bus: keep it deselected.
  pinMode(IoTPins::OLED_CS, OUTPUT);
  digitalWrite(IoTPins::OLED_CS, HIGH);
  pinMode(IoTPins::LORA_CS, OUTPUT);
  digitalWrite(IoTPins::LORA_CS, HIGH);

  iotInitSPI();
  loraOk = initLoRa();
  Serial.println(loraOk ? "LoRa OK (915 MHz), listening..."
                        : "LoRa INIT FAILED (check wiring: CS 14, RST 26, DIO0 25)");

  startWiFi();
  Serial.println("Type 'fake' + Enter to POST a test reading to the server.");
}

void loop() {
  keepWiFiAlive();
  checkSerialCommands();

  if (loraOk) {
    const int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      String packet;
      while (LoRa.available()) {
        packet += (char)LoRa.read();
      }
      handlePacket(packet, LoRa.packetRssi(), LoRa.packetSnr());
    }
  }
}
