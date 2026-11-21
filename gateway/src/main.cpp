#include <Arduino.h>
#include <Wire.h>   // not used directly, but iot_common.h needs it (PlatformIO only links what main.cpp includes)
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <Preferences.h>    // ESP32 flash key-value store (NVS): keeps the last accepted SEQ

#include "iot_common.h"     // from ../shared: serial, SPI helpers + kit pin map
#include "lora_settings.h"  // from ../shared: radio settings (must match the node)
#include "lora_diag.h"      // from ../shared: explains a LoRa init failure
#include "packet_auth.h"    // from ../shared: HMAC-SHA256 tag check (needs shared/secret_key.h)
#include "secrets.h"        // YOUR copy of secrets_example.h (not in Git)
#include "power_monitor.h"  // INA219: measures the node's current

// SecureLoRa GATEWAY.
//
//   1. Listen for LoRa packets from the node:  N=1;SEQ=5;T=23.0;H=41.0;A=0;M=<HMAC tag>
//      (H = humidity from the DHT-11; P = pressure is still accepted if a node sends it)
//   2. Check them: format, known node, HMAC tag (authentic), SEQ (fresh, not a replay).
//   3. POST accepted readings as JSON ("verdict":"accepted") to the server over HTTPS.
//      Rejections are counted, printed, and forwarded (rate-limited) as "type":"rejected".
//
// Not done: buffering readings during Wi-Fi/server outages (they are dropped and logged).
//
// Serial Monitor commands:
//   fake      -> POST one clearly-labelled TEST reading to the server
//   resetseq  -> forget the stored last-accepted SEQ (only needed if the node's flash was erased)

constexpr uint32_t WIFI_RETRY_MS = 10000;  // try to reconnect every 10 s if Wi-Fi drops

bool loraOk = false;
uint32_t lastWifiAttemptMs = 0;

// Simple counters, printed with every packet (useful for the 72 h reliability report)
uint32_t packetsReceived = 0;
uint32_t packetsMalformed = 0;
uint32_t postsOk = 0;
uint32_t postsFailed = 0;
uint32_t packetsAccepted = 0;
uint32_t packetsBadSig = 0;
uint32_t packetsReplay = 0;
uint32_t packetsUnknownNode = 0;
uint32_t packetsMissed = 0;    // gaps in SEQ between accepted packets = lost packets

// Replay protection state, persisted in flash
constexpr int KNOWN_NODE_ID = 1;   // the only node we share a key with
Preferences prefs;
long lastAcceptedSeq = -1;         // -1 = nothing accepted yet
String seqKey() { return "seq_n" + String(KNOWN_NODE_ID); }

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

constexpr int POST_NO_WIFI = -100;  // our own code; HTTPClient's negative codes are connection errors

// Sends one JSON body to SERVER_URL. Returns the HTTP status code (negative = no connection).
int postJson(const String& body) {
  if (WiFi.status() != WL_CONNECTED) return POST_NO_WIFI;

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
  if (code == POST_NO_WIFI) Serial.print(" (no Wi-Fi; reading dropped, buffering comes later)");
  else if (code == -1) Serial.print(" (could not connect to server: down, wrong IP, or HTTPS certificate rejected)");
  else if (code < 0) Serial.print(" (connection error, see HTTPClient error codes)");
  Serial.println();
}

// ---------------------------------------------------------------- handling packets

// Checks, in order (each failure is logged with its reason and counted):
//   1. format   - has N, SEQ, T and a ;M= tag              -> else "malformed" (logged only)
//   2. node id  - only nodes we have a key for (node 1)      -> else "unknown_node"
//   3. tag      - HMAC matches the body (needs the key)      -> else "bad_signature"
//   4. SEQ      - greater than the last ACCEPTED SEQ         -> else "replay"
// Only then is the reading "accepted" and forwarded as a real reading.
// The tag is checked BEFORE SEQ: a packet that isn't authentic tells us nothing about SEQ.
// The last accepted SEQ is stored in flash, so a gateway reboot can't be used to replay old packets.

