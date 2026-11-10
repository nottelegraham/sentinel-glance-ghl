// Sentinel Glance – GHL: four-tile at-a-glance display for GHL ProfiLux controllers (Mini, Light Wi-Fi, ProfiLux 4)
// and GHL Master Doser 2.1, up to six devices.
// Hardware: M5Stack Core2, plus a NeoPixel alarm light inside the back (G25) or on Port A (G32 / G33).
// Read-only: the Glance only sends register READ requests. It never writes to a controller.
//
//   Home      : clock, hamburger menu, alarm bell, four tiles. Tap a tile for its graph.
//   Detail    : 1 h / 6 h / 24 h history, min / max / average, limit lines.
//   Menu      : Alarms, Settings, Network & setup, About.
//   Setup page: http://<glance-ip>/  (or 192.168.8.1 on the "GHL-Glance-xxxx" hotspot)
//   Updates   : upload a firmware .bin on the setup page, or install the latest GitHub release from it.

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <Adafruit_NeoPixel.h>
#include <esp_sntp.h>
#include <LittleFS.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "ghl_codec.h"
#include "config.h"
#include "webpage.h"
#include "ota_certs.h"

static const char* FW_VERSION = "0.5.0";
static const char* GITHUB_REPO = "nottelegraham/sentinel-glance-ghl";   // releases carry firmware.bin
static const char* FW_ASSET = "firmware.bin";

// ---------- timing ----------
// One controller is read per 30 s slot, in turn across the enabled controllers.
// Two controllers → each is read once a minute; six → every 3 minutes.
constexpr uint32_t SLOT_MS = 30000;
constexpr uint32_t SLOT_SEND_WINDOW_MS = 15000;   // a slot waits this long for its socket, then gives up
constexpr uint32_t REPLY_TIMEOUT_MS = 15000;
// Offline after 3 missed turns, at least 3 minutes. Set from the number of enabled controllers.
volatile uint32_t staleMs = 180000, reidentifyMs = 300000;
constexpr uint32_t BACKOFF_MS[] = {2000, 5000, 10000, 15000, 30000};

// ---------- glow: NeoPixel(s) on Port A (G32 or G33) or the M5GO Bottom2 (G25) ----------
Adafruit_NeoPixel glow(1, 32, NEO_GRB + NEO_KHZ800);
int glowPinNow = -1, glowCountNow = 0;

// ---------- shared state (guarded by mtx) ----------
SemaphoreHandle_t mtx;
struct Lock { Lock() { xSemaphoreTake(mtx, portMAX_DELAY); } ~Lock() { xSemaphoreGive(mtx); } };

Config cfg;
volatile bool reconfigure = false, rebootPending = false;

struct DevState {
  bool haveToken = false;
  uint8_t token[8];
  int product = -1, firmware = -1;
  bool wsUp = false, waiting = false, everReplied = false;
  uint32_t lastReply = 0, lastRequest = 0, nextAttempt = 0, identityAt = 0;
  int backoff = 0;
  int alarm = 0, leak = 0, rssi = 0;
  char status[40] = "starting";
};
DevState dev[MAX_DEVICES];
WebSocketsClient ws[MAX_DEVICES];

struct TileState { bool valid = false; float value = NAN; uint32_t at = 0; };
TileState tileState[NUM_TILES];
struct PumpState { bool valid = false; int32_t ml = 0; uint32_t at = 0; };
PumpState pumpState[MAX_DEVICES][NUM_PUMPS];

static void recalcStale() {   // caller holds the lock
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) n += devUsed(cfg.dev[i]);
  staleMs = std::max<uint32_t>(180000, (uint32_t)std::max(n, 1) * SLOT_MS * 3);
  reidentifyMs = staleMs + 120000;
}

// OTA state for the setup page and About screen
volatile bool otaCheckReq = false, otaInstallReq = false;
char otaLatest[24] = "", otaStatus[64] = "", otaUrl[200] = "";
volatile int otaProgress = -1;   // percent while installing

// network status for the UI
volatile bool apActive = false, apAuto = false, staUp = false;
char apName[24];
char ipText[20] = "";
volatile int staRssi = 0;

WebServer server(80);
DNSServer dns;
const IPAddress AP_IP(192, 168, 8, 1);

// ---------- helpers ----------
static uint16_t C(uint32_t rgb) {   // 0xRRGGBB → RGB565
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {   // UTC date → days since 1970-01-01
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static bool timeValid() { return time(nullptr) > 1704067200; }   // after 2024-01-01

// "v0.5.1" > "0.5.0"? Compares up to three numeric parts; a leading "v" is ignored.
static bool versionNewer(const char* a, const char* b) {
  int x[3] = {0}, y[3] = {0};
  if (*a == 'v' || *a == 'V') a++;
  if (*b == 'v' || *b == 'V') b++;
  sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int k = 0; k < 3; k++) if (x[k] != y[k]) return x[k] > y[k];
  return false;
}

static void rtcToSystem() {
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2024) return;
  time_t t = (time_t)(daysFromCivil(dt.date.year, dt.date.month, dt.date.date) * 86400LL +
                      dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds);
  timeval tv{t, 0};
  settimeofday(&tv, nullptr);
}

static void systemToRtc() {
  time_t now = time(nullptr);
  tm u; gmtime_r(&now, &u);
  m5::rtc_datetime_t dt;
  dt.date.year = u.tm_year + 1900; dt.date.month = u.tm_mon + 1; dt.date.date = u.tm_mday; dt.date.weekDay = u.tm_wday;
  dt.time.hours = u.tm_hour; dt.time.minutes = u.tm_min; dt.time.seconds = u.tm_sec;
  M5.Rtc.setDateTime(dt);
}

// ==========================================================================================

// ==========================================================================================
// History: 7 days at 1-minute resolution + 90 days of 15-minute averages, per tile.
// Rings live in PSRAM and are indexed by absolute time (epoch minute / quarter hour), so a
// power cut shows up as a gap. Records are appended to LittleFS logs (one small write every
// 10 minutes for the minute data, one per quarter hour for the averages) and replayed at boot.
// Nothing is recorded until the clock is set from the network.
// ==========================================================================================
constexpr int FINE_LEN = 7 * 1440;      // minutes
constexpr int COARSE_LEN = 90 * 96;     // quarter hours
constexpr int NRANGE = 5;
extern const int RANGE_MIN[NRANGE] = {60, 360, 1440, 10080, 129600};   // 1 h, 6 h, 24 h, 7 d, 90 d
const char* RANGE_TXT[NRANGE] = {"1h", "6h", "24h", "7d", "90d"};
const char* FINE_LOG = "/hist_1m.log";
const char* COARSE_LOG = "/hist_15m.log";
constexpr uint32_t LOG_MAGIC = 0x31484C47;   // "GLH1"

float* fineH[NUM_TILES];
float* coarseH[NUM_TILES];
int32_t fineLast = -1, coarseLast = -1;     // newest slot written (-1 = empty)
float qSum[NUM_TILES]; uint16_t qCnt[NUM_TILES]; int32_t qCur = -1;
float* gbuf = nullptr;                      // shared scratch for drawing (FINE_LEN floats)
volatile bool histDeleteReq = false;
bool fsOk = false;

struct HRec { int32_t when; float v[NUM_TILES]; };
HRec finePending[16]; int finePendingN = 0;

static int32_t nowMinute() { return (int32_t)(time(nullptr) / 60); }

// caller holds the lock
static void ringPut(float** ring, int len, int32_t& last, int32_t when, const float* v) {
  if (last >= 0 && when <= last) {                       // replayed or out of order
    if (when > last - len) for (int t = 0; t < NUM_TILES; t++) ring[t][when % len] = v[t];
    return;
  }
  if (last >= 0) {
    int32_t gap = when - last - 1;
    if (gap >= len) { for (int t = 0; t < NUM_TILES; t++) for (int k = 0; k < len; k++) ring[t][k] = NAN; }
    else for (int32_t m = last + 1; m < when; m++) for (int t = 0; t < NUM_TILES; t++) ring[t][m % len] = NAN;
  }
  for (int t = 0; t < NUM_TILES; t++) ring[t][when % len] = v[t];
  last = when;
}

static float ringGet(float** ring, int len, int32_t last, int t, int32_t when) {
  if (last < 0 || when > last || when <= last - len || when < 0) return NAN;
  return ring[t][when % len];
}

static void clearTileHistory(int t) {   // caller holds the lock
  for (int k = 0; k < FINE_LEN; k++) fineH[t][k] = NAN;
  for (int k = 0; k < COARSE_LEN; k++) coarseH[t][k] = NAN;
  qSum[t] = 0; qCnt[t] = 0;
}

// Fills buf with the whole window for range r (oldest → newest, NaN = no data). Returns its length.
static int gatherRange(int t, int r, float* buf) {
  Lock l;
  if (r < 4) {
    int n = RANGE_MIN[r];
    int32_t end = timeValid() ? nowMinute() : fineLast;
    for (int k = 0; k < n; k++) buf[k] = ringGet(fineH, FINE_LEN, fineLast, t, end - n + 1 + k);
    return n;
  }
  int n = COARSE_LEN;
  int32_t end = timeValid() ? nowMinute() / 15 : coarseLast;
  for (int k = 0; k < n; k++) buf[k] = ringGet(coarseH, COARSE_LEN, coarseLast, t, end - n + 1 + k);
  return n;
}

// ---------- flash logs ----------
static void appendRecs(const char* path, const HRec* r, int n) {
  if (!fsOk || n <= 0) return;
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() == 0) { uint32_t m = LOG_MAGIC; uint8_t nt = NUM_TILES; f.write((uint8_t*)&m, 4); f.write(&nt, 1); }
  f.write((const uint8_t*)r, sizeof(HRec) * n);
  f.close();
}

