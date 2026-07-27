#include "cellular_mqtt.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

CellularMqtt::CellularMqtt(SoftwareSerial& modemSerial)
    : ser_(modemSerial), modemReady_(false), mqttUp_(false), lastAttempt_(0) {}

void CellularMqtt::copyProgmem(char* dest, size_t destLen, const char* src) {
  if (destLen == 0) {
    return;
  }
  strncpy_P(dest, src, destLen - 1);
  dest[destLen - 1] = '\0';
}

void CellularMqtt::powerPulse() {
  if (!MODEM_HAS_PWRKEY) {
    return;
  }
  Serial.println(F("[modem] PWRKEY"));
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  delay(1200);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  delay(12000);
}

void CellularMqtt::flushInput() {
  while (ser_.available()) {
    ser_.read();
  }
}

void CellularMqtt::sendRaw(const __FlashStringHelper* cmd) {
  ser_.println(cmd);
}

uint8_t CellularMqtt::waitFor(const char* expect, uint32_t timeoutMs) {
  char buf[96];
  uint8_t n = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    while (ser_.available()) {
      const char c = (char)ser_.read();
      if (c == '\r') {
        continue;
      }
      if (n >= sizeof(buf) - 1) {
        memmove(buf, buf + 32, n - 32);
        n -= 32;
      }
      buf[n++] = c;
      buf[n] = '\0';
      if (strstr(buf, expect) != nullptr) {
        return 1;
      }
      if (strstr(buf, "ERROR") != nullptr) {
        return 2;
      }
    }
  }
  return 0;
}

bool CellularMqtt::atCmd(const __FlashStringHelper* cmd, const char* expect, uint32_t timeoutMs) {
  flushInput();
  sendRaw(cmd);
  return waitFor(expect, timeoutMs) == 1;
}

bool CellularMqtt::probeAt() {
  flushInput();
  ser_.println(F("AT"));
  if (waitFor("OK", 1200) == 1) {
    return true;
  }
  flushInput();
  ser_.println(F("AT"));
  return waitFor("OK", 1200) == 1;
}

bool CellularMqtt::findBaud() {
  // Prefer configured baud first, then common SIM7600 rates
  static const uint32_t kRates[] = {9600, 115200, 57600, 38400, 19200};

  for (uint8_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
    const uint32_t rate = kRates[i];
    ser_.begin(rate);
    delay(150);
    if (!probeAt()) {
      continue;
    }

    Serial.print(F("[modem] AT OK @ "));
    Serial.println(rate);

    if (rate != MODEM_BAUD) {
      ser_.print(F("AT+IPR="));
      ser_.println(MODEM_BAUD);
      delay(400);
      ser_.begin(MODEM_BAUD);
      delay(300);
      if (!probeAt()) {
        Serial.println(F("[modem] lock to 9600 failed; staying at found baud"));
        ser_.begin(rate);
        delay(200);
      } else {
        Serial.print(F("[modem] UART locked "));
        Serial.println(MODEM_BAUD);
      }
    } else {
      Serial.print(F("[modem] UART "));
      Serial.println(MODEM_BAUD);
    }
    return true;
  }
  return false;
}

bool CellularMqtt::begin() {
  modemReady_ = false;
  mqttUp_ = false;
  ser_.listen();

  Serial.println(F("[modem] probing UART"));
  if (!findBaud()) {
    if (MODEM_HAS_PWRKEY) {
      powerPulse();
      if (!findBaud()) {
        Serial.println(F("[modem] no AT (check D2<-TX D3->RX GND)"));
        return false;
      }
    } else {
      Serial.println(F("[modem] no AT (check D2<-TX D3->RX GND)"));
      return false;
    }
  }

  atCmd(F("ATE0"), "OK", 2000);
  atCmd(F("AT+CMEE=2"), "OK", 2000);

  if (!atCmd(F("AT+CPIN?"), "READY", 10000)) {
    Serial.println(F("[modem] SIM not ready"));
    return false;
  }
  Serial.println(F("[modem] SIM ready"));

  modemReady_ = true;
  return true;
}

bool CellularMqtt::waitRegistration() {
  Serial.println(F("[modem] waiting for network"));
  const uint32_t deadline = millis() + NETWORK_WAIT_MS;
  while ((int32_t)(millis() - deadline) < 0) {
    flushInput();
    sendRaw(F("AT+CGREG?"));
    char buf[64];
    uint8_t n = 0;
    const uint32_t lineDeadline = millis() + 3000;
    while ((int32_t)(millis() - lineDeadline) < 0) {
      if (!ser_.available()) {
        continue;
      }
      const char c = (char)ser_.read();
      if (n < sizeof(buf) - 1 && c != '\r') {
        buf[n++] = c;
        buf[n] = '\0';
      }
    }
    if (strstr(buf, ",1") != nullptr || strstr(buf, ",5") != nullptr) {
      Serial.println(F("[modem] network registered"));
      return true;
    }
    delay(3000);
  }
  Serial.println(F("[modem] network timeout"));
  return false;
}

bool CellularMqtt::connectData() {
  if (!waitRegistration()) {
    return false;
  }

  char apn[32];
  copyProgmem(apn, sizeof(apn), APN);

  flushInput();
  ser_.print(F("AT+CGDCONT=1,\"IP\",\""));
  ser_.print(apn);
  ser_.println(F("\""));
  if (waitFor("OK", 5000) != 1) {
    Serial.println(F("[modem] APN config failed"));
    return false;
  }

  if (!atCmd(F("AT+CGATT=1"), "OK", 30000)) {
    Serial.println(F("[modem] GPRS attach failed"));
    return false;
  }

  atCmd(F("AT+CGACT=1,1"), "OK", 30000);

  flushInput();
  sendRaw(F("AT+NETOPEN"));
  if (waitFor("+NETOPEN:", 60000) == 0) {
    Serial.println(F("[modem] NETOPEN timeout"));
    return false;
  }
  waitFor("OK", 2000);

  Serial.println(F("[modem] data ready"));
  return true;
}