void forwardRejected(int nodeId, long seq, float claimedTemp, const char* verdict, const char* reason,
                     int rssi, float snr) {
  // Rate-limit: at most one rejected event per 5 s goes to the server (a flood of junk packets
  // must not flood the server). Every rejection is still counted and printed here.
  static uint32_t lastForwardMs = 0;
  if (lastForwardMs != 0 && millis() - lastForwardMs < 5000) {
    Serial.println("  (rejection not forwarded: rate limit)");
    return;
  }
  lastForwardMs = millis();
  char json[256];
  snprintf(json, sizeof(json),
           "{\"node_id\":%d,\"seq\":%ld,\"type\":\"rejected\",\"temp_c\":%.2f,\"alert\":false,"
           "\"rssi_dbm\":%d,\"snr_db\":%.1f,\"verdict\":\"%s\",\"reason\":\"%s\"}",
           nodeId, seq, claimedTemp, rssi, snr, verdict, reason);
  forwardToServer(json);
}

void printTotals() {
  Serial.printf("  totals: rx=%lu accepted=%lu bad_sig=%lu replay=%lu unknown=%lu malformed=%lu "
                "missed=%lu postOK=%lu postFail=%lu\n",
                (unsigned long)packetsReceived, (unsigned long)packetsAccepted,
                (unsigned long)packetsBadSig, (unsigned long)packetsReplay,
                (unsigned long)packetsUnknownNode, (unsigned long)packetsMalformed,
                (unsigned long)packetsMissed, (unsigned long)postsOk, (unsigned long)postsFailed);
}

void handlePacket(const String& packet, int rssi, float snr) {
  packetsReceived++;
  Serial.println();
  Serial.print("RX: ");
  Serial.print(packet);
  Serial.print("  RSSI ");
  Serial.print(rssi);
  Serial.print(" dBm  SNR ");
  Serial.println(snr, 1);

  // 1. Format
  String body, tag;
  String nodeStr, seqStr, tempStr, presStr, humStr, alertStr;
  if (!PacketAuth::split(packet, body, tag) || !getField(body, "N", nodeStr) ||
      !getField(body, "SEQ", seqStr) || !getField(body, "T", tempStr)) {
    packetsMalformed++;
    Serial.println("  REJECTED: malformed (missing N/SEQ/T or tag)");
    printTotals();
    return;
  }
  const int nodeId = nodeStr.toInt();
  const long seq = seqStr.toInt();
  const float tempC = tempStr.toFloat();

  // 2. Known node
  if (nodeId != KNOWN_NODE_ID) {
    packetsUnknownNode++;
    Serial.println("  REJECTED: unknown_node " + nodeStr);
    forwardRejected(nodeId, seq, tempC, "unknown_node", "no key for this node id", rssi, snr);
    printTotals();
    return;
  }

  // 3. Authentic? (HMAC tag must match the body)
  String checkedBody;
  if (!PacketAuth::verify(packet, checkedBody)) {
    packetsBadSig++;
    Serial.println("  REJECTED: bad_signature (tag does not match: forged or modified)");
    forwardRejected(nodeId, seq, tempC, "bad_signature", "HMAC tag mismatch", rssi, snr);
    printTotals();
    return;
  }

  // 4. Fresh? (SEQ must be higher than the last accepted one)
  if (lastAcceptedSeq >= 0 && seq <= lastAcceptedSeq) {
    packetsReplay++;
    Serial.printf("  REJECTED: replay (SEQ %ld <= last accepted %ld)\n", seq, lastAcceptedSeq);
    forwardRejected(nodeId, seq, tempC, "replay", "SEQ not greater than last accepted", rssi, snr);
    printTotals();
    return;
  }

  // Accepted. Count lost packets (SEQ jumped), then remember this SEQ in flash.
  if (lastAcceptedSeq >= 0 && seq > lastAcceptedSeq + 1) {
    const uint32_t gap = seq - lastAcceptedSeq - 1;
    packetsMissed += gap;
    Serial.printf("  NOTE: %lu packet(s) missed (SEQ %ld -> %ld)\n", (unsigned long)gap, lastAcceptedSeq, seq);
  }
  lastAcceptedSeq = seq;
  prefs.putLong(seqKey().c_str(), lastAcceptedSeq);
  packetsAccepted++;
  Serial.println("  ACCEPTED (tag OK, SEQ fresh)");

  // Optional fields: only forwarded if the node actually sent them (never invent values)
  const bool hasPressure = getField(body, "P", presStr);
  const bool hasHumidity = getField(body, "H", humStr);
  getField(body, "A", alertStr);
  const bool alert = (alertStr == "1");

  // Build the JSON piece by piece so optional fields are left out when missing
  char json[256];
  int len = snprintf(json, sizeof(json),
                     "{\"node_id\":%d,\"seq\":%ld,\"type\":\"reading\",\"temp_c\":%.2f,",
                     nodeId, seq, tempC);
  if (hasPressure) {
    len += snprintf(json + len, sizeof(json) - len, "\"pressure_hpa\":%.1f,", presStr.toFloat());
  }
  if (hasHumidity) {
    len += snprintf(json + len, sizeof(json) - len, "\"humidity_pct\":%.1f,", humStr.toFloat());
  }
  snprintf(json + len, sizeof(json) - len,
           "\"alert\":%s,\"rssi_dbm\":%d,\"snr_db\":%.1f,\"verdict\":\"accepted\"}",
           alert ? "true" : "false", rssi, snr);
  forwardToServer(json);
  printTotals();
}

