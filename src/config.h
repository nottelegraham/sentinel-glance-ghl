// Settings model, defaults and persistence (NVS, stored as one JSON blob).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

constexpr int MAX_DEVICES = 6;
constexpr int NUM_TILES = 4;
constexpr int NUM_PUMPS = 6;

// P_SENSOR: ProfiLux sensor N (reg = N) read from its live value at 10000 + 8 (N - 1).
enum Preset : uint8_t { P_PH = 0, P_TEMP_F, P_TEMP_C, P_RSSI, P_CUSTOM, P_PUMP_ML, P_PUMP_L, P_PUMP_PCT, P_SENSOR };
// DEV_PROFILUX covers the ProfiLux Mini, ProfiLux Light Wi-Fi and ProfiLux 4: same Wi-Fi WebSocket protocol.
enum DevType : uint8_t { DEV_PROFILUX = 0, DEV_DOSER = 1 };

inline bool isPumpPreset(uint8_t p) { return p == P_PUMP_ML || p == P_PUMP_L || p == P_PUMP_PCT; }

struct PumpCfg {
  char name[16];       // shown on the Doser page
  bool show;           // listed on the Doser page (and read from the doser)
  uint32_t capML;      // container size, for percent full (0 = show mL instead)
  uint8_t lowPct;      // alarm below this percent (0 = no alarm)
};

struct DeviceCfg {
  char name[16];
  char host[40];       // empty = unused slot
  bool enabled;
  uint8_t type;        // DevType
  PumpCfg pump[NUM_PUMPS];   // dosers only
};

struct TileCfg {
  char label[16];
  uint8_t device;      // index into dev[]
  uint8_t preset;      // Preset
  uint16_t reg;        // P_CUSTOM: register. Pump presets: pump 1..6. P_SENSOR: sensor 1..32
  float divisor;       // raw / divisor = displayed value (custom, sensor)
  uint8_t decimals;    // custom, sensor
  char unit[8];        // custom, sensor; "F"/"C" draw a degree sign
  bool useLimits;      // local low/high limits on top of the controller's own alarms
  float low, high;
  uint32_t alarmMask;  // tile turns red when (sensor alarm mask & this) != 0. Sensor N = bit N-1. 0 = never
  uint32_t color;      // accent, 0xRRGGBB
  uint32_t capacityML; // pump presets: container size, for percent full
};

struct Config {
  char ssid[33];
  char pass[65];
  char tz[48];
  DeviceCfg dev[MAX_DEVICES];
  TileCfg tile[NUM_TILES];
  uint8_t brightness;   // 0..3 → 25/50/75/100 %
  bool glowAlarm;
  bool glowOffline;
  uint8_t glowLevel;    // 0..2
  uint8_t glowPort;     // 0 = Port A white wire (G32), 1 = Port A yellow wire (G33), 2 = M-Bus pin 8 (G25): glow back or M5GO Bottom2
  uint8_t glowCount;    // NeoPixels on that output
  bool powerFromBack;   // true: 5 V comes IN on M-Bus pin 28 (Core2 bus set to input); false: USB-C, Core2 drives 5 V OUT on the bus
  bool beep;
  bool clock24;
  uint8_t rotateSec;    // home screen alternates tiles / doser page every N s (0 = off)
  uint8_t nightMode;    // 0 off, 1 dim, 2 screen off
  uint8_t nightStart, nightEnd;   // local hours, e.g. 22 → 7
  bool otaCheck;        // check GitHub for a newer release once a day and show it in About
};

inline bool devUsed(const DeviceCfg& d) { return d.enabled && d.host[0]; }

// Resolved register/scale for a tile
struct TileSpec { uint16_t reg; float divisor; uint8_t decimals; const char* unit; };

inline uint16_t sensorReg(uint16_t n) { return 10000 + 8 * (constrain(n, 1, 32) - 1); }

