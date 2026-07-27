#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>
#include "telemetry.h"

class CellularMqtt {
 public:
  CellularMqtt(SoftwareSerial& modemSerial);

  bool begin();
  bool ensureConnected();
  bool publishTelemetry(const TelemetryPayload& payload);
  void fillModemFields(TelemetryPayload& payload);
  void loop();
  bool ready() const { return modemReady_; }
  bool mqttConnected() const { return mqttUp_; }

 private:
  SoftwareSerial& ser_;
  bool modemReady_;
  bool mqttUp_;
  uint32_t lastAttempt_;

  void powerPulse();
  void flushInput();
  void sendRaw(const __FlashStringHelper* cmd);
  uint8_t waitFor(const char* expect, uint32_t timeoutMs);
  bool atCmd(const __FlashStringHelper* cmd, const char* expect, uint32_t timeoutMs);
  bool probeAt();
  bool findBaud();
  bool waitRegistration();
  bool connectData();
  bool connectMqtt();
  void teardownMqtt();
  bool mqttPublish(const char* topic, const char* payload, bool retain);
  bool queryCsq(uint8_t& rssi, uint8_t& ber);
  void copyProgmem(char* dest, size_t destLen, const char* src);
};
