#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>
#include "radar.h"

// Cellular + MQTT using the SIM7600's built-in MQTT client (AT+CMQTT*).
// No TCP stack on the Nano: the modem owns the socket and MQTT session,
// we only exchange short AT commands over SoftwareSerial.
class CellularMqtt {
 public:
  CellularMqtt(SoftwareSerial& modemSerial);

  bool begin();
  bool ensureConnected();
  bool publishTelemetry(const RadarReading& reading);
  bool publishStatus(const char* status);
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
  void sendLine(const __FlashStringHelper* cmd);
  // 1 = expect found, 2 = ERROR found, 0 = timeout. Mirrors modem output to Serial.
  uint8_t waitFor(const char* expect, uint32_t timeoutMs);
  bool cmd(const __FlashStringHelper* c, const char* expect, uint32_t timeoutMs);
  bool findBaud();
  bool waitRegistration();
  bool connectData();
  bool connectMqtt();
  void teardownMqtt();
  bool mqttPublish(const char* topic, const char* payload, bool retain);
  void copyProgmem(char* dest, size_t destLen, const char* src);
};