inline TileSpec tileSpec(const TileCfg& t) {
  switch (t.preset) {
    case P_PH:     return {10000, 100, 2, ""};       // sensor 1 on the Mini
    case P_TEMP_F: return {10013, 10, 1, "F"};       // sensor 2, Fahrenheit, on the Mini
    case P_TEMP_C: return {10008, 10, 1, "C"};       // sensor 2 on the Mini
    case P_RSSI:   return {10628, 1, 0, "dBm"};
    case P_SENSOR: return {sensorReg(t.reg), t.divisor > 0 ? t.divisor : 1, t.decimals, t.unit};
    case P_PUMP_ML: return {t.reg, 1, 0, "mL"};      // doser reports mL
    case P_PUMP_L:  return {t.reg, 1000, 2, "L"};
    case P_PUMP_PCT: return {t.reg, (t.capacityML ? t.capacityML : 1000) / 100.0f, 0, "%"};   // mL / (cap/100) = % full
    default:       return {t.reg, t.divisor > 0 ? t.divisor : 1, t.decimals, t.unit};
  }
}

// Percent tiles take their container size from their doser's pump settings.
inline void syncPumpCaps(Config& c) {
  for (int i = 0; i < NUM_TILES; i++) {
    TileCfg& t = c.tile[i];
    if (t.preset == P_PUMP_PCT && t.reg >= 1 && t.reg <= NUM_PUMPS && t.device < MAX_DEVICES)
      t.capacityML = c.dev[t.device].pump[t.reg - 1].capML;
  }
}

inline void pumpDefaults(DeviceCfg& d) {
  for (int p = 0; p < NUM_PUMPS; p++) {
    snprintf(d.pump[p].name, sizeof d.pump[p].name, "Pump %d", p + 1);
    d.pump[p].show = p < 4; d.pump[p].capML = 5000; d.pump[p].lowPct = 10;
  }
}

// Generic first-run defaults: one ProfiLux and one Doser, addresses left blank for the setup page.
inline void setDefaults(Config& c) {
  memset(&c, 0, sizeof c);
  strlcpy(c.tz, "EST5EDT,M3.2.0,M11.1.0", sizeof c.tz);
  for (int i = 0; i < MAX_DEVICES; i++) pumpDefaults(c.dev[i]);
  strlcpy(c.dev[0].name, "ProfiLux", 16); c.dev[0].enabled = true; c.dev[0].type = DEV_PROFILUX;
  strlcpy(c.dev[1].name, "Doser", 16);    c.dev[1].enabled = true; c.dev[1].type = DEV_DOSER;
  const char* labels[] = {"Temp", "pH", "Pump 1", "Pump 2"};
  const uint8_t devs[] = {0, 0, 1, 1};
  const uint8_t presets[] = {P_TEMP_F, P_PH, P_PUMP_PCT, P_PUMP_PCT};
  const uint16_t regs[] = {0, 0, 1, 2};
  const uint32_t masks[] = {2, 1, 0, 0};    // Mini: pH = sensor 1 (bit 0), temperature = sensor 2 (bit 1)
  const uint32_t colors[] = {0x6DBE5A, 0x9B7BD6, 0xE8A317, 0x3FB6C8};
  for (int i = 0; i < NUM_TILES; i++) {
    TileCfg& t = c.tile[i];
    strlcpy(t.label, labels[i], sizeof t.label);
    t.device = devs[i]; t.preset = presets[i]; t.reg = regs[i];
    t.divisor = 1; t.decimals = 1; t.useLimits = false; t.low = isPumpPreset(presets[i]) ? 10 : 0; t.high = 0;
    t.alarmMask = masks[i]; t.color = colors[i];
  }
  syncPumpCaps(c);
  c.brightness = 2; c.glowAlarm = true; c.glowOffline = true; c.glowLevel = 1;
  c.glowPort = 2; c.glowCount = 7; c.powerFromBack = true;   // WS2812 Jewel x7 in the glow back on M-Bus pin 8 (G25), powered through the back
  c.beep = true; c.clock24 = false; c.rotateSec = 10;
  c.nightMode = 1; c.nightStart = 22; c.nightEnd = 7;
  c.otaCheck = true;
}

inline void pumpsToJson(const DeviceCfg& dv, JsonArray a) {
  for (int p = 0; p < NUM_PUMPS; p++) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = dv.pump[p].name; o["show"] = dv.pump[p].show; o["cap"] = dv.pump[p].capML; o["low"] = dv.pump[p].lowPct;
  }
}

