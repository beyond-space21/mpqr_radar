#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>
#include "telemetry.h"

class RadarSensor {
 public:
  RadarSensor(SoftwareSerial& bus, uint8_t dePin, uint8_t slaveAddr);

  void begin(uint32_t baud);
  // Fills radar fields in payload; returns true if primary measurement block OK.
  bool readInto(TelemetryPayload& out);

 private:
  SoftwareSerial& bus_;
  uint8_t dePin_;
  uint8_t slaveAddr_;

  void setTransmit(bool tx);
  uint16_t crc16(const uint8_t* data, uint8_t len);
  bool readRegisters(uint16_t startAddr, uint16_t count, uint16_t* dest);
};
