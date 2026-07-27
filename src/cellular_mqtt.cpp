#include "cellular_mqtt.h"
#include "config.h"

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
  Serial.println(F("Modem: PWRKEY pulse"));
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  delay(1200);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  Serial.println(F("Modem: wait boot (~12s)"));
  delay(12000);
}

void CellularMqtt::flushInput() {
  while (ser_.available()) {
    ser_.read();
  }
}

void CellularMqtt::sendLine(const __FlashStringHelper* c) {
  Serial.print(F(">> "));
  Serial.println(c);
  ser_.println(c);
}

uint8_t CellularMqtt::waitFor(const char* expect, uint32_t timeoutMs) {
  char buf[96];
  uint8_t n = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    while (ser_.available()) {
      const char c = (char)ser_.read();
      Serial.write(c);  // mirror everything so failures are visible
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

bool CellularMqtt::cmd(const __FlashStringHelper* c, const char* expect, uint32_t timeoutMs) {
  flushInput();
  sendLine(c);
  return waitFor(expect, timeoutMs) == 1;
}

bool CellularMqtt::findBaud() {
  static const uint32_t kRates[] = {9600, 115200, 57600, 38400, 19200};

  for (uint8_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
    const uint32_t rate = kRates[i];
    Serial.print(F("Modem: try "));
    Serial.println(rate);
    ser_.begin(rate);
    delay(150);
    flushInput();
    ser_.println(F("AT"));
    if (waitFor("OK", 1200) != 1) {
      flushInput();
      ser_.println(F("AT"));
      if (waitFor("OK", 1200) != 1) {
        continue;
      }
    }
    Serial.print(F("\nModem: AT OK @ "));
    Serial.println(rate);

    if (rate != MODEM_BAUD) {
      Serial.print(F("Modem: set baud "));
      Serial.println(MODEM_BAUD);
      ser_.print(F("AT+IPR="));
      ser_.println(MODEM_BAUD);
      delay(400);
      ser_.begin(MODEM_BAUD);
      delay(300);
      flushInput();
      ser_.println(F("AT"));
      if (waitFor("OK", 1500) != 1) {
        Serial.println(F("Modem: relock failed, staying at found rate"));
        ser_.begin(rate);
        delay(200);
      }
    }
    return true;
  }
  return false;
}

bool CellularMqtt::begin() {
  modemReady_ = false;
  mqttUp_ = false;
  ser_.listen();

  Serial.println(F("Modem: bring-up"));

  if (!findBaud()) {
    if (MODEM_HAS_PWRKEY) {
      powerPulse();
      if (!findBaud()) {
        Serial.println(F("Modem: no AT response"));
        return false;
      }
    } else {
      Serial.println(F("Modem: no AT response (TX/RX? GND? power?)"));
      return false;
    }
  }

  cmd(F("ATE0"), "OK", 2000);          // echo off
  cmd(F("AT+CMEE=2"), "OK", 2000);     // verbose errors

  // SIM state — must be READY
  if (!cmd(F("AT+CPIN?"), "READY", 10000)) {
    Serial.println(F("\n!! SIM not READY (inserted? PIN lock?)"));
    return false;
  }

  cmd(F("AT+CSQ"), "OK", 5000);        // signal, mirrored for visibility
  cmd(F("AT+COPS?"), "OK", 10000);     // current operator

  modemReady_ = true;
  return true;
}

bool CellularMqtt::waitRegistration() {
  Serial.println(F("Net: waiting for registration"));
  const uint32_t deadline = millis() + NETWORK_WAIT_MS;
  while ((int32_t)(millis() - deadline) < 0) {
    flushInput();
    sendLine(F("AT+CGREG?"));
    // ",1" home / ",5" roaming
    char buf[64];
    uint8_t n = 0;
    const uint32_t lineDeadline = millis() + 3000;
    while ((int32_t)(millis() - lineDeadline) < 0) {
      if (!ser_.available()) {
        continue;
      }
      const char c = (char)ser_.read();
      Serial.write(c);
      if (n < sizeof(buf) - 1 && c != '\r') {
        buf[n++] = c;
        buf[n] = '\0';
      }
    }
    if (strstr(buf, ",1") != nullptr || strstr(buf, ",5") != nullptr) {
      Serial.println(F("\nNet: registered"));
      return true;
    }
    cmd(F("AT+CSQ"), "OK", 3000);
    delay(3000);
  }
  Serial.println(F("\nNet: registration timeout (antenna? coverage? SIM plan?)"));
  return false;
}

bool CellularMqtt::connectData() {
  if (!waitRegistration()) {
    return false;
  }

  char apn[32];
  copyProgmem(apn, sizeof(apn), APN);

  // Define PDP context
  flushInput();
  Serial.print(F(">> AT+CGDCONT=1,\"IP\",\""));
  Serial.print(apn);
  Serial.println(F("\""));
  ser_.print(F("AT+CGDCONT=1,\"IP\",\""));
  ser_.print(apn);
  ser_.println(F("\""));
  if (waitFor("OK", 5000) != 1) {
    return false;
  }

  if (!cmd(F("AT+CGATT=1"), "OK", 30000)) {
    Serial.println(F("\nGPRS attach failed"));
    return false;
  }

  // Activate PDP (needed before NETOPEN on many firmwares)
  cmd(F("AT+CGACT=1,1"), "OK", 30000);

  // Open TCP/IP stack — required before CMQTTCONNECT
  flushInput();
  sendLine(F("AT+NETOPEN"));
  // Already open → +NETOPEN: 1  /  success → +NETOPEN: 0
  const uint8_t net = waitFor("+NETOPEN:", 60000);
  if (net == 0) {
    Serial.println(F("\nNETOPEN timeout"));
    return false;
  }
  // Drain OK line if any
  waitFor("OK", 2000);

  // Show assigned IP (proves data path really works)
  cmd(F("AT+IPADDR"), "OK", 10000);
  Serial.println(F("\nData: ready"));
  return true;
}

void CellularMqtt::teardownMqtt() {
  cmd(F("AT+CMQTTDISC=0,60"), "OK", 10000);
  cmd(F("AT+CMQTTREL=0"), "OK", 5000);
  cmd(F("AT+CMQTTSTOP"), "OK", 10000);
  mqttUp_ = false;
}

bool CellularMqtt::connectMqtt() {
  // Start service (tolerate "already started")
  flushInput();
  sendLine(F("AT+CMQTTSTART"));
  waitFor("+CMQTTSTART: 0", 12000);

  // Acquire client (tolerate "already acquired")
  char clientId[24];
  copyProgmem(clientId, sizeof(clientId), MQTT_CLIENT_ID);
  flushInput();
  Serial.print(F(">> AT+CMQTTACCQ=0,\""));
  Serial.print(clientId);
  Serial.println(F("\""));
  ser_.print(F("AT+CMQTTACCQ=0,\""));
  ser_.print(clientId);
  ser_.println(F("\""));
  waitFor("OK", 5000);

  // Prefer MQTT 3.1.1 (some brokers reject 3.1)
  flushInput();
  sendLine(F("AT+CMQTTCFG=\"version\",0,4"));
  waitFor("OK", 3000);

  // Connect
  char broker[48];
  copyProgmem(broker, sizeof(broker), MQTT_BROKER);
  flushInput();
  Serial.print(F(">> AT+CMQTTCONNECT=0,\"tcp://"));
  Serial.print(broker);
  Serial.print(':');
  Serial.print(MQTT_PORT);
  Serial.println(F("\",60,1"));
  ser_.print(F("AT+CMQTTCONNECT=0,\"tcp://"));
  ser_.print(broker);
  ser_.print(':');
  ser_.print(MQTT_PORT);
  ser_.println(F("\",60,1"));

  if (waitFor("+CMQTTCONNECT: 0,0", 45000) != 1) {
    Serial.println(F("\nMQTT fail (0,6 = no CONNACK: broker unreachable / port blocked)"));
    Serial.println(F("Hint: Airtel often blocks :1883 — try :1884 or a public test broker"));
    teardownMqtt();
    return false;
  }

  Serial.println(F("\nMQTT connected"));
  mqttUp_ = true;
  publishStatus("online");
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

bool CellularMqtt::publishStatus(const char* status) {
  if (!mqttUp_) {
    return false;
  }
  char topic[40];
  copyProgmem(topic, sizeof(topic), TOPIC_STATUS);
  return mqttPublish(topic, status, true);
}

bool CellularMqtt::publishTelemetry(const RadarReading& reading) {
  if (!mqttUp_ || !reading.valid) {
    return false;
  }

  // AVR libc snprintf has no %f by default — use dtostrf
  char flowStr[12];
  char velStr[12];
  dtostrf(reading.flow_m3s, 0, 3, flowStr);
  dtostrf(reading.velocity_ms, 0, 2, velStr);

  char payload[160];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"vol\":%lu,\"flow\":%s,\"lvl\":%u,\"vel\":%s,\"empty\":%u,\"dir\":%u}",
      (unsigned long)reading.volume_m3, flowStr, reading.level_mm, velStr,
      reading.empty_height_mm, reading.flow_direction);

  if (n <= 0 || n >= (int)sizeof(payload)) {
    return false;
  }

  char topic[40];
  copyProgmem(topic, sizeof(topic), TOPIC_TELEMETRY);
  const bool ok = mqttPublish(topic, payload, false);
  Serial.print(F("\nPub "));
  Serial.println(ok ? payload : "fail");
  return ok;
}

void CellularMqtt::loop() {
  // Modem handles MQTT keepalive internally; nothing to do here.
}