inline void pumpsFromJson(DeviceCfg& dv, JsonArray a) {
  for (int p = 0; p < NUM_PUMPS && p < (int)a.size(); p++) {
    JsonObject o = a[p]; PumpCfg& pc = dv.pump[p];
    if (o["name"].is<const char*>()) strlcpy(pc.name, o["name"].as<const char*>(), sizeof pc.name);
    if (o["show"].is<bool>()) pc.show = o["show"];
    if (!o["cap"].isNull()) pc.capML = constrain(o["cap"].as<long>(), 0L, 3000000L);
    if (!o["low"].isNull()) pc.lowPct = constrain(o["low"].as<int>(), 0, 100);
  }
}

inline void configToJson(const Config& c, JsonDocument& d, bool includePass) {
  d["ssid"] = c.ssid;
  d["pass"] = includePass ? c.pass : (c.pass[0] ? "********" : "");
  d["tz"] = c.tz;
  JsonArray devs = d["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVICES; i++) {
    JsonObject o = devs.add<JsonObject>();
    o["name"] = c.dev[i].name; o["host"] = c.dev[i].host; o["enabled"] = c.dev[i].enabled; o["type"] = c.dev[i].type;
    pumpsToJson(c.dev[i], o["pumps"].to<JsonArray>());
  }
  JsonArray tiles = d["tiles"].to<JsonArray>();
  for (int i = 0; i < NUM_TILES; i++) {
    const TileCfg& t = c.tile[i];
    JsonObject o = tiles.add<JsonObject>();
    o["label"] = t.label; o["device"] = t.device; o["preset"] = t.preset;
    o["reg"] = t.reg; o["divisor"] = t.divisor; o["decimals"] = t.decimals; o["unit"] = t.unit;
    o["useLimits"] = t.useLimits; o["low"] = t.low; o["high"] = t.high; o["capacity"] = t.capacityML;
    o["alarmMask"] = t.alarmMask;
    char hex[8]; snprintf(hex, sizeof hex, "#%06lX", (unsigned long)t.color); o["color"] = hex;
  }
  d["brightness"] = c.brightness; d["glowAlarm"] = c.glowAlarm; d["glowOffline"] = c.glowOffline;
  d["glowLevel"] = c.glowLevel; d["glowPort"] = c.glowPort; d["glowCount"] = c.glowCount; d["powerFromBack"] = c.powerFromBack;
  d["beep"] = c.beep; d["clock24"] = c.clock24; d["rotateSec"] = c.rotateSec;
  d["nightMode"] = c.nightMode; d["nightStart"] = c.nightStart; d["nightEnd"] = c.nightEnd;
  d["otaCheck"] = c.otaCheck;
}