static void loadLog(const char* path, float** ring, int len, int32_t& last) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  uint32_t m = 0; uint8_t nt = 0;
  if (f.read((uint8_t*)&m, 4) != 4 || f.read(&nt, 1) != 1 || m != LOG_MAGIC || nt != NUM_TILES) { f.close(); LittleFS.remove(path); return; }
  HRec r;
  while (f.read((uint8_t*)&r, sizeof r) == sizeof r) { Lock l; ringPut(ring, len, last, r.when, r.v); }
  f.close();
}

// Rewrites a log from its ring when it has grown past keepSlots * 1.15, so it never fills the flash.
static void compactLog(const char* path, float** ring, int len, int32_t last) {
  if (!fsOk || last < 0) return;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  size_t sz = f.size(); f.close();
  if (sz < (size_t)(len * 1.15) * sizeof(HRec)) return;
  String tmp = String(path) + ".tmp";
  File o = LittleFS.open(tmp, FILE_WRITE);
  if (!o) return;
  uint32_t m = LOG_MAGIC; uint8_t nt = NUM_TILES; o.write((uint8_t*)&m, 4); o.write(&nt, 1);
  for (int32_t w = last - len + 1; w <= last; w++) {
    HRec r; r.when = w; bool any = false;
    { Lock l; for (int t = 0; t < NUM_TILES; t++) { r.v[t] = ringGet(ring, len, last, t, w); any |= !isnan(r.v[t]); } }
    if (any) o.write((uint8_t*)&r, sizeof r);
  }
  o.close();
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
}

static void flushFine() { appendRecs(FINE_LOG, finePending, finePendingN); finePendingN = 0; }

static void historyBegin() {
  fsOk = LittleFS.begin(true);   // formats the empty storage partition on first use
  if (!fsOk) return;
  loadLog(FINE_LOG, fineH, FINE_LEN, fineLast);
  loadLog(COARSE_LOG, coarseH, COARSE_LEN, coarseLast);
  compactLog(FINE_LOG, fineH, FINE_LEN, fineLast);
  compactLog(COARSE_LOG, coarseH, COARSE_LEN, coarseLast);
}

// Called from the network task every loop: one point per minute once the clock is set.
static void historyTick() {
  static int32_t lastRec = -1;
  static uint32_t lastCompact = 0;
  if (histDeleteReq) {
    histDeleteReq = false;
    { Lock l; for (int t = 0; t < NUM_TILES; t++) clearTileHistory(t); fineLast = coarseLast = -1; qCur = -1; finePendingN = 0; }
    if (fsOk) { LittleFS.remove(FINE_LOG); LittleFS.remove(COARSE_LOG); }
    lastRec = -1;
  }
  if (!timeValid()) return;
  int32_t m = nowMinute();
  if (m == lastRec) return;
  lastRec = m;
  HRec rec; rec.when = m;
  HRec q; bool qDone = false;
  {
    Lock l;
    for (int t = 0; t < NUM_TILES; t++) {
      bool fresh = tileState[t].valid && millis() - tileState[t].at < staleMs;
      rec.v[t] = fresh ? tileState[t].value : NAN;
    }
    ringPut(fineH, FINE_LEN, fineLast, m, rec.v);
    int32_t qq = m / 15;
    if (qCur >= 0 && qq != qCur) {                     // close the finished quarter hour
      q.when = qCur;
      for (int t = 0; t < NUM_TILES; t++) { q.v[t] = qCnt[t] ? qSum[t] / qCnt[t] : NAN; qSum[t] = 0; qCnt[t] = 0; }
      ringPut(coarseH, COARSE_LEN, coarseLast, qCur, q.v);
      qDone = true;
    }
    qCur = qq;
    for (int t = 0; t < NUM_TILES; t++) if (!isnan(rec.v[t])) { qSum[t] += rec.v[t]; qCnt[t]++; }
  }
  finePending[finePendingN++] = rec;
  if (finePendingN >= 10) flushFine();
  if (qDone) appendRecs(COARSE_LOG, &q, 1);
  if (millis() - lastCompact > 3600000UL) {            // hourly size check
    lastCompact = millis();
    compactLog(FINE_LOG, fineH, FINE_LEN, fineLast);
    compactLog(COARSE_LOG, coarseH, COARSE_LEN, coarseLast);
  }
}

// Network task: Wi-Fi, setup web server, controller polling
// ==========================================================================================

static void startAP(bool automatic) {
  if (apActive) return;
  WiFi.mode(cfg.ssid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName);
  dns.start(53, "*", AP_IP);
  apActive = true; apAuto = automatic;
}

static void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false; apAuto = false;
}

volatile bool apRequest = false;   // set by the UI's "Start setup hotspot" button

// ---------- OTA: GitHub release check and install, verified TLS ----------
static void otaSetStatus(const char* fmt, ...) {
  Lock l; va_list ap; va_start(ap, fmt); vsnprintf(otaStatus, sizeof otaStatus, fmt, ap); va_end(ap);
}

// Reads the latest release's tag and its firmware.bin download URL.
static void otaCheckGithub() {
  otaSetStatus("Checking GitHub...");
  WiFiClientSecure tls; tls.setCACert(OTA_CA_PEM);
  HTTPClient http;
  String url = String("https://api.github.com/repos/") + GITHUB_REPO + "/releases/latest";
  if (!http.begin(tls, url)) { otaSetStatus("Check failed: bad URL"); return; }
  http.setUserAgent(String("sentinel-glance/") + FW_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); otaSetStatus(code == 404 ? "No release published yet" : "Check failed (HTTP %d)", code); return; }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) { otaSetStatus("Check failed: bad reply"); return; }
  const char* tag = d["tag_name"] | "";
  const char* bin = "";
  for (JsonObject a : d["assets"].as<JsonArray>()) if (strcmp(a["name"] | "", FW_ASSET) == 0) bin = a["browser_download_url"] | "";
  Lock l;
  strlcpy(otaLatest, tag, sizeof otaLatest);
  strlcpy(otaUrl, bin, sizeof otaUrl);
  if (!bin[0]) snprintf(otaStatus, sizeof otaStatus, "Release %s has no %s", tag, FW_ASSET);
  else if (versionNewer(tag, FW_VERSION)) snprintf(otaStatus, sizeof otaStatus, "Update %s available", tag);
  else snprintf(otaStatus, sizeof otaStatus, "Up to date (latest %s)", tag);
}

static void flushFine();
static void otaInstallGithub() {
  char url[200]; { Lock l; strlcpy(url, otaUrl, sizeof url); }
  if (!url[0]) { otaSetStatus("Check for an update first"); return; }
  otaSetStatus("Downloading...");
  WiFiClientSecure tls; tls.setCACert(OTA_CA_PEM);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // github.com → release-assets host
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) { if (total > 0) otaProgress = (int)(cur * 100LL / total); });
  otaProgress = 0;
  t_httpUpdate_return r = httpUpdate.update(tls, url);
  otaProgress = -1;
  if (r == HTTP_UPDATE_OK) { otaSetStatus("Installed. Restarting..."); flushFine(); delay(1500); ESP.restart(); }
  otaSetStatus("Update failed: %s", httpUpdate.getLastErrorString().c_str());
}

static void sendJson(JsonDocument& d, int code = 200) {
  String s; serializeJson(d, s);
  server.send(code, "application/json", s);
}

