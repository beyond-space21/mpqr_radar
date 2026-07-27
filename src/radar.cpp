#include "radar.h"
#include "config.h"

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

  bus_.listen();
  while (bus_.available()) {
    bus_.read();
  }

  setTransmit(true);
  delayMicroseconds(50);
  bus_.write(req, sizeof(req));
  bus_.flush();
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

  if (got < expectBytes) {
    return false;
  }
  if (resp[0] != slaveAddr_ || resp[1] != 0x03 || resp[2] != (count * 2)) {
    return false;
  }

  const uint16_t rxCrc = ((uint16_t)resp[got - 1] << 8) | resp[got - 2];
  if (rxCrc != crc16(resp, got - 2)) {
    return false;
  }

  for (uint16_t i = 0; i < count; i++) {
    dest[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
  }
  return true;
}

bool RadarSensor::readInto(TelemetryPayload& out) {
  out.rok = 0;
  out.vol = 0;
  out.flow_x1000 = 0;
  out.lvl = 0;
  out.vel_cms = 0;
  out.empty = 0;
  out.range = 0;
  out.flow2 = 0;
  out.dir = 0;
  out.vol_t = 0;
  out.sect = 0;
  out.s1 = 0;
  out.s2 = 0;
  out.s3 = 0;
  out.jl = 0;
  out.jv = 0;

  // Primary contiguous block: vol HI/LO, flow×1000, level, velocity @ 0x03E8
  uint16_t primary[5];
  if (!readRegisters(0x03E8, 5, primary)) {
    return false;
  }

  out.vol = ((uint32_t)primary[0] << 16) | primary[1];
  out.flow_x1000 = primary[2];
  out.lvl = primary[3];
  out.vel_cms = primary[4];
  out.rok = 1;

  // Optional extras — ignore individual failures
  uint16_t v = 0;
  if (readRegisters(0x0401, 1, &v)) {
    out.empty = v;
  }
  if (readRegisters(0x0422, 1, &v)) {
    out.range = v;
  }
  if (readRegisters(0x0437, 1, &v)) {
    out.flow2 = v;
  }
  if (readRegisters(0x0439, 1, &v)) {
    out.dir = (uint8_t)(v & 0x01);
  }

  uint16_t times[2];
  if (readRegisters(0x0430, 2, times)) {
    out.vol_t = ((uint32_t)times[0] << 16) | times[1];
  }

  uint16_t sect[4];
  if (readRegisters(0x0412, 4, sect)) {
    out.sect = sect[0];
    out.s1 = sect[1];
    out.s2 = sect[2];
    out.s3 = sect[3];
  }

  uint16_t jumps[2];
  if (readRegisters(0x0417, 2, jumps)) {
    out.jl = jumps[0];
    out.jv = jumps[1];
  }

  return true;
}