// Applies whatever keys are present; missing keys keep their current value.
inline void configFromJson(Config& c, JsonDocument& d) {
  auto str = [](JsonVariant v, char* dst, size_t n) { if (v.is<const char*>()) strlcpy(dst, v.as<const char*>(), n); };
  str(d["ssid"], c.ssid, sizeof c.ssid);
  if (d["pass"].is<const char*>() && strcmp(d["pass"].as<const char*>(), "********") != 0) str(d["pass"], c.pass, sizeof c.pass);
  str(d["tz"], c.tz, sizeof c.tz);
  JsonArray devs = d["devices"];
  for (int i = 0; i < MAX_DEVICES && i < (int)devs.size(); i++) {
    JsonObject o = devs[i];
    str(o["name"], c.dev[i].name, 16); str(o["host"], c.dev[i].host, 40);
    if (o["enabled"].is<bool>()) c.dev[i].enabled = o["enabled"];
    if (!o["type"].isNull()) c.dev[i].type = constrain(o["type"].as<int>(), 0, 1);
    if (o["pumps"].is<JsonArray>()) pumpsFromJson(c.dev[i], o["pumps"]);
  }
  if (d["pumps"].is<JsonArray>())   // settings from v0.4 kept one pump list at the top level: give it to the first doser
    for (int i = 0; i < MAX_DEVICES; i++) if (c.dev[i].type == DEV_DOSER) { pumpsFromJson(c.dev[i], d["pumps"]); break; }
  JsonArray tiles = d["tiles"];
  for (int i = 0; i < NUM_TILES && i < (int)tiles.size(); i++) {
    JsonObject o = tiles[i]; TileCfg& t = c.tile[i];
    str(o["label"], t.label, sizeof t.label);
    if (!o["device"].isNull()) t.device = constrain(o["device"].as<int>(), 0, MAX_DEVICES - 1);
    if (!o["preset"].isNull()) t.preset = constrain(o["preset"].as<int>(), 0, (int)P_SENSOR);
    if (!o["reg"].isNull()) t.reg = o["reg"];
    if (!o["divisor"].isNull()) t.divisor = o["divisor"].as<float>() > 0 ? o["divisor"].as<float>() : 1;
    if (!o["decimals"].isNull()) t.decimals = constrain(o["decimals"].as<int>(), 0, 3);
    str(o["unit"], t.unit, sizeof t.unit);
    if (o["useLimits"].is<bool>()) t.useLimits = o["useLimits"];
    if (!o["low"].isNull()) t.low = o["low"];
    if (!o["high"].isNull()) t.high = o["high"];
    if (!o["capacity"].isNull()) t.capacityML = constrain(o["capacity"].as<long>(), 1L, 3000000L);
    if (!o["alarmMask"].isNull()) t.alarmMask = o["alarmMask"].as<uint32_t>();
    else if (!o["alarmCode"].isNull()) t.alarmMask = o["alarmCode"].as<uint32_t>();   // v0.4: 1 = pH, 2 = temperature, same bits
    if (o["color"].is<const char*>()) {
      const char* h = o["color"]; if (*h == '#') h++;
      t.color = strtoul(h, nullptr, 16) & 0xFFFFFF;
    }
  }
  if (!d["brightness"].isNull()) c.brightness = constrain(d["brightness"].as<int>(), 0, 3);
  if (d["glowAlarm"].is<bool>()) c.glowAlarm = d["glowAlarm"];
  if (d["glowOffline"].is<bool>()) c.glowOffline = d["glowOffline"];
  if (!d["glowLevel"].isNull()) c.glowLevel = constrain(d["glowLevel"].as<int>(), 0, 2);
  if (!d["glowPort"].isNull()) c.glowPort = constrain(d["glowPort"].as<int>(), 0, 2);
  if (!d["glowCount"].isNull()) c.glowCount = constrain(d["glowCount"].as<int>(), 1, 30);
  if (d["powerFromBack"].is<bool>()) c.powerFromBack = d["powerFromBack"];
  if (d["beep"].is<bool>()) c.beep = d["beep"];
  if (d["clock24"].is<bool>()) c.clock24 = d["clock24"];
  if (!d["rotateSec"].isNull()) c.rotateSec = constrain(d["rotateSec"].as<int>(), 0, 120);
  if (!d["nightMode"].isNull()) c.nightMode = constrain(d["nightMode"].as<int>(), 0, 2);
  if (!d["nightStart"].isNull()) c.nightStart = constrain(d["nightStart"].as<int>(), 0, 23);
  if (!d["nightEnd"].isNull()) c.nightEnd = constrain(d["nightEnd"].as<int>(), 0, 23);
  if (d["otaCheck"].is<bool>()) c.otaCheck = d["otaCheck"];
  for (int i = 0; i < NUM_TILES; i++) {   // doser tiles are percent only, and must point at a doser
    TileCfg& t = c.tile[i];
    if (t.preset == P_PUMP_ML || t.preset == P_PUMP_L) t.preset = P_PUMP_PCT;
  }
  syncPumpCaps(c);
}

// Stored as a blob: six controllers with their pumps outgrow NVS's 4,000-byte string limit.
inline void loadConfig(Config& c) {
  setDefaults(c);
  Preferences p;
  if (!p.begin("glance", true)) return;
  String s;
  size_t n = p.getBytesLength("cfg3");
  if (n) { s.reserve(n); char* b = (char*)malloc(n + 1); if (b) { p.getBytes("cfg3", b, n); b[n] = 0; s = b; free(b); } }
  else s = p.getString("cfg2", "");   // v0.4 settings
  p.end();
  if (s.isEmpty()) return;
  JsonDocument d;
  if (deserializeJson(d, s) == DeserializationError::Ok) configFromJson(c, d);
}

inline void saveConfig(const Config& c) {
  JsonDocument d;
  configToJson(c, d, true);
  String s; serializeJson(d, s);
  Preferences p;
  if (!p.begin("glance", false)) return;
  p.putBytes("cfg3", s.c_str(), s.length());
  p.remove("cfg2");
  p.end();
}

inline int glowGpio(uint8_t port) { return port == 1 ? 33 : port == 2 ? 25 : 32; }