static void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", SETUP_PAGE); });
  server.on("/api/config", HTTP_GET, [] {
    JsonDocument d;
    { Lock l; configToJson(cfg, d, false); }
    sendJson(d);
  });
  server.on("/api/config", HTTP_POST, [] {
    JsonDocument in;
    if (deserializeJson(in, server.arg("plain")) != DeserializationError::Ok) { server.send(400, "text/plain", "bad json"); return; }
    bool wifiChanged;
    {
      Lock l;
      char oldSsid[33], oldPass[65];
      bool oldPower = cfg.powerFromBack;
      TileCfg oldTiles[NUM_TILES]; memcpy(oldTiles, cfg.tile, sizeof oldTiles);
      strlcpy(oldSsid, cfg.ssid, sizeof oldSsid); strlcpy(oldPass, cfg.pass, sizeof oldPass);
      configFromJson(cfg, in);
      wifiChanged = strcmp(oldSsid, cfg.ssid) || strcmp(oldPass, cfg.pass) || oldPower != cfg.powerFromBack;
      if (cfg.powerFromBack && !oldPower) M5.Power.setExtOutput(false);   // stop driving pin 28 right away
      for (int t = 0; t < NUM_TILES; t++)   // a tile pointed at a different reading starts a fresh history
        if (oldTiles[t].device != cfg.tile[t].device || oldTiles[t].preset != cfg.tile[t].preset || oldTiles[t].reg != cfg.tile[t].reg)
          clearTileHistory(t);
      saveConfig(cfg);
    }
    reconfigure = true;
    JsonDocument out; out["ok"] = true; out["reboot"] = wifiChanged;
    sendJson(out);
    if (wifiChanged) rebootPending = true;
  });
  server.on("/api/ota", HTTP_GET, [] {
    JsonDocument d;
    { Lock l;
      d["version"] = FW_VERSION; d["latest"] = otaLatest; d["status"] = otaStatus; d["progress"] = otaProgress;
      d["available"] = otaLatest[0] && otaUrl[0] && versionNewer(otaLatest, FW_VERSION); d["repo"] = GITHUB_REPO; }
    sendJson(d);
  });
  server.on("/api/ota/check", HTTP_POST, [] { otaCheckReq = true; server.send(202, "application/json", "{\"ok\":true}"); });
  server.on("/api/ota/install", HTTP_POST, [] { otaInstallReq = true; server.send(202, "application/json", "{\"ok\":true}"); });
  // Firmware upload from the setup page: multipart POST of a .bin built for this board.
  server.on("/api/ota/upload", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    if (ok) { otaSetStatus("Installed. Restarting..."); rebootPending = true; }
  }, [] {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      otaSetStatus("Receiving %s", u.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaSetStatus("Update failed: %s", Update.errorString());
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (Update.isRunning() && Update.write(u.buf, u.currentSize) != u.currentSize) otaSetStatus("Update failed: %s", Update.errorString());
    } else if (u.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) otaSetStatus("Update failed: %s", Update.errorString());   // checks the image before switching
      otaProgress = -1;
    } else if (u.status == UPLOAD_FILE_ABORTED) { Update.abort(); otaProgress = -1; otaSetStatus("Upload aborted"); }
  });
  server.on("/api/scan", HTTP_GET, [] {
    int n = WiFi.scanNetworks();
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;
      bool dup = false;
      for (JsonVariant v : a) if (s == v.as<const char*>()) { dup = true; break; }
      if (!dup) a.add(s);
    }
    WiFi.scanDelete();
    sendJson(d);
  });
  server.on("/api/history.csv", HTTP_GET, [] {   // ?r=7d (1-minute rows) or ?r=90d (15-minute rows)
    bool coarse = server.arg("r") == "90d";
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Content-Disposition", coarse ? "attachment; filename=glance_90d.csv" : "attachment; filename=glance_7d.csv");
    server.send(200, "text/csv", "");
    String out = "time";
    { Lock l; for (int t = 0; t < NUM_TILES; t++) { out += ","; out += cfg.tile[t].label; } }
    out += "\n";
    int len = coarse ? COARSE_LEN : FINE_LEN;
    int32_t last; { Lock l; last = coarse ? coarseLast : fineLast; }
    for (int32_t w = last - len + 1; last >= 0 && w <= last; w++) {
      float v[NUM_TILES]; bool any = false;
      { Lock l; for (int t = 0; t < NUM_TILES; t++) { v[t] = ringGet(coarse ? coarseH : fineH, len, last, t, w); any |= !isnan(v[t]); } }
      if (!any) continue;
      time_t ts = (time_t)w * (coarse ? 900 : 60);
      tm lt; localtime_r(&ts, &lt);
      char b[24]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
      out += b;
      for (int t = 0; t < NUM_TILES; t++) { out += ","; if (!isnan(v[t])) out += String(v[t], 3); }
      out += "\n";
      if (out.length() > 2048) { server.sendContent(out); out = ""; }
    }
    if (out.length()) server.sendContent(out);
    server.sendContent("");
  });
  server.on("/api/status", HTTP_GET, [] {   // read-only JSON for Home Assistant or curl
    JsonDocument d;
    {
      Lock l;
      JsonArray t = d["tiles"].to<JsonArray>();
      for (int i = 0; i < NUM_TILES; i++) {
        JsonObject o = t.add<JsonObject>();
        o["label"] = cfg.tile[i].label;
        if (tileState[i].valid) o["value"] = tileState[i].value; else o["value"] = nullptr;
        o["unit"] = tileSpec(cfg.tile[i]).unit;
      }
      JsonArray c = d["controllers"].to<JsonArray>();
      for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devUsed(cfg.dev[i])) continue;
        JsonObject o = c.add<JsonObject>();
        o["name"] = cfg.dev[i].name; o["host"] = cfg.dev[i].host; o["type"] = cfg.dev[i].type == DEV_DOSER ? "doser" : "profilux";
        o["product"] = dev[i].product;
        o["online"] = dev[i].everReplied && millis() - dev[i].lastReply < staleMs;
        o["alarm"] = dev[i].alarm; o["leak"] = dev[i].leak; o["firmware"] = dev[i].firmware;
      }
    }
    sendJson(d);
  });
  server.onNotFound([] {
    if (apActive) { server.sendHeader("Location", "http://192.168.8.1/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
}

// GET /api_wifi/v1/status → token. Blocking (≤ ~6 s).
static bool fetchIdentity(int i, const char* host) {
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  String url = String("http://") + host + "/api_wifi/v1/status";
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); snprintf(dev[i].status, sizeof dev[i].status, code < 0 ? "no answer" : "status HTTP %d", code); return false; }
  String body = http.getString();
  http.end();
  JsonDocument d;
  if (deserializeJson(d, body) != DeserializationError::Ok) { strlcpy(dev[i].status, "bad status JSON", 40); return false; }
  JsonObject cd = d["ConnectedDevice"];
  const char* mac = cd["MACaddress"] | "";
  uint32_t serial = cd["Serialnumber"] | 0u;
  uint8_t tok[8];
  if (!serial || !ghl::deriveToken(mac, serial, tok)) { strlcpy(dev[i].status, "no MAC/serial", 40); return false; }
  Lock l;
  memcpy(dev[i].token, tok, 8);
  dev[i].product = cd["ProductID"] | -1;
  dev[i].firmware = cd["FirmwareVersion"] | -1;
  dev[i].haveToken = true;
  dev[i].identityAt = millis();
  strlcpy(dev[i].status, "connecting", 40);
  return true;
}

static void onWs(int i, WStype_t type, uint8_t* p, size_t len) {
  DevState& d = dev[i];
  switch (type) {
    case WStype_CONNECTED: { Lock l; d.wsUp = true; d.waiting = false; d.lastRequest = 0; strlcpy(d.status, "connected", 40); } break;
    case WStype_DISCONNECTED: { Lock l; d.wsUp = false; d.waiting = false; if (d.haveToken) strlcpy(d.status, "reconnecting", 40); } break;
    case WStype_BIN: {
      Lock l;
      uint32_t now = millis();
      int n = ghl::parseRead(p, len, d.token, [&](uint16_t id, int32_t v) {
        if (id == ghl::REG_ALARM) d.alarm = v;
        else if (id == ghl::REG_LEAK) d.leak = v;
        else if (id == ghl::REG_RSSI) d.rssi = v;
        for (int t = 0; t < NUM_TILES; t++) {
          if (cfg.tile[t].device != i) continue;
          TileSpec s = tileSpec(cfg.tile[t]);
          if (s.reg == id) { tileState[t].value = v / s.divisor; tileState[t].valid = true; tileState[t].at = now; }
        }
      });
      if (n >= 0) {
        d.waiting = false; d.lastReply = now; d.everReplied = true; d.backoff = 0;
        snprintf(d.status, sizeof d.status, "ok");
      }
    } break;
    default: break;
  }
}

static void resetDevice(int i) {
  ws[i].disconnect();
  Lock l;
  dev[i].haveToken = false; dev[i].wsUp = false; dev[i].waiting = false; dev[i].nextAttempt = 0; dev[i].backoff = 0;
  for (int t = 0; t < NUM_TILES; t++) if (cfg.tile[t].device == i) tileState[t] = TileState();
}

static void pumpDevice(int i) {
  DevState& d = dev[i];
  char host[40]; bool enabled;
  { Lock l; strlcpy(host, cfg.dev[i].host, 40); enabled = cfg.dev[i].enabled && host[0]; }
  if (!enabled) { if (d.haveToken) resetDevice(i); strlcpy(d.status, "disabled", 40); return; }
  bool isDoser; { Lock l; isDoser = cfg.dev[i].type == DEV_DOSER; }
  if (isDoser) { if (d.haveToken) resetDevice(i); return; }   // doser is read on its slot, see readDoser()
  uint32_t now = millis();

  if (!d.haveToken) {
    if (now < d.nextAttempt) return;
    if (fetchIdentity(i, host)) {
      ws[i].begin(host, 80, "/ws");
      ws[i].setReconnectInterval(5000);
    } else {
      d.nextAttempt = millis() + BACKOFF_MS[min(d.backoff, 4)];
      d.backoff++;
    }
    return;
  }

  ws[i].loop();
  now = millis();

  if (d.waiting && now - d.lastRequest > REPLY_TIMEOUT_MS) {       // watchdog: drop and reconnect
    strlcpy(d.status, "no reply, reconnecting", 40);
    d.waiting = false;
    ws[i].disconnect();
  }

  uint32_t last = d.everReplied ? d.lastReply : d.identityAt;
  if (now - last > reidentifyMs && now - d.identityAt > reidentifyMs) {   // start over from the status endpoint
    resetDevice(i);
    strlcpy(d.status, "re-identifying", 40);
  }
}

// One batched read: alarm, leak, and every register a tile on this controller uses.
static bool sendRead(int i) {
  DevState& d = dev[i];
  if (!d.haveToken || !d.wsUp || d.waiting) return false;
  uint32_t now = millis();
  {
    uint16_t regs[2 + NUM_TILES]; uint8_t n = 0;
    regs[n++] = ghl::REG_ALARM; regs[n++] = ghl::REG_LEAK;
    {
      Lock l;
      for (int t = 0; t < NUM_TILES; t++) {
        if (cfg.tile[t].device != i || isPumpPreset(cfg.tile[t].preset)) continue;
        uint16_t r = tileSpec(cfg.tile[t]).reg;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= regs[k] == r;
        if (!dup) regs[n++] = r;
      }
    }
    uint8_t frame[64];
    size_t len = ghl::buildRead(d.token, regs, n, frame);
    ws[i].sendBIN(frame, len);
    d.waiting = true; d.lastRequest = now;
  }
  return true;
}

// ---------- Doser 2.1: one short TCP connection per pump, read-only ----------
static bool readDoserPump(const char* host, int pump, int32_t* ml) {
  const doser::Pump& p = doser::PUMP[pump - 1];
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClient c;
    int r = 0;
    if (c.connect(host, doser::PORT, 3000)) {
      c.write(p.req, doser::REQ_LEN);
      uint8_t buf[256]; size_t n = 0;
      uint32_t t0 = millis();
      while (millis() - t0 < 3000 && n < sizeof buf) {
        int a = c.available();
        if (a > 0) {
          n += c.read(buf + n, min((size_t)a, sizeof buf - n));
          r = doser::decode(buf, n, p.field, ml);
          if (r) break;
        } else if (!c.connected()) break;
        else delay(10);
      }
      c.stop();
    }
    if (r == 1) return true;
    delay(350);
  }
  return false;
}

