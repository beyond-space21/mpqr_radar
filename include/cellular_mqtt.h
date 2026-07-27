#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include "radar.h"

class CellularMqtt {
 public:
  CellularMqtt(SoftwareSerial& modemSerial);

  bool begin();
  bool ensureConnected();
  bool publishTelemetry(const RadarReading& reading);
  bool publishStatus(const char* status);
  void loop();
  bool ready() const { return modemReady_; }

 private:
  SoftwareSerial& modemSerial_;
  TinyGsm modem_;
  TinyGsmClient gsmClient_;
  PubSubClient mqtt_;

  bool modemReady_;
  uint32_t lastReconnectAttempt_;

  void powerPulse();
  bool probeAt();
  bool findBaudAndLock();
  bool connectGprs();
  bool connectMqtt();
  void copyProgmem(char* dest, size_t destLen, const char* src);
};
