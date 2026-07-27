#include <Arduino.h>
#include <SoftwareSerial.h>
#include <string.h>

#include "config.h"
#include "telemetry.h"
#include "radar.h"
#include "cellular_mqtt.h"

SoftwareSerial modemSerial(PIN_MODEM_RX, PIN_MODEM_TX);
SoftwareSerial radarSerial(PIN_RS485_RX, PIN_RS485_TX);

RadarSensor radar(radarSerial, PIN_RS485_DE, RADAR_SLAVE_ADDR);
CellularMqtt cellular(modemSerial);

uint32_t lastPublishMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("[boot] mpqr radar"));
  Serial.print(F("[radar] UART "));
  Serial.println(RADAR_BAUD);

  radar.begin(RADAR_BAUD);

  if (!cellular.begin()) {
    Serial.println(F("[modem] init deferred"));
  }

  lastPublishMs = millis() - PUBLISH_INTERVAL_MS;
}

void loop() {
  modemSerial.listen();
  cellular.ensureConnected();
  cellular.loop();

  if (!cellular.mqttConnected()) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastPublishMs < PUBLISH_INTERVAL_MS) {
    return;
  }
  lastPublishMs = now;

  TelemetryPayload t;
  memset(&t, 0, sizeof(t));
  strncpy(t.mst, "ok", sizeof(t.mst) - 1);
  t.csq = 99;
  t.ber = 99;

  radarSerial.listen();
  delay(50);
  const bool radarOk = radar.readInto(t);
  modemSerial.listen();
  delay(20);

  if (!radarOk) {
    strncpy(t.mst, "radar_err", sizeof(t.mst) - 1);
    Serial.println(F("[radar] read failed"));
  } else {
    Serial.print(F("[radar] lvl="));
    Serial.print(t.lvl);
    Serial.print(F("mm flow="));
    Serial.print(t.flow_x1000 / 1000.0, 3);
    Serial.print(F(" vel="));
    Serial.println(t.vel_cms / 100.0, 2);
  }

  cellular.fillModemFields(t);
  Serial.print(F("[modem] csq="));
  Serial.println(t.csq);

  // Always the same telemetry template
  if (cellular.publishTelemetry(t)) {
    Serial.println(F("[mqtt] telemetry sent"));
  } else {
    Serial.println(F("[mqtt] publish failed"));
  }
}
