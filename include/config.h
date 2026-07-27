#pragma once

// ---------------------------------------------------------------------------
// Hardware wiring (Arduino Nano)
//
 //  SIM7600G-H (SoftwareSerial; factory UART often 115200, locked to 9600 after AT)
 //    Nano D2 (RX)  <- SIM7600 TX
 //    Nano D3 (TX)  -> SIM7600 RX
 //    Nano D4       -> SIM7600 PWRKEY (only if MODEM_HAS_PWRKEY; pulse turns ON or OFF)
 //    Power the module from a dedicated supply (USB alone is usually not enough)
 //    GND           -- common
 //
 //  MAX485 / RS485 transceiver (SoftwareSerial @ radar baud)
 //    Nano D8 (RX)  <- RO
 //    Nano D9 (TX)  -> DI
 //    Nano D7       -> DE + RE (tied together; HIGH = TX)
 //    A             -> radar Yellow/Green (485-A)
 //    B             -> radar Blue (485-B)
 //
 //  RS-RAD-N01-3
 //    Brown  V+  (10–30 V DC)
 //    Black  V-
 //    Yellow/Green 485-A
 //    Blue          485-B
 // ---------------------------------------------------------------------------

// --- SIM7600 pins ---
static const uint8_t PIN_MODEM_RX = 2;   // Nano listens
static const uint8_t PIN_MODEM_TX = 3;   // Nano transmits
static const uint8_t PIN_MODEM_PWRKEY = 4;
// false = assume modem already powered (safer while debugging).
// true  = pulse PWRKEY on bring-up (can turn a running modem OFF).
static const bool MODEM_HAS_PWRKEY = false;
static const uint32_t MODEM_BAUD = 9600;  // SoftSerial-safe; modem UART locked here

// --- RS485 / radar pins ---
static const uint8_t PIN_RS485_RX = 8;
static const uint8_t PIN_RS485_TX = 9;
static const uint8_t PIN_RS485_DE = 7;
static const uint32_t RADAR_BAUD = 4800;  // this unit answers at 4800 (manual §3)
static const uint8_t RADAR_SLAVE_ADDR = 0x01;

// --- Cellular ---
static const char APN[] PROGMEM = "airtelgprs.com";   // set to your SIM APN
static const char GPRS_USER[] PROGMEM = "";
static const char GPRS_PASS[] PROGMEM = "";

// --- MQTT ---
// Must be reachable from Airtel cellular (public IP / VPS). LAN IPs will fail on GPRS.
static const char MQTT_BROKER[] PROGMEM = "217.217.249.208";
static const uint16_t MQTT_PORT = 1883;
static const char MQTT_USER[] PROGMEM = "";
static const char MQTT_PASS[] PROGMEM = "";
static const char MQTT_CLIENT_ID[] PROGMEM = "mpqr-radar-01";

// Topics (device publishes telemetry; optional command topic for future use)
static const char TOPIC_TELEMETRY[] PROGMEM = "mpqr/radar/01/telemetry";
static const char TOPIC_STATUS[] PROGMEM = "mpqr/radar/01/status";

// --- Timing ---
static const uint32_t PUBLISH_INTERVAL_MS = 30000UL;
static const uint32_t MODEM_RECONNECT_MS = 15000UL;
static const uint32_t NETWORK_WAIT_MS = 120000UL;
static const uint16_t MODBUS_RESPONSE_TIMEOUT_MS = 1500;
static const uint16_t MODBUS_INTER_FRAME_MS = 5;
