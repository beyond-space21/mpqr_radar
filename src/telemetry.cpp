#include "telemetry.h"
#include <stdio.h>
#include <string.h>

int formatTelemetryJson(char* dest, size_t destLen, const TelemetryPayload& t) {
  char flowStr[12];
  char velStr[12];
  dtostrf(t.flow_x1000 / 1000.0, 0, 3, flowStr);
  dtostrf(t.vel_cms / 100.0, 0, 2, velStr);

  const int n = snprintf(
      dest, destLen,
      "{\"rok\":%u,\"vol\":%lu,\"flow\":%s,\"lvl\":%u,\"vel\":%s,"
      "\"empty\":%u,\"range\":%u,\"flow2\":%u,\"dir\":%u,\"vol_t\":%lu,"
      "\"sect\":%u,\"s1\":%u,\"s2\":%u,\"s3\":%u,\"jl\":%u,\"jv\":%u,"
      "\"mst\":\"%s\",\"csq\":%u,\"ber\":%u}",
      (unsigned)t.rok, (unsigned long)t.vol, flowStr, t.lvl, velStr, t.empty,
      t.range, t.flow2, (unsigned)t.dir, (unsigned long)t.vol_t, t.sect, t.s1,
      t.s2, t.s3, t.jl, t.jv, t.mst, (unsigned)t.csq, (unsigned)t.ber);

  if (n <= 0 || (size_t)n >= destLen) {
    return -1;
  }
  return n;
}