// "fake" command: tests the Wi-Fi -> server -> Grafana path before the 2nd radio arrives.
// The reading is clearly labelled as TEST data so nobody mistakes it for a real measurement.
void sendFakeTestReading() {
  Serial.println("Sending a TEST reading (not real sensor data)...");
  forwardToServer("{\"node_id\":0,\"seq\":0,\"type\":\"test\",\"temp_c\":0.0,"
                  "\"alert\":false,\"verdict\":\"test\"}");
}

void checkSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "fake") {
    sendFakeTestReading();
  } else if (cmd == "resetseq") {
    lastAcceptedSeq = -1;
    prefs.remove(seqKey().c_str());
    Serial.println("Stored last-accepted SEQ cleared. The next valid packet will be accepted.");
  } else if (cmd.length() > 0) {
    Serial.println("Unknown command. Available: fake, resetseq");
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

  if (!PacketAuth::loadKey()) {
    Serial.println("HMAC KEY MISSING/INVALID: copy shared/secret_key_example.h to shared/secret_key.h "
                   "(same file as the node). Every packet will be rejected as bad_signature.");
  }
  prefs.begin("securelora-gw", false);
  lastAcceptedSeq = prefs.getLong(seqKey().c_str(), -1);
  Serial.println("Last accepted SEQ from flash: " + String(lastAcceptedSeq));

  iotInitSPI();
  loraOk = initLoRa();
  Serial.println(loraOk ? "LoRa OK (915 MHz), listening..."
                        : "LoRa INIT FAILED (check wiring: CS 14, RST 26, DIO0 25)");
  if (!loraOk) loraDiagnose();

  // I2C (SDA 21 / SCL 22) is only used by the INA219 on the gateway
  iotInitI2C();
  PowerMonitor::begin();

  startWiFi();
  Serial.println("Commands: 'fake' = POST a test reading, 'resetseq' = clear stored SEQ.");
}

void loop() {
  keepWiFiAlive();
  checkSerialCommands();
  PowerMonitor::update();

  if (loraOk) {
    const int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      String packet;
      while (LoRa.available()) {
        packet += (char)LoRa.read();
      }
      const int rssi = LoRa.packetRssi();
      const float snr = LoRa.packetSnr();
      // Start listening again BEFORE handling: handlePacket() can block 1-5 s on the HTTPS POST,
      // and a packet arriving meanwhile would otherwise be lost (seen in the fault tests: a real
      // packet right after a rejected one went missing). The radio keeps the next packet in its
      // buffer and parsePacket() picks it up on the next loop.
      LoRa.receive();
      handlePacket(packet, rssi, snr);
    }
  }
}