// Reads every pump that a tile on this doser shows, then closes. Called on the doser's slot.
static void readDoser(int i) {
  char host[40]; int pumps[doser::PUMPS]; int np = 0;
  {
    Lock l;
    strlcpy(host, cfg.dev[i].host, 40);
    for (int p = 1; p <= doser::PUMPS; p++) {          // pumps shown on the Doser page or on a tile
      bool want = cfg.dev[i].pump[p - 1].show;
      for (int t = 0; t < NUM_TILES; t++)
        want |= cfg.tile[t].device == i && isPumpPreset(cfg.tile[t].preset) && cfg.tile[t].reg == p;
      if (want) pumps[np++] = p;
    }
  }
  if (!np) { Lock l; strlcpy(dev[i].status, "no pumps selected", 40); return; }
  int ok = 0;
  for (int k = 0; k < np; k++) {
    int32_t ml;
    if (!readDoserPump(host, pumps[k], &ml)) continue;
    ok++;
    Lock l;
    uint32_t now = millis();
    PumpState& ps = pumpState[i][pumps[k] - 1];
    ps.ml = ml; ps.valid = true; ps.at = now;
    for (int t = 0; t < NUM_TILES; t++) {
      const TileCfg& c = cfg.tile[t];
      if (c.device == i && isPumpPreset(c.preset) && c.reg == pumps[k]) {
        tileState[t].value = ml / tileSpec(c).divisor; tileState[t].valid = true; tileState[t].at = now;
      }
    }
  }
  Lock l;
  if (ok) { dev[i].lastReply = millis(); dev[i].everReplied = true; strlcpy(dev[i].status, "ok", 40); }
  else strlcpy(dev[i].status, "no answer on 10001", 40);
}

static void netTask(void*) {
  for (int i = 0; i < MAX_DEVICES; i++)
    ws[i].onEvent([i](WStype_t type, uint8_t* p, size_t len) { onWs(i, type, p, len); });

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  char ssid[33], pass[65], tz[48];
  { Lock l; strlcpy(ssid, cfg.ssid, 33); strlcpy(pass, cfg.pass, 65); strlcpy(tz, cfg.tz, 48); recalcStale(); }
  if (ssid[0]) { WiFi.mode(WIFI_STA); WiFi.begin(ssid, pass); }
  else startAP(false);
  setupWeb();
  historyBegin();

  uint32_t bootAt = millis(), staSince = 0, lastRtcWrite = 0;
  bool ntpStarted = false;
  int slotDev = -1; uint32_t slotStart = 0, nextSlot = 0; bool slotSent = true;

  for (;;) {
    uint32_t now = millis();
    if (apActive) dns.processNextRequest();
    server.handleClient();

    if (rebootPending) { flushFine(); delay(1500); ESP.restart(); }
    if (apRequest) { apRequest = false; startAP(false); }

    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !staUp) { staSince = now; snprintf(ipText, sizeof ipText, "%s", WiFi.localIP().toString().c_str()); }
    staUp = up;
    if (up) staRssi = WiFi.RSSI();

    // No Wi-Fi 30 s after boot → open the setup hotspot alongside the station
    if (ssid[0] && !up && !apActive && now - bootAt > 30000) startAP(true);
    // Auto-opened hotspot closes once Wi-Fi has been up a minute and nobody is on it
    if (apActive && apAuto && up && now - staSince > 60000 && WiFi.softAPgetStationNum() == 0) stopAP();

    if (up && !ntpStarted) { configTzTime(tz, "pool.ntp.org", "time.nist.gov"); ntpStarted = true; }
    if (ntpStarted && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && now - lastRtcWrite > 3600000UL) {
      systemToRtc(); lastRtcWrite = now;
    }

    if (reconfigure) {
      reconfigure = false;
      { Lock l; strlcpy(tz, cfg.tz, 48); recalcStale(); }
      setenv("TZ", tz, 1); tzset();
      for (int i = 0; i < MAX_DEVICES; i++) resetDevice(i);
    }

    // OTA: on request from the setup page, and a daily check (first one 2 min after Wi-Fi comes up)
    static uint32_t nextOtaCheck = 0;
    if (up && !nextOtaCheck) nextOtaCheck = now + 120000;
    bool daily; { Lock l; daily = cfg.otaCheck; }
    if (up && (otaCheckReq || (daily && nextOtaCheck && (int32_t)(now - nextOtaCheck) >= 0))) {
      otaCheckReq = false; nextOtaCheck = millis() + 86400000UL;
      otaCheckGithub();
    }
    if (up && otaInstallReq) { otaInstallReq = false; otaInstallGithub(); }

    if (up) {
      for (int i = 0; i < MAX_DEVICES; i++) pumpDevice(i);

      // Slot scheduler: every 30 s hand the turn to the next enabled controller.
      now = millis();
      if ((int32_t)(now - nextSlot) >= 0) {
        int pick = -1;
        for (int k = 1; k <= MAX_DEVICES; k++) {
          int c = (slotDev + k + MAX_DEVICES) % MAX_DEVICES;
          bool en; { Lock l; en = devUsed(cfg.dev[c]); }
          if (en) { pick = c; break; }
        }
        slotDev = pick; slotStart = now; slotSent = pick < 0; nextSlot = now + SLOT_MS;
      }
      // Send as soon as the controller's socket is up, within the first 15 s of its slot.
      if (!slotSent) {
        bool isDoser; { Lock l; isDoser = cfg.dev[slotDev].type == DEV_DOSER; }
        if (isDoser) { readDoser(slotDev); slotSent = true; }
        else if (now - slotStart < SLOT_SEND_WINDOW_MS && sendRead(slotDev)) slotSent = true;
      }
    }

    historyTick();   // one point per minute per tile (NaN when there's no fresh reading)
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================================================================
// UI
// ==========================================================================================

M5Canvas cv(&M5.Display);

constexpr uint32_t COL_BG = 0xEEF1F5, COL_CARD = 0xFFFFFF, COL_INK = 0x1D2433, COL_MUTE = 0x6B7385,
                   COL_LINE = 0xD5DAE2, COL_RED = 0xD93025, COL_AMBER = 0xE8A317, COL_ACC = 0x2F6FED;

enum Screen { HOME, DETAIL, MENU, ALARMS, SETTINGS, SETTINGS2, NETWORK, ABOUT, SETUP };
Screen screen = HOME, alarmsFrom = HOME;
int detailTile = 0, detailRange = 2;   // index into RANGE_MIN

struct AlarmItem { char text[48]; bool offline; };
AlarmItem alarms[12]; int alarmCount = 0, offlineCount = 0, prevAlarmCount = 0;
bool tileAlarm[NUM_TILES], devOffline[MAX_DEVICES];

// snapshot taken once per frame
Config sc;
DevState sd[MAX_DEVICES];
TileState st[NUM_TILES];
PumpState sp[MAX_DEVICES][NUM_PUMPS];
bool pumpLow[MAX_DEVICES][NUM_PUMPS];
int doserCount = 0;  // enabled dosers; the Doser page lists their shown pumps
int homePage = 0;    // 0 = tiles, 1 = doser
uint32_t pageAt = 0, touchAt = 0;

static void takeSnapshot() {
  Lock l;
  sc = cfg;
  for (int i = 0; i < MAX_DEVICES; i++) sd[i] = dev[i];
  for (int i = 0; i < NUM_TILES; i++) st[i] = tileState[i];
  memcpy(sp, pumpState, sizeof sp);
}

static void addAlarm(bool offline, const char* fmt, ...) {
  if (alarmCount + offlineCount >= 12) return;
  AlarmItem& a = alarms[alarmCount + offlineCount];
  va_list ap; va_start(ap, fmt); vsnprintf(a.text, sizeof a.text, fmt, ap); va_end(ap);
  a.offline = offline;
  if (offline) offlineCount++; else alarmCount++;
}

static void evaluateAlarms() {
  alarmCount = offlineCount = 0;
  uint32_t now = millis();
  for (int i = 0; i < MAX_DEVICES; i++) {
    const DevState& d = sd[i];
    devOffline[i] = false;
    if (!devUsed(sc.dev[i])) continue;
    bool online = d.everReplied && now - d.lastReply < staleMs;
    if (!online) {
      if (now > staleMs) { devOffline[i] = true; addAlarm(true, "%s not responding", sc.dev[i].name); }
      continue;
    }
    if (sc.dev[i].type == DEV_DOSER) continue;
    // sensor alarm mask: name each set bit after the tile that shows that sensor, if there is one
    for (int b = 0; b < 32; b++) {
      if (!((uint32_t)d.alarm & (1u << b))) continue;
      const char* label = nullptr;
      for (int t = 0; t < NUM_TILES && !label; t++)
        if (sc.tile[t].device == i && (sc.tile[t].alarmMask & (1u << b))) label = sc.tile[t].label;
      if (label) addAlarm(false, "%s: %s alarm", sc.dev[i].name, label);
      else addAlarm(false, "%s: sensor %d alarm", sc.dev[i].name, b + 1);
    }
    if (d.leak) addAlarm(false, "%s: LEAK / level alarm", sc.dev[i].name);
  }
  // doser pumps below their low percent (single source for pump alarms, page and tiles)
  doserCount = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    bool isDoser = devUsed(sc.dev[i]) && sc.dev[i].type == DEV_DOSER;
    doserCount += isDoser;
    for (int p = 0; p < NUM_PUMPS; p++) {
      const PumpCfg& pc = sc.dev[i].pump[p];
      const PumpState& ps = sp[i][p];
      bool fresh = ps.valid && now - ps.at < staleMs;
      pumpLow[i][p] = isDoser && fresh && pc.capML && pc.lowPct && ps.ml * 100LL < (int64_t)pc.lowPct * pc.capML;
      if (pumpLow[i][p]) addAlarm(false, "%s low: %d%%", pc.name, (int)(ps.ml * 100LL / pc.capML));
    }
  }
  for (int t = 0; t < NUM_TILES; t++) {
    const TileCfg& c = sc.tile[t];
    const DevState& d = sd[c.device];
    bool fresh = st[t].valid && now - st[t].at < staleMs;
    bool ctl = fresh && c.alarmMask && ((uint32_t)d.alarm & c.alarmMask);
    bool pump = isPumpPreset(c.preset);   // doser tiles follow their pump's low % setting
    bool lim = !pump && fresh && c.useLimits && (st[t].value < c.low || st[t].value > c.high);
    bool low = pump && c.reg >= 1 && c.reg <= NUM_PUMPS && pumpLow[c.device][c.reg - 1];
    tileAlarm[t] = ctl || lim || low;
    if (lim) {
      TileSpec s = tileSpec(c);
      addAlarm(false, "%s out of range: %.*f", c.label, s.decimals, st[t].value);
    }
  }
  // keep alarms ahead of offline notices in the list
  for (int a = 0, b = 0; b < alarmCount + offlineCount; b++)
    if (!alarms[b].offline) { AlarmItem tmp = alarms[a]; alarms[a] = alarms[b]; alarms[b] = tmp; a++; }
}

