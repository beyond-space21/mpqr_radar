#pragma once

#include <Arduino.h>

// Single telemetry template used for every MQTT publish.
// Radar fields from RS-RAD-N01-3 manual §4.3; modem fields from AT+CSQ.
struct TelemetryPayload {
  // Radar (rok=0 → radar values may be stale/zero)
  uint8_t rok;           // 1 = Modbus read OK
  uint32_t vol;          // m³ cumulative (03E8/03E9)
  uint16_t flow_x1000;   // m³/s ×1000 (03EA)
  uint16_t lvl;          // mm water level (03EB)
  uint16_t vel_cms;      // cm/s surface velocity (03EC)
  uint16_t empty;        // mm empty height (0401)
  uint16_t range;        // mm water level range (0422)
  uint16_t flow2;        // instantaneous flow alt (0437), raw
  uint8_t dir;           // 0 with stream / 1 against (0439)
  uint32_t vol_t;        // volume record start time (0430/0431)
  uint16_t sect;         // section type 1 trap / 2 rect (0412)
  uint16_t s1;           // section dim 1 mm (0413)
  uint16_t s2;           // section dim 2 mm (0414)
  uint16_t s3;           // section dim 3 mm (0415)
  uint16_t jl;           // level jump threshold mm (0417)
  uint16_t jv;           // velocity jump threshold (0418)

  // Modem
  char mst[12];          // "ok" | "radar_err" | "boot" | ...
  uint8_t csq;           // 0–31, 99 unknown
  uint8_t ber;           // bit error rate from +CSQ
};

// Format into dest. Returns length, or -1 on overflow.
int formatTelemetryJson(char* dest, size_t destLen, const TelemetryPayload& t);
