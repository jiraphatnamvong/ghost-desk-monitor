// Small raw I2C drivers instead of libraries: each sensor needs two or three commands, and this keeps
// the gateway free of library-version surprises. Datasheets: Sensirion SCD4x, ROHM BH1750FVI.
#include "room.h"
#include <Wire.h>
#include "pins.h"
#include "config.h"
#include "cloud.h"

#define SCD41_ADDR  0x62
#define BH1750_ADDR 0x23

static RoomData s_room = {};
static SemaphoreHandle_t s_mutex;

// ---------- SCD41 ----------

static uint8_t sensirionCrc(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

static bool scdCommand(uint16_t cmd) {
  Wire.beginTransmission(SCD41_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

// Reads `words` 16-bit words, each followed by a CRC byte.
static bool scdRead(uint16_t *out, int words) {
  if (Wire.requestFrom(SCD41_ADDR, words * 3) != words * 3) return false;
  for (int i = 0; i < words; i++) {
    uint8_t b[3] = {(uint8_t)Wire.read(), (uint8_t)Wire.read(), (uint8_t)Wire.read()};
    if (sensirionCrc(b, 2) != b[2]) return false;
    out[i] = (b[0] << 8) | b[1];
  }
  return true;
}

// Single-shot mode, not periodic. Each SCD41 measurement pulses its infrared source for a few seconds, and on
// the shared 3.3 V rail those current pulses reach the MAX4466 (gain up to 125x): in periodic mode the mic read
// full scale for ~1.2 s every 5 s in a silent room (device test 09). With single shots the gateway knows when
// the sensor is busy, so roomTask ignores the mic for that time.
static bool scdBegin() {
  scdCommand(0x3F86);  // stop_periodic_measurement (it keeps running across a reset, e.g. after device test 08)
  delay(500);
  uint16_t serial[3];
  if (!scdCommand(0x3682)) return false;  // get_serial_number: is it there and talking cleanly?
  delay(1);
  return scdRead(serial, 3);
}

static bool scdStartShot() {
  return scdCommand(0x219D);  // measure_single_shot: result ready after SCD_SHOT_MS
}

static bool scdReadIfReady(RoomData &r) {
  uint16_t status;
  if (!scdCommand(0xE4B8)) return false;  // get_data_ready_status
  delay(1);
  if (!scdRead(&status, 1) || (status & 0x07FF) == 0) return false;
  if (!scdCommand(0xEC05)) return false;  // read_measurement
  delay(1);
  uint16_t w[3];
  if (!scdRead(w, 3)) return false;
  r.co2_ppm = w[0];
  r.temp_c = -45.0f + 175.0f * w[1] / 65535.0f;
  r.rh = 100.0f * w[2] / 65535.0f;
  return true;
}

// ---------- BH1750 ----------

static bool bhBegin() {
  Wire.beginTransmission(BH1750_ADDR);
  Wire.write(0x01);  // power on
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(BH1750_ADDR);
  Wire.write(0x10);  // continuous high-resolution mode, 1 lx, ~120 ms
  return Wire.endTransmission() == 0;
}

static bool bhRead(float &lux) {
  if (Wire.requestFrom(BH1750_ADDR, 2) != 2) return false;
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  lux = raw / 1.2f;
  return true;
}

// ---------- MAX4466 ----------

// Peak-to-peak over 50 ms (the Lab 8 method): louder sound swings further around the 1.65 V midpoint.
static uint16_t micWindow() {
  uint16_t lo = 4095, hi = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 50) {
    uint16_t v = analogRead(PIN_MIC);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return hi - lo;
}

// ---------- task ----------

static void roomTask(void *) {
  bool scdOk = scdBegin();
  bool bhOk = bhBegin();
  Serial.printf("[room] SCD41 %s, BH1750 %s\n", scdOk ? "ok" : "NOT FOUND", bhOk ? "ok" : "NOT FOUND");

  uint32_t soundSum = 0, soundN = 0;
  uint16_t soundPeak = 0;
  uint32_t lastI2c = 0, lastPost = millis();
  bool shotBusy = false;   // an SCD41 single shot is running: the mic is not trusted
  uint32_t shotAt = 0;     // when it started
  bool firstShot = true;

  for (;;) {
    if (scdOk && !shotBusy && (firstShot || millis() - shotAt >= SCD_EVERY_MS)) {
      shotAt = millis();
      shotBusy = scdStartShot();
      firstShot = false;
    }

    if (!shotBusy) {
      uint16_t p2p = micWindow();
      soundSum += p2p;
      soundN++;
      if (p2p > soundPeak) soundPeak = p2p;
    }

    if (millis() - lastI2c >= 1000) {
      lastI2c = millis();
      RoomData r = roomLatest();
      // The SCD41 doesn't answer while it measures, so ask only once the shot should be done.
      if (shotBusy && millis() - shotAt >= SCD_SHOT_MS) {
        if (scdReadIfReady(r)) {
          r.co2Ok = true;
          shotBusy = false;
        } else if (millis() - shotAt >= SCD_SHOT_MS + 3000) {
          shotBusy = false;  // no answer: give the mic back, try again at the next shot
          Serial.println("[room] SCD41 single shot gave no reading");
        }
      }
      float lux;
      if (bhRead(lux)) {
        r.lux = lux;
        r.luxOk = true;
      }
      r.sound_avg = soundN ? soundSum / soundN : 0;
      r.sound_peak = soundPeak;
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      s_room = r;
      xSemaphoreGive(s_mutex);
    }

    if (millis() - lastPost >= ROOM_EVERY_MS) {
      lastPost = millis();
      RoomData r = roomLatest();
      String json = "{";
      json += "\"co2_ppm\":" + (r.co2Ok ? String(r.co2_ppm) : String("null"));
      json += ",\"temp_c\":" + (r.co2Ok ? String(r.temp_c, 1) : String("null"));
      json += ",\"rh\":" + (r.co2Ok ? String(r.rh, 1) : String("null"));
      json += ",\"lux\":" + (r.luxOk ? String(r.lux, 0) : String("null"));
      json += ",\"sound_level\":" + String(r.sound_avg);
      json += ",\"sound_peak\":" + String(r.sound_peak);
      json += "}";
      cloudQueueRow("room_env", json);
      Serial.printf("[room] %s\n", json.c_str());
      soundSum = soundN = 0;
      soundPeak = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void roomBegin() {
  s_mutex = xSemaphoreCreateMutex();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);  // full 0-3.3 V range
  xTaskCreatePinnedToCore(roomTask, "room", 4096, nullptr, 2, nullptr, 1);
}

RoomData roomLatest() {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  RoomData r = s_room;
  xSemaphoreGive(s_mutex);
  return r;
}