// ---------- drawing primitives ----------
static void text(const char* s, int x, int y, const lgfx::IFont* f, uint32_t col, lgfx::textdatum_t datum) {
  cv.setFont(f); cv.setTextColor(C(col)); cv.setTextDatum(datum); cv.drawString(s, x, y);
}

static void drawHamburger(int x, int y, uint32_t col) {
  for (int k = 0; k < 3; k++) cv.fillRoundRect(x, y + k * 7, 22, 3, 1, C(col));
}

static void drawBell(int cx, int cy, uint32_t col) {
  cv.fillCircle(cx, cy - 3, 7, C(col));
  cv.fillRect(cx - 7, cy - 3, 15, 8, C(col));
  cv.fillRoundRect(cx - 10, cy + 4, 21, 3, 1, C(col));
  cv.fillCircle(cx, cy + 9, 2, C(col));
}

static void drawBack(int x, int y, uint32_t col) {
  cv.drawWideLine(x + 10, y, x, y + 10, 1.6f, C(col));
  cv.drawWideLine(x, y + 10, x + 10, y + 20, 1.6f, C(col));
}

// value + unit centred on cx; "F"/"C" get a drawn degree sign (GFX fonts are ASCII only)
static void drawValue(const char* val, const char* unit, int cx, int cy, const lgfx::IFont* vf, const lgfx::IFont* uf, uint32_t col) {
  cv.setFont(vf); int vw = cv.textWidth(val);
  bool deg = (strcmp(unit, "F") == 0 || strcmp(unit, "C") == 0);
  cv.setFont(uf); int uw = unit[0] ? cv.textWidth(unit) + (deg ? 9 : 0) + 4 : 0;
  int x0 = cx - (vw + uw) / 2;
  text(val, x0, cy, vf, col, middle_left);
  if (unit[0]) {
    int ux = x0 + vw + 4;
    cv.setFont(vf); int top = cy - cv.fontHeight() / 2 + 8;
    if (deg) { cv.drawCircle(ux + 3, top + 3, 3, C(col)); cv.drawCircle(ux + 3, top + 3, 2, C(col)); ux += 9; }
    text(unit, ux, top, uf, col, top_left);
  }
}

static void fmtValue(char* out, size_t n, int t) {
  TileSpec s = tileSpec(sc.tile[t]);
  bool fresh = st[t].valid && millis() - st[t].at < staleMs;
  if (!fresh) strlcpy(out, "--", n);
  else snprintf(out, n, "%.*f", s.decimals, st[t].value);
}

