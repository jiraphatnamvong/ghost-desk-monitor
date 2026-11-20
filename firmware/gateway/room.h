// Room sensors on the gateway: SCD41 (CO2, temperature, humidity), BH1750 (lux), MAX4466 (sound level).
#pragma once
#include <Arduino.h>

struct RoomData {
  bool co2Ok, luxOk;
  uint16_t co2_ppm;
  float temp_c, rh;
  float lux;
  uint16_t sound_avg;   // mean peak-to-peak ADC counts over the last minute (relative, not dB)
  uint16_t sound_peak;  // loudest 50 ms window in the last minute
};

void roomBegin();               // I2C + sensors + roomTask (posts a room_env row every minute)
RoomData roomLatest();
