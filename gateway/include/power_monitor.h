#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// Measures the NODE's current with the INA219 (the INA219's I2C goes to this gateway board,
// its VIN+/VIN- sit in the node's 5 V power line). Prints one summary line per minute:
//
//   POWER: avg=52.31 mA  min=48.10  max=131.20 mA  bus=5.02 V  samples=860  total=0.87 mAh  uptime=60 s
//
// How it works: the INA219 itself averages 128 samples per reading (~69 ms), so every reading
// already covers a 69 ms slice of time and short LoRa TX bursts are included. We read it about
// every 70 ms and average those readings. While the gateway is busy with an HTTPS POST (~0.5 s)
// we can't read it, so a few slices are skipped. That's a small gap; mention it in the report.

namespace PowerMonitor {

constexpr uint8_t  INA219_ADDR       = 0x40;
constexpr uint32_t SAMPLE_PERIOD_MS  = 70;      // matches the INA219's 128-sample averaging time
constexpr uint32_t REPORT_PERIOD_MS  = 60000;   // one summary line per minute

inline Adafruit_INA219& sensor() {
  static Adafruit_INA219 ina(INA219_ADDR);
  return ina;
}

static bool     ok = false;
static uint32_t lastSampleMs = 0;
static uint32_t lastReportMs = 0;
static uint32_t startMs = 0;

// Current window (reset every report)
static double   sumMa = 0;
static float    minMa = 1e9, maxMa = -1e9;
static uint32_t samples = 0;
static float    lastBusV = 0;

// Whole run
static double   totalMas = 0;   // mA * seconds, converted to mAh when printed

static bool begin() {
  // Assumes Wire is already started on SDA 21 / SCL 22.
  ok = sensor().begin(&Wire);
  if (!ok) {
    Serial.println("INA219 NOT FOUND at 0x40 (check VCC->3V3, GND, SDA->21, SCL->22). Power logging off.");
    return false;
  }
  // 16 V / 400 mA range: 0.05 mA resolution. The node (5 V, well under 400 mA) fits.
  sensor().setCalibration_16V_400mA();
  // Override the config register: 12-bit bus voltage, shunt averaged over 128 samples (~69 ms),
  // continuous. 0x01FF = BRNG 16V | PGA /1 (40 mV) | BADC 12-bit | SADC 128 samples | MODE continuous
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(0x00);
  Wire.write(0x01);
  Wire.write(0xFF);
  Wire.endTransmission();

  startMs = lastReportMs = lastSampleMs = millis();
  Serial.println("INA219 OK: logging node current, one POWER line per minute.");
  return true;
}

inline void printReport() {
  const uint32_t now = millis();
  if (samples == 0) {
    Serial.println("POWER: no samples this minute");
    return;
  }
  const double avg = sumMa / samples;
  Serial.printf("POWER: avg=%.2f mA  min=%.2f  max=%.2f mA  bus=%.2f V  samples=%lu  total=%.3f mAh  uptime=%lu s\n",
                avg, minMa, maxMa, lastBusV, (unsigned long)samples, totalMas / 3600.0,
                (unsigned long)((now - startMs) / 1000));
  if (avg < 1.0) {
    Serial.println("  NOTE: ~0 mA. Is the node powered THROUGH the INA219 (VIN+ -> VIN- -> node)? "
                   "Unplug the node's USB cable, or it bypasses the sensor.");
  }
  sumMa = 0;
  minMa = 1e9;
  maxMa = -1e9;
  samples = 0;
}

// Call every loop(). Non-blocking.
inline void update() {
  if (!ok) return;
  const uint32_t now = millis();
  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    const float dtS = (now - lastSampleMs) / 1000.0f;
    lastSampleMs = now;
    const float mA = sensor().getCurrent_mA();
    lastBusV = sensor().getBusVoltage_V();
    sumMa += mA;
    samples++;
    if (mA < minMa) minMa = mA;
    if (mA > maxMa) maxMa = mA;
    totalMas += mA * dtS;  // charge used since start
  }
  if (now - lastReportMs >= REPORT_PERIOD_MS) {
    lastReportMs = now;
    printReport();
  }
}

}  // namespace PowerMonitor