static void fmtAge(char* out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

static void drawClock(int x, int y, lgfx::textdatum_t datum, const lgfx::IFont* f, uint32_t col) {
  char buf[16] = "--:--";
  if (timeValid()) {
    time_t now = time(nullptr); tm lt; localtime_r(&now, &lt);
    if (sc.clock24) strftime(buf, sizeof buf, "%H:%M", &lt);
    else { int h = lt.tm_hour % 12; if (!h) h = 12; snprintf(buf, sizeof buf, "%d:%02d %s", h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM"); }
  }
  text(buf, x, y, f, col, datum);
}

static void drawTitleBar(const char* title, bool back) {
  cv.fillRect(0, 0, 320, 38, C(COL_CARD));
  cv.drawFastHLine(0, 38, 320, C(COL_LINE));
  if (back) drawBack(12, 9, COL_INK);
  text(title, back ? 34 : 12, 19, &fonts::FreeSansBold12pt7b, COL_INK, middle_left);
  drawClock(310, 19, middle_right, &fonts::FreeSans9pt7b, COL_MUTE);
}

// ---------- home ----------
const int TILE_X[] = {6, 164, 6, 164}, TILE_Y[] = {44, 44, 142, 142}, TW = 150, TH = 92;

static void drawDoserPage();

// ---------- page 1 graph: pH (left scale) and temperature (right scale) together ----------
int homeRange = 1;   // index into RANGE_MIN; tap the graph to change

static void drawHomeGraph() {
  const int cx = 6, cy = 142, cw = 308, ch = 94;
  const int gx = 42, gw = 236, gy = cy + 10, gh = ch - 28;
  cv.fillRoundRect(cx, cy, cw, ch, 10, C(COL_CARD));
  for (int k = 0; k <= 2; k++) cv.drawFastHLine(gx, gy + k * gh / 2, gw, C(0xEEF0F3));

  // pH on the left scale, temperature on the right, each in its tile's colour
  int left = sc.tile[1].preset == P_PH ? 1 : (sc.tile[0].preset == P_PH ? 0 : 1);
  int right = 1 - left;
  float* buf = gbuf;
  bool any = false;

  for (int side = 0; side < 2; side++) {
    int t = side == 0 ? left : right;
    TileSpec s = tileSpec(sc.tile[t]);
    int n = gatherRange(t, homeRange, buf), want = n;
    float lo = INFINITY, hi = -INFINITY; int cnt = 0;
    for (int k = 0; k < n; k++) if (!isnan(buf[k])) { lo = min(lo, buf[k]); hi = max(hi, buf[k]); cnt++; }
    uint32_t col = sc.tile[t].color;
    int lx = side == 0 ? gx - 4 : gx + gw + 4;
    lgfx::textdatum_t dt = side == 0 ? middle_right : middle_left;
    if (cnt < 2) { text("--", lx, gy + gh / 2, &fonts::Font0, col, dt); continue; }
    any = true;
    float minSpan = 4.0f * powf(10, -(int)s.decimals);
    if (hi - lo < minSpan) { float m = (hi + lo) / 2; lo = m - minSpan / 2; hi = m + minSpan / 2; }
    float pad = (hi - lo) * 0.12f; lo -= pad; hi += pad;
    for (int k = 0; k <= 2; k++) {
      char lb[12]; snprintf(lb, sizeof lb, "%.*f", s.decimals, hi - (hi - lo) * k / 2);
      text(lb, lx, gy + k * gh / 2, &fonts::Font0, col, dt);
    }
    auto Y = [&](float v) { return gy + gh - (v - lo) / (hi - lo) * gh; };
    int offset = want - n;
    float px0 = NAN, py0 = NAN;
    uint16_t lc = C(col);
    for (int c = 0; c < gw; c++) {
      int a = c * want / gw - offset, b = (c + 1) * want / gw - offset;
      if (b <= 0) continue;
      a = max(a, 0); b = min(max(b, a + 1), n);
      float acc = 0; int m = 0;
      for (int k = a; k < b; k++) if (!isnan(buf[k])) { acc += buf[k]; m++; }
      if (!m) { px0 = NAN; continue; }
      float px = gx + c, py = Y(acc / m);
      if (!isnan(px0)) cv.drawWideLine(px0, py0, px, py, 1.3f, lc); else cv.fillCircle(px, py, 1, lc);
      px0 = px; py0 = py;
    }
  }
  if (!any) text("Collecting history", cx + cw / 2, gy + gh / 2, &fonts::FreeSans9pt7b, COL_MUTE, middle_center);
  int by = cy + ch - 9;
  char rl[8]; snprintf(rl, sizeof rl, "-%s", RANGE_TXT[homeRange]);
  text(rl, gx, by, &fonts::Font0, COL_MUTE, middle_left);
  text("tap to change range", cx + cw / 2, by, &fonts::Font0, COL_MUTE, middle_center);
  text("now", gx + gw, by, &fonts::Font0, COL_MUTE, middle_right);
}

static void drawHome() {
  cv.fillRect(0, 0, 320, 38, C(COL_CARD));
  cv.drawFastHLine(0, 38, 320, C(COL_LINE));
  drawHamburger(12, 12, COL_INK);
  drawBell(62, 18, alarmCount ? COL_RED : COL_INK);
  if (alarmCount || offlineCount) {
    int n = alarmCount ? alarmCount : offlineCount;
    char b[4]; snprintf(b, sizeof b, "%d", min(n, 9));
    cv.fillCircle(73, 9, 8, C(alarmCount ? COL_RED : COL_AMBER));
    text(b, 73, 9, &fonts::Font2, 0xFFFFFF, middle_center);
  }
  if (!staUp) text(apActive ? "Setup hotspot on" : "No Wi-Fi", 160, 19, &fonts::FreeSansBold9pt7b, COL_RED, middle_center);
  drawClock(310, 19, middle_right, &fonts::FreeSansBold12pt7b, COL_INK);
  if (doserCount) {                                   // page dots
    for (int k = 0; k < 2; k++) cv.fillCircle(152 + k * 16, 33, 3, C(k == homePage ? COL_ACC : COL_LINE));
  }
  if (homePage == 1 && doserCount) { drawDoserPage(); return; }

  uint32_t now = millis();
  for (int t = 0; t < 2; t++) {                      // page 1: temperature and pH tiles on top
    const TileCfg& c = sc.tile[t];
    int x = TILE_X[t], y = TILE_Y[t];
    bool alarm = tileAlarm[t];
    bool offline = devOffline[c.device];
    uint32_t bg = alarm ? COL_RED : COL_CARD;
    uint32_t ink = alarm ? 0xFFFFFF : (offline ? COL_MUTE : COL_INK);
    uint32_t sub = alarm ? 0xFFE3E0 : COL_MUTE;

    cv.fillRoundRect(x, y, TW, TH, 10, C(alarm ? COL_RED : (offline ? COL_LINE : c.color)));
    cv.fillRoundRect(x, y + 5, TW, TH - 5, 10, C(bg));
    cv.fillRect(x, y + 5, TW, 10, C(bg));

    text(c.label, x + 10, y + 11, &fonts::FreeSans9pt7b, sub, top_left);
    char val[16]; fmtValue(val, sizeof val, t);
    drawValue(val, tileSpec(c).unit, x + TW / 2, y + 52, &fonts::FreeSansBold24pt7b, &fonts::FreeSansBold9pt7b, ink);

    char foot[32];
    const DevState& d = sd[c.device];
    if (!devUsed(sc.dev[c.device])) strlcpy(foot, sc.dev[c.device].host[0] ? "controller disabled" : "set up on the setup page", sizeof foot);
    else if (offline) strlcpy(foot, "offline", sizeof foot);
    else if (!st[t].valid) strlcpy(foot, d.status, sizeof foot);
    else if (alarm && c.alarmMask && ((uint32_t)d.alarm & c.alarmMask)) strlcpy(foot, "CONTROLLER ALARM", sizeof foot);
    else if (alarm) strlcpy(foot, isPumpPreset(c.preset) ? "LOW" : "OUT OF RANGE", sizeof foot);
    else foot[0] = 0;
    if (foot[0]) text(foot, x + 10, y + TH - 7, &fonts::Font2, sub, bottom_left);
  }
  (void)now;
  drawHomeGraph();
}

// ---------- doser page: every shown pump, percent full ----------
// Lists the shown pumps of every enabled doser, up to six rows.
static void drawDoserPage() {
  constexpr int MAXROWS = 6;
  int rowDev[MAXROWS], rowPump[MAXROWS], n = 0;
  for (int i = 0; i < MAX_DEVICES && n < MAXROWS; i++) {
    if (!devUsed(sc.dev[i]) || sc.dev[i].type != DEV_DOSER) continue;
    for (int p = 0; p < NUM_PUMPS && n < MAXROWS; p++) if (sc.dev[i].pump[p].show) { rowDev[n] = i; rowPump[n] = p; n++; }
  }
  uint32_t now = millis();
  if (!n) { text("No pumps selected on the setup page", 160, 140, &fonts::FreeSans9pt7b, COL_MUTE, middle_center); return; }
  int h = min(64, 192 / n), y0 = 44;
  for (int k = 0; k < n; k++) {
    int i = rowDev[k], p = rowPump[k], y = y0 + k * h;
    const PumpCfg& pc = sc.dev[i].pump[p];
    const PumpState& ps = sp[i][p];
    bool offline = devOffline[i];
    bool fresh = ps.valid && now - ps.at < staleMs;
    bool low = pumpLow[i][p];
    cv.fillRoundRect(6, y, 308, h - 4, 8, C(low ? COL_RED : COL_CARD));
    uint32_t ink = low ? 0xFFFFFF : (offline || !fresh ? COL_MUTE : COL_INK);
    int cy = y + (h - 4) / 2;
    text(pc.name, 16, cy, &fonts::FreeSansBold9pt7b, ink, middle_left);
    char v[16];
    if (!fresh) strlcpy(v, "--", sizeof v);
    else if (pc.capML) snprintf(v, sizeof v, "%d%%", (int)(ps.ml * 100LL / pc.capML));
    else if (ps.ml >= 10000) snprintf(v, sizeof v, "%.1f L", ps.ml / 1000.0f);
    else snprintf(v, sizeof v, "%d mL", (int)ps.ml);
    text(v, 304, cy, h >= 40 ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b, ink, middle_right);
    // bar
    if (pc.capML) {
      int bx = 130, bw = 100, bh = min(14, h / 3);
      cv.fillRoundRect(bx, cy - bh / 2, bw, bh, bh / 2, C(low ? 0xF4A9A3 : COL_LINE));
      if (fresh) {
        int f = constrain((int)(ps.ml * (int64_t)bw / pc.capML), 0, bw);
        if (f > 0) cv.fillRoundRect(bx, cy - bh / 2, max(f, bh), bh, bh / 2, C(low ? 0xFFFFFF : (f * 100 / bw < 25 ? COL_AMBER : COL_ACC)));
      }
    }
  }
}

// ---------- detail ----------
static void drawDetail() {
  int t = detailTile;
  const TileCfg& c = sc.tile[t];
  TileSpec s = tileSpec(c);
  cv.fillRect(0, 0, 320, 38, C(COL_CARD));
  cv.drawFastHLine(0, 38, 320, C(COL_LINE));
  drawBack(12, 9, COL_INK);
  text(c.label, 34, 19, &fonts::FreeSansBold12pt7b, COL_INK, middle_left);
  char val[16]; fmtValue(val, sizeof val, t);
  drawValue(val, s.unit, 262, 19, &fonts::FreeSansBold12pt7b, &fonts::FreeSans9pt7b, tileAlarm[t] ? COL_RED : COL_INK);

  // range chips
  for (int r = 0; r < NRANGE; r++) {
    int x = 8 + r * 50;
    bool sel = r == detailRange;
    cv.fillRoundRect(x, 46, 44, 24, 12, C(sel ? COL_ACC : COL_CARD));
    if (!sel) cv.drawRoundRect(x, 46, 44, 24, 12, C(COL_LINE));
    text(RANGE_TXT[r], x + 22, 58, &fonts::FreeSansBold9pt7b, sel ? 0xFFFFFF : COL_INK, middle_center);
  }

  // gather the whole window (oldest → newest)
  float* buf = gbuf;
  int n = gatherRange(t, detailRange, buf), want = n;
  float lo = INFINITY, hi = -INFINITY, sum = 0; int cnt = 0;
  for (int k = 0; k < n; k++) if (!isnan(buf[k])) { lo = min(lo, buf[k]); hi = max(hi, buf[k]); sum += buf[k]; cnt++; }

  const int gx = 44, gy = 92, gw = 268, gh = 124;
  cv.fillRoundRect(4, 74, 312, 162, 8, C(COL_CARD));
  if (cnt) {
    char st1[48];
    snprintf(st1, sizeof st1, "lo %.*f   hi %.*f   avg %.*f", s.decimals, lo, s.decimals, hi, s.decimals, sum / cnt);
    text(st1, 308, 82, &fonts::Font2, COL_MUTE, middle_right);
  }
  if (cnt < 2) {
    text("Collecting history", 160, 140, &fonts::FreeSansBold9pt7b, COL_MUTE, middle_center);
    text("One point a minute, saved through power cuts.", 160, 164, &fonts::Font2, COL_MUTE, middle_center);
    return;
  }

  float ylo = lo, yhi = hi;
  bool pumpTile = isPumpPreset(c.preset);
  if (c.useLimits) { ylo = min(ylo, c.low); if (!pumpTile) yhi = max(yhi, c.high); }
  float minSpan = 5.0f * powf(10, -(int)s.decimals);
  if (yhi - ylo < minSpan) { float m = (yhi + ylo) / 2; ylo = m - minSpan / 2; yhi = m + minSpan / 2; }
  float pad = (yhi - ylo) * 0.1f; ylo -= pad; yhi += pad;
  auto Y = [&](float v) { return gy + gh - (v - ylo) / (yhi - ylo) * gh; };

  for (int k = 0; k <= 4; k++) {
    int yy = gy + k * gh / 4;
    cv.drawFastHLine(gx, yy, gw, C(0xEEF0F3));
    char lbl[12]; snprintf(lbl, sizeof lbl, "%.*f", s.decimals, yhi - (yhi - ylo) * k / 4);
    text(lbl, gx - 4, yy, &fonts::Font0, COL_MUTE, middle_right);
  }
  char sl[12]; snprintf(sl, sizeof sl, "-%s", RANGE_TXT[detailRange]);
  text(sl, gx, gy + gh + 4, &fonts::Font0, COL_MUTE, top_left);
  text("now", gx + gw, gy + gh + 4, &fonts::Font0, COL_MUTE, top_right);

  if (c.useLimits) {
    for (float lim : {c.low, c.high}) {
      if (pumpTile && lim == c.high) continue;
      int yy = (int)Y(lim);
      for (int x = gx; x < gx + gw; x += 8) cv.drawFastHLine(x, yy, 4, C(COL_RED));
    }
  }

  // average samples into columns across the full range width; data fills from the right
  float prevX = NAN, prevY = NAN;
  uint16_t lineCol = C(tileAlarm[t] ? COL_RED : c.color);
  int offset = 0;
  for (int col = 0; col < gw; col++) {
    int a = col * want / gw - offset, b = (col + 1) * want / gw - offset;
    if (b <= 0) continue;
    a = max(a, 0); b = min(max(b, a + 1), n);
    float acc = 0; int m = 0;
    for (int k = a; k < b; k++) if (!isnan(buf[k])) { acc += buf[k]; m++; }
    if (!m) { prevX = NAN; continue; }
    float px = gx + col, py = Y(acc / m);
    if (!isnan(prevX)) cv.drawWideLine(prevX, prevY, px, py, 1.2f, lineCol);
    else cv.fillCircle(px, py, 1, lineCol);
    prevX = px; prevY = py;
  }
}

// ---------- menu / lists ----------
const char* MENU_ITEMS[] = {"Alarms", "Display settings", "Alarm settings", "Network & setup", "About"};
constexpr int MENU_N = 5, MENU_H = 38;

static void drawRow(int y, const char* left, const char* right, uint32_t rightCol = COL_MUTE, int h = 32) {
  cv.drawFastHLine(12, y + h - 1, 296, C(COL_LINE));
  text(left, 14, y + h / 2, &fonts::FreeSans9pt7b, COL_INK, middle_left);
  if (right) text(right, 306, y + h / 2, &fonts::FreeSansBold9pt7b, rightCol, middle_right);
}

static void drawMenu() {
  drawTitleBar("Menu", true);
  for (int k = 0; k < MENU_N; k++) {
    int y = 44 + k * MENU_H, cy = y + (MENU_H - 4) / 2;
    cv.fillRoundRect(8, y, 304, MENU_H - 4, 8, C(COL_CARD));
    text(MENU_ITEMS[k], 22, cy, &fonts::FreeSansBold12pt7b, COL_INK, middle_left);
    if (k == 0 && (alarmCount || offlineCount)) {
      char b[8]; snprintf(b, sizeof b, "%d", alarmCount + offlineCount);
      cv.fillCircle(280, cy, 11, C(alarmCount ? COL_RED : COL_AMBER));
      text(b, 280, cy, &fonts::FreeSansBold9pt7b, 0xFFFFFF, middle_center);
    } else {
      cv.drawWideLine(282, cy - 7, 289, cy, 1.4f, C(COL_MUTE));
      cv.drawWideLine(289, cy, 282, cy + 7, 1.4f, C(COL_MUTE));
    }
  }
}

static void drawAlarms() {
  drawTitleBar("Alarms", true);
  cv.fillRoundRect(8, 44, 304, 190, 8, C(COL_CARD));
  if (!alarmCount && !offlineCount) {
    text("No active alarms", 160, 130, &fonts::FreeSansBold12pt7b, COL_MUTE, middle_center);
    return;
  }
  for (int k = 0; k < alarmCount + offlineCount && k < 6; k++) {
    int y = 50 + k * 30;
    cv.fillCircle(24, y + 14, 6, C(alarms[k].offline ? COL_AMBER : COL_RED));
    text(alarms[k].text, 40, y + 14, &fonts::FreeSans9pt7b, COL_INK, middle_left);
  }
  text("Alarms clear when the controller clears them.", 160, 226, &fonts::Font2, COL_MUTE, bottom_center);
}

const char* BRIGHT_TXT[] = {"25%", "50%", "75%", "100%"};
const char* GLOW_TXT[] = {"Low", "Medium", "High"};

constexpr int SET_ROW_H = 32, SET_ROWS = 6, SET2_ROW_H = 32, SET2_ROWS = 4;
static const uint8_t ROTATE_STEPS[] = {0, 5, 10, 15, 30, 60};   // tap cycles: Off, 5, 10, 15, 30, 60 s

static const char* NIGHT_TXT[] = {"Off", "Dim", "Screen off"};
static const uint8_t NIGHT_HOURS[][2] = {{21, 6}, {21, 7}, {22, 6}, {22, 7}, {23, 6}, {23, 7}, {0, 7}};   // tap cycles these

static uint32_t delArmedUntil = 0, deletedAt = 0;   // two-tap confirm for Delete history

static void drawSettings() {
  drawTitleBar("Display", true);
  cv.fillRoundRect(8, 42, 304, 196, 8, C(COL_CARD));
  const int H = SET_ROW_H;
  char rot[12];
  if (sc.rotateSec) snprintf(rot, sizeof rot, "Every %ds", sc.rotateSec); else strlcpy(rot, "Off", sizeof rot);
  char hrs[16]; snprintf(hrs, sizeof hrs, "%02d:00-%02d:00", sc.nightStart, sc.nightEnd);
  drawRow(44 + 0 * H, "Screen brightness", BRIGHT_TXT[sc.brightness], COL_ACC, H);
  drawRow(44 + 1 * H, "Page change", rot, COL_ACC, H);
  drawRow(44 + 2 * H, "Clock", sc.clock24 ? "24-hour" : "12-hour", COL_ACC, H);
  drawRow(44 + 3 * H, "Night mode", NIGHT_TXT[sc.nightMode], COL_ACC, H);
  drawRow(44 + 4 * H, "Night hours", hrs, COL_ACC, H);
  bool armed = (int32_t)(delArmedUntil - millis()) > 0;
  bool done = deletedAt && millis() - deletedAt < 3000;
  drawRow(44 + 5 * H, "Delete history", done ? "Deleted" : armed ? "Tap again to delete" : "Tap twice", armed ? COL_RED : COL_ACC, H);
}

static void drawSettings2() {
  drawTitleBar("Alarm settings", true);
  cv.fillRoundRect(8, 42, 304, 4 * SET2_ROW_H + 4, 8, C(COL_CARD));
  const int H = SET2_ROW_H;
  drawRow(44 + 0 * H, "Red glow on alarm", sc.glowAlarm ? "On" : "Off", COL_ACC, H);
  drawRow(44 + 1 * H, "Amber glow when offline", sc.glowOffline ? "On" : "Off", COL_ACC, H);
  drawRow(44 + 2 * H, "Glow brightness", GLOW_TXT[sc.glowLevel], COL_ACC, H);
  drawRow(44 + 3 * H, "Beep on new alarm", sc.beep ? "On" : "Off", COL_ACC, H);
  text("Pump sizes and low % are on the setup page.", 160, 222, &fonts::Font2, COL_MUTE, middle_center);
}

static void drawNetwork() {
  drawTitleBar("Network & setup", true);
  cv.fillRoundRect(8, 42, 304, 150, 8, C(COL_CARD));
  char b[48];
  if (staUp) snprintf(b, sizeof b, "%s  (%d dBm)", sc.ssid, staRssi); else strlcpy(b, sc.ssid[0] ? "not connected" : "not set", sizeof b);
  drawRow(44, "Wi-Fi", b, staUp ? COL_INK : COL_RED);
  snprintf(b, sizeof b, "http://%s/", staUp ? ipText : "-");
  drawRow(76, "Setup page", b, COL_ACC);
  // controllers: rows shrink to fit up to six in the space above the hotspot button
  int idx[MAX_DEVICES], n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) if (sc.dev[i].host[0]) idx[n++] = i;
  if (!n) text("No controllers yet: add them on the setup page", 160, 150, &fonts::Font2, COL_MUTE, middle_center);
  int h = n <= 2 ? 32 : (n <= 4 ? 22 : 15);
  const lgfx::IFont* f = h >= 32 ? (const lgfx::IFont*)&fonts::FreeSans9pt7b : (const lgfx::IFont*)&fonts::Font2;
  if (h < 22) f = &fonts::Font0;
  for (int k = 0; k < n; k++) {
    int i = idx[k], y = 108 + k * h;
    const DevState& d = sd[i];
    bool online = d.everReplied && millis() - d.lastReply < staleMs;
    if (!sc.dev[i].enabled) snprintf(b, sizeof b, "%s  disabled", sc.dev[i].host);
    else if (online && sc.dev[i].type == DEV_DOSER) snprintf(b, sizeof b, "%s  doser ok", sc.dev[i].host);
    else if (online) snprintf(b, sizeof b, "%s  fw %d.%02d", sc.dev[i].host, d.firmware / 100, d.firmware % 100);
    else snprintf(b, sizeof b, "%s  %s", sc.dev[i].host, d.status);
    cv.drawFastHLine(12, y + h - 1, 296, C(COL_LINE));
    text(sc.dev[i].name, 14, y + h / 2, f, COL_INK, middle_left);
    text(b, 306, y + h / 2, f, online || !sc.dev[i].enabled ? COL_INK : COL_AMBER, middle_right);
  }
  bool on = apActive;
  cv.fillRoundRect(8, 198, 304, 36, 8, C(on ? COL_CARD : COL_ACC));
  if (on) { snprintf(b, sizeof b, "Hotspot %s  192.168.8.1", apName); text(b, 160, 216, &fonts::FreeSansBold9pt7b, COL_ACC, middle_center); }
  else text("Start setup hotspot", 160, 216, &fonts::FreeSansBold9pt7b, 0xFFFFFF, middle_center);
}

static void drawAbout() {
  drawTitleBar("About", true);
  cv.fillRoundRect(8, 44, 304, 190, 8, C(COL_CARD));
  char b[64]; snprintf(b, sizeof b, "Sentinel Glance v%s", FW_VERSION);
  text(b, 20, 56, &fonts::FreeSansBold12pt7b, COL_INK, top_left);
  text("Reads GHL ProfiLux controllers and", 20, 90, &fonts::FreeSans9pt7b, COL_INK, top_left);
  text("dosers over Wi-Fi. Read-only: it never", 20, 112, &fonts::FreeSans9pt7b, COL_INK, top_left);
  text("writes settings to a controller.", 20, 134, &fonts::FreeSans9pt7b, COL_INK, top_left);
  { Lock l;
    if (otaProgress >= 0) snprintf(b, sizeof b, "Updating: %d%%", otaProgress);
    else if (otaLatest[0] && versionNewer(otaLatest, FW_VERSION)) snprintf(b, sizeof b, "Update %s available on the setup page", otaLatest);
    else strlcpy(b, otaStatus, sizeof b); }
  if (b[0]) text(b, 20, 160, &fonts::Font2, COL_ACC, top_left);
  text("TelegrahamTested", 20, 180, &fonts::FreeSansBold9pt7b, COL_ACC, top_left);
  snprintf(b, sizeof b, "Battery %d%%  %s", M5.Power.getBatteryLevel(), M5.Power.isCharging() ? "charging" : "");
  text(b, 20, 204, &fonts::Font2, COL_MUTE, top_left);
}

static void drawSetup() {
  cv.fillScreen(C(COL_BG));
  text("Sentinel Glance setup", 160, 30, &fonts::FreeSansBold12pt7b, COL_INK, middle_center);
  cv.fillRoundRect(12, 54, 296, 150, 10, C(COL_CARD));
  text("1. On your phone or Mac, join Wi-Fi", 24, 70, &fonts::FreeSans9pt7b, COL_INK, top_left);
  text(apName, 160, 102, &fonts::FreeSansBold12pt7b, COL_ACC, middle_center);
  text("2. Open this address in a browser", 24, 124, &fonts::FreeSans9pt7b, COL_INK, top_left);
  text("192.168.8.1", 160, 158, &fonts::FreeSansBold12pt7b, COL_ACC, middle_center);
  text("3. Pick your network and save", 24, 178, &fonts::FreeSans9pt7b, COL_INK, top_left);
}

// ---------- input ----------
static void applySetting2(int row) {
  Lock l;
  switch (row) {
    case 0: cfg.glowAlarm = !cfg.glowAlarm; break;
    case 1: cfg.glowOffline = !cfg.glowOffline; break;
    case 2: cfg.glowLevel = (cfg.glowLevel + 1) % 3; break;
    case 3: cfg.beep = !cfg.beep; break;
  }
  saveConfig(cfg);
}

static void applySetting(int row) {
  Lock l;
  switch (row) {
    case 0: cfg.brightness = (cfg.brightness + 1) % 4; break;
    case 1: {
      int k = 0, n = sizeof ROTATE_STEPS;
      while (k < n && ROTATE_STEPS[k] <= cfg.rotateSec) k++;   // next step above the current value
      cfg.rotateSec = ROTATE_STEPS[k % n];
      pageAt = millis();
    } break;
    case 2: cfg.clock24 = !cfg.clock24; break;
    case 3: cfg.nightMode = (cfg.nightMode + 1) % 3; break;
    case 5:
      if ((int32_t)(delArmedUntil - millis()) > 0) { histDeleteReq = true; delArmedUntil = 0; deletedAt = millis(); }
      else delArmedUntil = millis() + 4000;
      return;   // nothing to save
    case 4: {
      int n = sizeof NIGHT_HOURS / sizeof NIGHT_HOURS[0], k = 0;
      while (k < n && !(NIGHT_HOURS[k][0] == cfg.nightStart && NIGHT_HOURS[k][1] == cfg.nightEnd)) k++;
      k = (k + 1) % n;   // a custom pair from the setup page restarts the list
      cfg.nightStart = NIGHT_HOURS[k][0]; cfg.nightEnd = NIGHT_HOURS[k][1];
    } break;
  }
  saveConfig(cfg);
}

static void tap(int x, int y) {
  if (y >= 240) return;   // the Core2's touch-button strip is handled as BtnA/B/C
  bool backHit = x < 40 && y < 38;
  switch (screen) {
    case HOME:
      if (y < 38 && x < 44) screen = MENU;
      else if (y < 38 && x < 96) { alarmsFrom = HOME; screen = ALARMS; }
      else if (y < 38 && x > 130 && x < 190 && doserCount) { homePage ^= 1; pageAt = millis(); }   // page dots
      else if (homePage == 1) { homePage = 0; pageAt = millis(); }                                    // tap the doser page → tiles
      else if (y >= 142) homeRange = (homeRange + 1) % NRANGE;                                       // graph: 1 h / 6 h / 24 h
      else for (int t = 0; t < 2; t++)
        if (x >= TILE_X[t] && x < TILE_X[t] + TW && y >= TILE_Y[t] && y < TILE_Y[t] + TH) { detailTile = t; screen = DETAIL; }
      break;
    case DETAIL:
      if (backHit) screen = HOME;
      else if (y >= 42 && y < 74) detailRange = constrain((x - 8) / 50, 0, NRANGE - 1);
      break;
    case MENU:
      if (backHit) screen = HOME;
      else if (y >= 44 && y < 44 + MENU_N * MENU_H) { const Screen to[] = {ALARMS, SETTINGS, SETTINGS2, NETWORK, ABOUT}; screen = to[(y - 44) / MENU_H]; alarmsFrom = MENU; }
      break;
    case SETTINGS:
      if (backHit) screen = MENU;
      else if (y >= 44 && y < 44 + SET_ROWS * SET_ROW_H) applySetting((y - 44) / SET_ROW_H);
      break;
    case NETWORK:
      if (backHit) screen = MENU;
      else if (y >= 198 && !apActive) apRequest = true;
      break;
    case SETTINGS2:
      if (backHit) screen = MENU;
      else if (y >= 44 && y < 44 + SET2_ROWS * SET2_ROW_H) applySetting2((y - 44) / SET2_ROW_H);
      break;
    case ALARMS: case ABOUT:
      if (backHit) screen = (screen == ALARMS ? alarmsFrom : MENU);
      break;
    case SETUP: break;
  }
}

// ---------- glow ----------
static void updateGlow() {
  static uint32_t lastColor = 0xFFFFFFFF;
  int pin = glowGpio(sc.glowPort), count = sc.glowCount;
  if (pin != glowPinNow || count != glowCountNow) {    // output changed on the setup page
    if (glowPinNow >= 0) { glow.clear(); glow.show(); }
    glow.setPin(pin); glow.updateLength(count); glow.begin();
    glow.clear(); glow.show();
    glowPinNow = pin; glowCountNow = count; lastColor = 0xFFFFFFFF;
  }
  const uint8_t maxB[] = {40, 110, 255};
  uint8_t m = maxB[sc.glowLevel];
  uint32_t c = 0;
  if (alarmCount && sc.glowAlarm) {
    float ph = (sinf(millis() / 1000.0f * 2 * PI / 1.6f) + 1) / 2;   // 1.6 s breathing pulse
    uint8_t r = (uint8_t)(m * (0.2f + 0.8f * ph));
    c = glow.Color(r, 0, 0);
  } else if (offlineCount && sc.glowOffline) {
    c = glow.Color(m * 2 / 3, m / 4, 0);
  }
  if (c == lastColor) return;
  lastColor = c;
  for (int i = 0; i < glowCountNow; i++) glow.setPixelColor(i, c);
  glow.show();
}

// ==========================================================================================

void setup() {
  mtx = xSemaphoreCreateMutex();
  loadConfig(cfg);   // before M5.begin: the power input decides the M-Bus 5 V direction

  auto mc = M5.config();
  // USB-C: the Core2 drives 5 V out on M-Bus pin 28 (feeds the Jewel).
  // Power from the back: pin 28 is an input; the Core2 must NOT drive it.
  mc.output_power = !cfg.powerFromBack;
  M5.begin(mc);
  M5.Power.setExtOutput(!cfg.powerFromBack);
  M5.Speaker.setVolume(140);

  for (int t = 0; t < NUM_TILES; t++) {   // ~300 KB of history in PSRAM
    fineH[t] = (float*)ps_malloc(FINE_LEN * sizeof(float));
    coarseH[t] = (float*)ps_malloc(COARSE_LEN * sizeof(float));
    for (int k = 0; k < FINE_LEN; k++) fineH[t][k] = NAN;
    for (int k = 0; k < COARSE_LEN; k++) coarseH[t][k] = NAN;
    qSum[t] = 0; qCnt[t] = 0;
  }
  gbuf = (float*)ps_malloc(FINE_LEN * sizeof(float));

  uint64_t efuse = ESP.getEfuseMac();   // bytes LSB-first: last two MAC bytes are 4 and 5
  snprintf(apName, sizeof apName, "GHL-Glance-%02X%02X", (unsigned)((efuse >> 32) & 0xFF), (unsigned)((efuse >> 40) & 0xFF));

  setenv("TZ", cfg.tz, 1); tzset();
  rtcToSystem();


  cv.setColorDepth(16);
  cv.setPsram(true);
  cv.createSprite(320, 240);

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

static uint32_t swallowUntil = 0;   // the touch that wakes the screen at night does nothing else

void loop() {
  M5.update();
  takeSnapshot();
  evaluateAlarms();

  if (alarmCount > prevAlarmCount && sc.beep) {
    M5.Speaker.tone(2800, 120); delay(170); M5.Speaker.tone(2800, 120);
  }
  prevAlarmCount = alarmCount;

  // Night: dim or blank the screen between nightStart and nightEnd. A touch wakes it for a
  // minute (that touch isn't passed to the UI); an alarm brings it to full brightness.
  static uint32_t wakeUntil = 0;
  bool night = false;
  if (sc.nightMode && timeValid() && sc.nightStart != sc.nightEnd) {
    time_t tnow = time(nullptr); tm lt; localtime_r(&tnow, &lt);
    int h = lt.tm_hour;
    night = sc.nightStart < sc.nightEnd ? (h >= sc.nightStart && h < sc.nightEnd)
                                        : (h >= sc.nightStart || h < sc.nightEnd);
  }
  bool awake = (int32_t)(wakeUntil - millis()) > 0;
  bool resting = night && !awake && alarmCount == 0;

  static int lastBright = -1;
  const uint8_t bright[] = {50, 110, 180, 255};
  int want = resting ? (sc.nightMode == 1 ? 12 : 0) : bright[sc.brightness];
  if (want != lastBright) { lastBright = want; M5.Display.setBrightness(want); }
  if (resting && screen != HOME && screen != SETUP) screen = HOME;   // fall back to Home overnight

  bool setupOnly = apActive && !staUp && !sc.ssid[0];
  if (setupOnly) screen = SETUP; else if (screen == SETUP) screen = HOME;

  auto td = M5.Touch.getDetail();
  bool anyPress = td.wasPressed() || M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
  if (resting && anyPress) { wakeUntil = millis() + 60000; touchAt = millis(); swallowUntil = millis() + 400; }
  if (night && awake && anyPress) wakeUntil = millis() + 60000;
  if (td.wasClicked() && (int32_t)(swallowUntil - millis()) <= 0) { touchAt = millis(); tap(td.x, td.y); }

  // Home alternates tiles / doser page every rotateSec; a touch pauses it for a minute
  uint32_t nowMs = millis();
  if (!doserCount) homePage = 0;
  else if (screen == HOME && sc.rotateSec && nowMs - touchAt > 60000 && nowMs - pageAt >= sc.rotateSec * 1000UL) {
    homePage ^= 1; pageAt = nowMs;
  }
  if (M5.BtnA.wasClicked() && (int32_t)(swallowUntil - millis()) <= 0 && screen != HOME && screen != SETUP)
    screen = (screen == DETAIL || screen == MENU) ? HOME : (screen == ALARMS ? alarmsFrom : MENU);

  cv.fillScreen(C(COL_BG));
  switch (screen) {
    case HOME: drawHome(); break;
    case DETAIL: drawDetail(); break;
    case MENU: drawMenu(); break;
    case ALARMS: drawAlarms(); break;
    case SETTINGS: drawSettings(); break;
    case SETTINGS2: drawSettings2(); break;
    case NETWORK: drawNetwork(); break;
    case ABOUT: drawAbout(); break;
    case SETUP: drawSetup(); break;
  }
  cv.pushSprite(0, 0);

  updateGlow();
  delay(30);
}
