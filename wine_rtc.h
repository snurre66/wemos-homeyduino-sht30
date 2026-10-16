#ifndef WINE_RTC_H
#define WINE_RTC_H

#include <Arduino.h>

// --- RTC User Memory Layout (survives soft resets & OTA updates) ---
#define RTC_WINE_MAGIC 0x57494E45                   // "WINE"
#define RTC_USER_OFFSET 64                          // First safe 4-byte block in ESP8266 RTC user memory

struct RtcWineData {
  uint32_t magic;
  float liquidTemp;
  uint32_t checksum;
};

static inline uint32_t calculateRtcChecksum(const RtcWineData& data) {
  uint32_t tempBits;
  memcpy(&tempBits, &data.liquidTemp, sizeof(float));
  return data.magic ^ tempBits ^ 0xAA55AA55;
}

static inline bool loadWineStateFromRtc(float& tempOut) {
  RtcWineData data;
  if (ESP.rtcUserMemoryRead(RTC_USER_OFFSET, (uint32_t*)&data, sizeof(data))) {
    if (data.magic == RTC_WINE_MAGIC && data.checksum == calculateRtcChecksum(data)) {
      if (!isnan(data.liquidTemp) && data.liquidTemp > -10.0f && data.liquidTemp < 50.0f) {
        tempOut = data.liquidTemp;
        return true;
      }
    }
  }
  return false;
}

static inline void saveWineStateToRtc(float temp) {
  RtcWineData data;
  data.magic = RTC_WINE_MAGIC;
  data.liquidTemp = temp;
  data.checksum = calculateRtcChecksum(data);
  ESP.rtcUserMemoryWrite(RTC_USER_OFFSET, (uint32_t*)&data, sizeof(data));
}

#endif // WINE_RTC_H
