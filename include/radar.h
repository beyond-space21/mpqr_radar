#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>

struct RadarReading {
  uint32_t volume_m3;
  float flow_m3s;
  uint16_t level_mm;
  float velocity_ms;
  uint16_t empty_height_mm;
  uint8_t flow_direction;
  bool valid;
};

class RadarSensor {
 public:
  RadarSensor(SoftwareSerial& bus, uint8_t dePin, uint8_t slaveAddr);

  void begin(uint32_t baud);
  bool read(RadarReading& out);

 private:
  SoftwareSerial& bus_;
  uint8_t dePin_;
  uint8_t slaveAddr_;
  uint32_t baud_;

  void setTransmit(bool tx);
  uint16_t crc16(const uint8_t* data, uint8_t len);
  bool readRegisters(uint8_t func, uint16_t startAddr, uint16_t count, uint16_t* dest);
  bool tryReadAt(uint32_t baud, RadarReading& out);
};