void CellularMqtt::teardownMqtt() {
  atCmd(F("AT+CMQTTDISC=0,60"), "OK", 10000);
  atCmd(F("AT+CMQTTREL=0"), "OK", 5000);
  atCmd(F("AT+CMQTTSTOP"), "OK", 10000);
  mqttUp_ = false;
}

bool CellularMqtt::connectMqtt() {
  flushInput();
  sendRaw(F("AT+CMQTTSTART"));
  waitFor("+CMQTTSTART: 0", 12000);

  char clientId[24];
  copyProgmem(clientId, sizeof(clientId), MQTT_CLIENT_ID);
  flushInput();
  ser_.print(F("AT+CMQTTACCQ=0,\""));
  ser_.print(clientId);
  ser_.println(F("\""));
  waitFor("OK", 5000);

  atCmd(F("AT+CMQTTCFG=\"version\",0,4"), "OK", 3000);

  char broker[48];
  copyProgmem(broker, sizeof(broker), MQTT_BROKER);
  flushInput();
  ser_.print(F("AT+CMQTTCONNECT=0,\"tcp://"));
  ser_.print(broker);
  ser_.print(':');
  ser_.print(MQTT_PORT);
  ser_.println(F("\",60,1"));

  if (waitFor("+CMQTTCONNECT: 0,0", 45000) != 1) {
    Serial.println(F("[mqtt] connect failed"));
    teardownMqtt();
    return false;
  }

  Serial.print(F("[mqtt] connected "));
  Serial.print(broker);
  Serial.print(':');
  Serial.println(MQTT_PORT);
  mqttUp_ = true;
  return true;
}

bool CellularMqtt::ensureConnected() {
  if (modemReady_ && mqttUp_) {
    return true;
  }

  const uint32_t now = millis();
  if (now - lastAttempt_ < MODEM_RECONNECT_MS) {
    return false;
  }
  lastAttempt_ = now;

  ser_.listen();

  if (!modemReady_ && !begin()) {
    return false;
  }
  if (!connectData()) {
    return false;
  }
  return connectMqtt();
}

bool CellularMqtt::mqttPublish(const char* topic, const char* payload, bool retain) {
  const size_t topicLen = strlen(topic);
  const size_t payloadLen = strlen(payload);

  flushInput();
  ser_.print(F("AT+CMQTTTOPIC=0,"));
  ser_.println(topicLen);
  if (waitFor(">", 5000) != 1) {
    mqttUp_ = false;
    return false;
  }
  ser_.print(topic);
  if (waitFor("OK", 5000) != 1) {
    mqttUp_ = false;
    return false;
  }

  ser_.print(F("AT+CMQTTPAYLOAD=0,"));
  ser_.println(payloadLen);
  if (waitFor(">", 5000) != 1) {
    mqttUp_ = false;
    return false;
  }
  ser_.print(payload);
  if (waitFor("OK", 5000) != 1) {
    mqttUp_ = false;
    return false;
  }

  ser_.print(F("AT+CMQTTPUB=0,1,60"));
  if (retain) {
    ser_.print(F(",1"));
  }
  ser_.println();
  if (waitFor("+CMQTTPUB: 0,0", 30000) != 1) {
    mqttUp_ = false;
    return false;
  }
  return true;
}

bool CellularMqtt::queryCsq(uint8_t& rssi, uint8_t& ber) {
  rssi = 99;
  ber = 99;
  flushInput();
  sendRaw(F("AT+CSQ"));

  char buf[48];
  uint8_t n = 0;
  const uint32_t deadline = millis() + 3000;
  while ((int32_t)(millis() - deadline) < 0) {
    while (ser_.available()) {
      const char c = (char)ser_.read();
      if (c == '\r') {
        continue;
      }
      if (n < sizeof(buf) - 1) {
        buf[n++] = c;
        buf[n] = '\0';
      }
      if (strstr(buf, "OK") != nullptr || strstr(buf, "ERROR") != nullptr) {
        char* p = strstr(buf, "+CSQ:");
        if (p != nullptr) {
          int a = 99;
          int b = 99;
          if (sscanf(p, "+CSQ: %d,%d", &a, &b) >= 1) {
            rssi = (uint8_t)a;
            ber = (uint8_t)b;
            return true;
          }
        }
        return false;
      }
    }
  }
  return false;
}

void CellularMqtt::fillModemFields(TelemetryPayload& payload) {
  uint8_t rssi = 99;
  uint8_t ber = 99;
  queryCsq(rssi, ber);
  payload.csq = rssi;
  payload.ber = ber;
}

bool CellularMqtt::publishTelemetry(const TelemetryPayload& payload) {
  if (!mqttUp_) {
    return false;
  }

  char json[280];
  if (formatTelemetryJson(json, sizeof(json), payload) < 0) {
    Serial.println(F("[mqtt] payload too large"));
    return false;
  }

  char topic[40];
  copyProgmem(topic, sizeof(topic), TOPIC_TELEMETRY);
  if (!mqttPublish(topic, json, false)) {
    return false;
  }

  copyProgmem(topic, sizeof(topic), TOPIC_STATUS);
  mqttPublish(topic, payload.mst, true);
  return true;
}

void CellularMqtt::loop() {}
