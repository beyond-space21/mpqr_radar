#include "cellular_mqtt.h"
#include "config.h"

CellularMqtt::CellularMqtt(SoftwareSerial& modemSerial)
    : modemSerial_(modemSerial),
      modem_(modemSerial_),
      gsmClient_(modem_),
      mqtt_(gsmClient_),
      modemReady_(false),
      lastReconnectAttempt_(0) {}

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
  delay(1200);  // SIM7600 needs ~1s active-low
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  Serial.println(F("Modem: wait boot (~12s)"));
  delay(12000);
}

bool CellularMqtt::probeAt() {
  modemSerial_.listen();
  while (modemSerial_.available()) {
    modemSerial_.read();
  }
  modemSerial_.println(F("AT"));
  const uint32_t deadline = millis() + 1000;
  char buf[32];
  uint8_t n = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    if (!modemSerial_.available()) {
      continue;
    }
    const char c = (char)modemSerial_.read();
    if (n + 1 < sizeof(buf)) {
      buf[n++] = c;
      buf[n] = '\0';
    }
    if (n >= 2 && strstr(buf, "OK") != nullptr) {
      return true;
    }
  }
  return false;
}

bool CellularMqtt::findBaudAndLock() {
  static const uint32_t kRates[] = {115200, 57600, 38400, 19200, 9600};

  for (uint8_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
    const uint32_t rate = kRates[i];
    Serial.print(F("Modem: try "));
    Serial.println(rate);
    modemSerial_.begin(rate);
    delay(200);
    if (!probeAt()) {
      // SoftSerial is flaky at 115200 — one more try
      if (!probeAt()) {
        continue;
      }
    }

    Serial.print(F("Modem: AT OK @ "));
    Serial.println(rate);

    if (rate != MODEM_BAUD) {
      Serial.print(F("Modem: lock UART to "));
      Serial.println(MODEM_BAUD);
      modemSerial_.print(F("AT+IPR="));
      modemSerial_.println(MODEM_BAUD);
      delay(300);
      modemSerial_.begin(MODEM_BAUD);
      delay(200);
      // Persist optional — ignore failure on clones
      modemSerial_.println(F("AT&W"));
      delay(200);
      if (!probeAt() && !probeAt()) {
        Serial.println(F("Modem: lock failed, stay at found baud"));
        modemSerial_.begin(rate);
        delay(200);
      }
    }
    return true;
  }
  return false;
}

bool CellularMqtt::begin() {
  modemReady_ = false;
  modemSerial_.listen();

  Serial.println(F("Modem: bring-up"));

  // First try without PWRKEY (module may already be on)
  if (!findBaudAndLock()) {
    if (MODEM_HAS_PWRKEY) {
      powerPulse();
      if (!findBaudAndLock()) {
        Serial.println(F("Modem: no AT response"));
        return false;
      }
    } else {
      Serial.println(F("Modem: no AT response"));
      Serial.println(F("Check: TX/RX swap, GND, 5V supply, baud, set MODEM_HAS_PWRKEY"));
      return false;
    }
  }

  Serial.println(F("Modem: init"));
  if (!modem_.init()) {
    Serial.println(F("Modem: init failed, trying restart"));
    if (!modem_.restart()) {
      Serial.println(F("Modem: restart failed"));
      return false;
    }
  }

  String info = modem_.getModemInfo();
  Serial.print(F("Modem: "));
  Serial.println(info);

  modemReady_ = true;

  char broker[48];
  copyProgmem(broker, sizeof(broker), MQTT_BROKER);
  mqtt_.setServer(broker, MQTT_PORT);
  mqtt_.setKeepAlive(60);
  return true;
}

bool CellularMqtt::connectGprs() {
  if (modem_.isNetworkConnected() && modem_.isGprsConnected()) {
    return true;
  }

  Serial.print(F("Waiting for network"));
  if (!modem_.waitForNetwork(60000L)) {
    Serial.println(F(" fail"));
    return false;
  }
  Serial.println(F(" OK"));

  char apn[32];
  char user[16];
  char pass[16];
  copyProgmem(apn, sizeof(apn), APN);
  copyProgmem(user, sizeof(user), GPRS_USER);
  copyProgmem(pass, sizeof(pass), GPRS_PASS);

  Serial.print(F("GPRS "));
  Serial.print(apn);
  if (!modem_.gprsConnect(apn, user, pass)) {
    Serial.println(F(" fail"));
    return false;
  }
  Serial.println(F(" OK"));
  return true;
}

bool CellularMqtt::connectMqtt() {
  if (mqtt_.connected()) {
    return true;
  }

  char clientId[24];
  char user[24];
  char pass[24];
  copyProgmem(clientId, sizeof(clientId), MQTT_CLIENT_ID);
  copyProgmem(user, sizeof(user), MQTT_USER);
  copyProgmem(pass, sizeof(pass), MQTT_PASS);

  Serial.print(F("MQTT "));
  Serial.print(clientId);

  bool ok;
  if (user[0] != '\0') {
    ok = mqtt_.connect(clientId, user, pass);
  } else {
    ok = mqtt_.connect(clientId);
  }

  if (!ok) {
    Serial.println(F(" fail"));
    return false;
  }
  Serial.println(F(" OK"));
  publishStatus("online");
  return true;
}

bool CellularMqtt::ensureConnected() {
  if (modemReady_ && mqtt_.connected()) {
    return true;
  }

  const uint32_t now = millis();
  if (now - lastReconnectAttempt_ < MODEM_RECONNECT_MS) {
    return false;
  }
  lastReconnectAttempt_ = now;

  if (!modemReady_) {
    if (!begin()) {
      return false;
    }
  }

  if (!connectGprs()) {
    return false;
  }
  return connectMqtt();
}

bool CellularMqtt::publishStatus(const char* status) {
  if (!mqtt_.connected()) {
    return false;
  }
  char topic[40];
  copyProgmem(topic, sizeof(topic), TOPIC_STATUS);
  return mqtt_.publish(topic, status, true);
}

bool CellularMqtt::publishTelemetry(const RadarReading& reading) {
  if (!mqtt_.connected() || !reading.valid) {
    return false;
  }

  char payload[160];
  const int n = snprintf(
      payload, sizeof(payload),
      "{\"vol\":%lu,\"flow\":%.3f,\"lvl\":%u,\"vel\":%.2f,\"empty\":%u,\"dir\":%u}",
      (unsigned long)reading.volume_m3, (double)reading.flow_m3s, reading.level_mm,
      (double)reading.velocity_ms, reading.empty_height_mm, reading.flow_direction);

  if (n <= 0 || n >= (int)sizeof(payload)) {
    return false;
  }

  char topic[40];
  copyProgmem(topic, sizeof(topic), TOPIC_TELEMETRY);
  const bool ok = mqtt_.publish(topic, payload);
  Serial.print(F("Pub "));
  Serial.println(ok ? payload : "fail");
  return ok;
}

void CellularMqtt::loop() {
  if (mqtt_.connected()) {
    mqtt_.loop();
  }
}
