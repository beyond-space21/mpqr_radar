#include "radar.h"
#include "config.h"

// RS-RAD-N01-3 register map (manual §4.3)
static const uint16_t REG_VOLUME_HI = 0x03E8;  // + LO, flow×1000, level, velocity

RadarSensor::RadarSensor(SoftwareSerial& bus, uint8_t dePin, uint8_t slaveAddr)
    : bus_(bus), dePin_(dePin), slaveAddr_(slaveAddr), baud_(9600) {}

void RadarSensor::begin(uint32_t baud) {
  baud_ = baud;
  pinMode(dePin_, OUTPUT);
  setTransmit(false);
  bus_.begin(baud_);
}

void RadarSensor::setTransmit(bool tx) {
  digitalWrite(dePin_, tx ? HIGH : LOW);
}

uint16_t RadarSensor::crc16(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

bool RadarSensor::readRegisters(uint8_t func, uint16_t startAddr, uint16_t count,
                                uint16_t* dest) {
  uint8_t req[8];
  req[0] = slaveAddr_;
  req[1] = func;
  req[2] = highByte(startAddr);
  req[3] = lowByte(startAddr);
  req[4] = highByte(count);
  req[5] = lowByte(count);
  const uint16_t crc = crc16(req, 6);
  req[6] = lowByte(crc);
  req[7] = highByte(crc);

  bus_.listen();
  while (bus_.available()) {
    bus_.read();
  }

  setTransmit(true);
  delayMicroseconds(50);
  bus_.write(req, sizeof(req));
  bus_.flush();
  // Hold DE until last stop bit is out, then give bus settle time
  delay(2);
  setTransmit(false);
  delay(2);

  const uint8_t expectBytes = 5 + (count * 2);
  uint8_t resp[64];
  if (expectBytes > sizeof(resp)) {
    return false;
  }

  const uint32_t deadline = millis() + MODBUS_RESPONSE_TIMEOUT_MS;
  uint8_t got = 0;
  while (got < expectBytes && (int32_t)(millis() - deadline) < 0) {
    if (bus_.available()) {
      resp[got++] = bus_.read();
    }
  }

  if (got == 0) {
    Serial.println(F("RS485: no reply"));
    return false;
  }

  if (got < expectBytes) {
    Serial.print(F("RS485: short "));
    Serial.println(got);
    return false;
  }
  if (resp[0] != slaveAddr_ || resp[1] != func || resp[2] != (count * 2)) {
    Serial.println(F("RS485: bad header"));
    return false;
  }

  const uint16_t rxCrc = ((uint16_t)resp[got - 1] << 8) | resp[got - 2];
  if (rxCrc != crc16(resp, got - 2)) {
    Serial.println(F("RS485: CRC fail"));
    return false;
  }

  for (uint16_t i = 0; i < count; i++) {
    dest[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }
  return true;
}

bool RadarSensor::tryReadAt(uint32_t baud, RadarReading& out) {
  baud_ = baud;
  bus_.begin(baud_);
  delay(30);
  bus_.listen();

  // Manual allows FC 03 and 04 — try both
  static const uint8_t kFuncs[] = {0x03, 0x04};
  uint16_t block[5];

  for (uint8_t i = 0; i < 2; i++) {
    if (!readRegisters(kFuncs[i], REG_VOLUME_HI, 5, block)) {
      continue;
    }

    out.volume_m3 = ((uint32_t)block[0] << 16) | block[1];
    out.flow_m3s = block[2] / 1000.0f;
    out.level_mm = block[3];
    out.velocity_ms = block[4] / 100.0f;
    out.empty_height_mm = 0;
    out.flow_direction = 0;
    out.valid = true;
    return true;
  }
  return false;
}

bool RadarSensor::read(RadarReading& out) {
  out.valid = false;

  // Prefer last-known baud, then the other common factory rate
  if (tryReadAt(baud_, out)) {
    return true;
  }
  const uint32_t alt = (baud_ == 9600UL) ? 4800UL : 9600UL;
  if (tryReadAt(alt, out)) {
    Serial.print(F("RS485: locked baud "));
    Serial.println(baud_);
    return true;
  }
  return false;
}
