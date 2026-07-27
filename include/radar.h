#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>

// Measurement snapshot from RS-RAD-N01-3 (Modbus holding/input regs §4.3)
struct RadarReading {
  uint32_t volume_m3;       // cumulative water volume (m³)
  float flow_m3s;           // instantaneous flow (m³/s), register ×0.001
  uint16_t level_mm;        // water level (mm)
  float velocity_ms;        // surface velocity (m/s), register cm/s ÷ 100
  uint16_t empty_height_mm; // empty height / air gap (mm), reg 0x0401
  uint8_t flow_direction;   // 0 = with stream, 1 = against stream
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

  void setTransmit(bool tx);
  uint16_t crc16(const uint8_t* data, uint8_t len);
  bool readRegisters(uint16_t startAddr, uint16_t count, uint16_t* dest);
};
