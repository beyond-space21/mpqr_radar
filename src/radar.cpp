#include "radar.h"
#include "config.h"

// Register map (hex) — RS-RAD-N01-3 manual §4.3
static const uint16_t REG_VOLUME_HI = 0x03E8;
static const uint16_t REG_FLOW_X1000 = 0x03EA;
static const uint16_t REG_LEVEL_MM = 0x03EB;
static const uint16_t REG_VELOCITY_CMS = 0x03EC;
static const uint16_t REG_EMPTY_HEIGHT = 0x0401;
static const uint16_t REG_FLOW_DIR = 0x0439;

RadarSensor::RadarSensor(SoftwareSerial& bus, uint8_t dePin, uint8_t slaveAddr)
    : bus_(bus), dePin_(dePin), slaveAddr_(slaveAddr) {}

void RadarSensor::begin(uint32_t baud) {
  pinMode(dePin_, OUTPUT);
  setTransmit(false);
  bus_.begin(baud);
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

bool RadarSensor::readRegisters(uint16_t startAddr, uint16_t count, uint16_t* dest) {
  uint8_t req[8];
  req[0] = slaveAddr_;
  req[1] = 0x03;
  req[2] = highByte(startAddr);
  req[3] = lowByte(startAddr);
  req[4] = highByte(count);
  req[5] = lowByte(count);
  const uint16_t crc = crc16(req, 6);
  req[6] = lowByte(crc);
  req[7] = highByte(crc);

  while (bus_.available()) {
    bus_.read();
  }

  setTransmit(true);
  delayMicroseconds(100);
  bus_.write(req, sizeof(req));
  bus_.flush();
  delay(MODBUS_INTER_FRAME_MS);
  setTransmit(false);

  // Response: addr, func, byteCount, data[2*count], crcLo, crcHi
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

  if (got < expectBytes) {
    return false;
  }
  if (resp[0] != slaveAddr_ || resp[1] != 0x03 || resp[2] != (count * 2)) {
    return false;
  }

  const uint16_t rxCrc = (uint16_t)resp[got - 1] << 8 | resp[got - 2];
  if (rxCrc != crc16(resp, got - 2)) {
    return false;
  }

  for (uint16_t i = 0; i < count; i++) {
    dest[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }
  return true;
}

bool RadarSensor::read(RadarReading& out) {
  out.valid = false;

  // Contiguous block: volume HI/LO, flow×1000, level, velocity (5 regs @ 0x03E8)
  uint16_t block[5];
  if (!readRegisters(REG_VOLUME_HI, 5, block)) {
    return false;
  }

  uint16_t empty = 0;
  uint16_t dir = 0;
  const bool gotEmpty = readRegisters(REG_EMPTY_HEIGHT, 1, &empty);
  const bool gotDir = readRegisters(REG_FLOW_DIR, 1, &dir);

  out.volume_m3 = ((uint32_t)block[0] << 16) | block[1];
  out.flow_m3s = block[2] / 1000.0f;          // enlarged ×1000
  out.level_mm = block[3];
  out.velocity_ms = block[4] / 100.0f;        // cm/s → m/s
  out.empty_height_mm = gotEmpty ? empty : 0;
  out.flow_direction = gotDir ? (uint8_t)(dir & 0x01) : 0;
  out.valid = true;
  return true;
}
