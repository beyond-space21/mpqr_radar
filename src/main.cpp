#include <Arduino.h>
#include <SoftwareSerial.h>

#include "config.h"
#include "radar.h"
#include "cellular_mqtt.h"

// Two SoftwareSerials: only one can listen at a time on AVR.
SoftwareSerial modemSerial(PIN_MODEM_RX, PIN_MODEM_TX);
SoftwareSerial radarSerial(PIN_RS485_RX, PIN_RS485_TX);

RadarSensor radar(radarSerial, PIN_RS485_DE, RADAR_SLAVE_ADDR);
CellularMqtt cellular(modemSerial);

uint32_t lastPublishMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\nmpqr_radar — Nano + SIM7600G-H + RS-RAD-N01-3"));

  radar.begin(RADAR_BAUD);

  if (!cellular.begin()) {
    Serial.println(F("Modem bring-up failed; will retry in loop"));
  }

  lastPublishMs = millis() - PUBLISH_INTERVAL_MS;
}

void loop() {
  // Prefer modem RX while idle / MQTT keepalive
  modemSerial.listen();
  cellular.ensureConnected();
  cellular.loop();

  const uint32_t now = millis();
  if (now - lastPublishMs < PUBLISH_INTERVAL_MS) {
    return;
  }
  lastPublishMs = now;

  // Pause modem listen, poll radar over RS485 Modbus
  radarSerial.listen();
  delay(20);

  RadarReading reading;
  const bool ok = radar.read(reading);

  modemSerial.listen();

  if (!ok) {
    Serial.println(F("Radar read failed"));
    cellular.publishStatus("radar_error");
    return;
  }

  Serial.print(F("lvl="));
  Serial.print(reading.level_mm);
  Serial.print(F("mm vel="));
  Serial.print(reading.velocity_ms);
  Serial.print(F("m/s flow="));
  Serial.println(reading.flow_m3s);

  if (cellular.ensureConnected()) {
    cellular.publishTelemetry(reading);
  }
}
