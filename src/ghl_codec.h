// GHL ProfiLux Wi-Fi protocol codec (Mini, Light Wi-Fi, ProfiLux 4) (header-only, no Arduino dependencies).
//
// Same framing TelegrahamTested Sentinel uses, verified against 696 captured
// requests and 783 responses:
//
//   REQUEST  18 00 00 FE FF | LL LL | token(8) | 01 | n | ids(LE16 × n) | cks | 19
//            LL LL = little-endian, frame length minus 9
//            cks   = sum of all bytes before it, mod 256
//
//   RESPONSE token(8) | 03 | n | n × ( id LE16 | len | value (len bytes, LE, signed) )
//            bare: no header, checksum or trailer
//
//   token = last four MAC bytes reversed + serial number as LE uint32,
//           both from GET /api_wifi/v1/status → ConnectedDevice
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace ghl {

// Parses "02:00:00:12:34:56" + serial into the 8-byte token. Returns false on a bad MAC.
inline bool deriveToken(const char* mac, uint32_t serial, uint8_t token[8]) {
  uint8_t b[6]; int n = 0;
  const char* p = mac;
  while (*p && n < 6) {
    int v = 0, digits = 0;
    while (*p && *p != ':' && *p != '-') {
      char c = *p++;
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0 || ++digits > 2) return false;
      v = v * 16 + d;
    }
    if (digits == 0) return false;
    b[n++] = (uint8_t)v;
    if (*p) p++;
  }
  if (n != 6 || *p) return false;
  token[0] = b[5]; token[1] = b[4]; token[2] = b[3]; token[3] = b[2];
  for (int i = 0; i < 4; i++) token[4 + i] = (uint8_t)(serial >> (8 * i));
  return true;
}

// Builds a read request. `out` needs 7 + 10 + 2*count + 2 bytes. Returns frame length.
inline size_t buildRead(const uint8_t token[8], const uint16_t* regs, uint8_t count, uint8_t* out) {
  size_t i = 0;
  out[i++] = 0x18; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0xFE; out[i++] = 0xFF;
  uint16_t len = 8 + 2 + 2 * count;
  out[i++] = len & 0xFF; out[i++] = len >> 8;
  memcpy(out + i, token, 8); i += 8;
  out[i++] = 0x01; out[i++] = count;
  for (uint8_t k = 0; k < count; k++) { out[i++] = regs[k] & 0xFF; out[i++] = regs[k] >> 8; }
  uint32_t sum = 0;
  for (size_t k = 0; k < i; k++) sum += out[k];
  out[i++] = (uint8_t)(sum & 0xFF);
  out[i++] = 0x19;
  return i;
}

// Walks a read response. Calls cb(id, value) for each entry. Returns the entry
// count, or -1 if the frame isn't a read response for this token.
template <typename F>
inline int parseRead(const uint8_t* d, size_t n, const uint8_t token[8], F cb) {
  if (n < 10 || memcmp(d, token, 8) != 0 || d[8] != 0x03) return -1;
  int count = d[9], seen = 0;
  size_t i = 10;
  for (int k = 0; k < count; k++) {
    if (i + 3 > n) return -1;
    uint16_t id = d[i] | (d[i + 1] << 8);
    uint8_t len = d[i + 2];
    i += 3;
    if (len == 0 || len > 4 || i + len > n) return -1;
    uint32_t u = 0;
    for (uint8_t b = 0; b < len; b++) u |= (uint32_t)d[i + b] << (8 * b);
    int32_t v;
    if (len == 4) v = (int32_t)u;
    else { uint32_t sign = 1u << (8 * len - 1); v = (int32_t)((u ^ sign) - sign); }
    i += len;
    cb(id, v);
    seen++;
  }
  return seen;
}

// Live registers. The ProfiLux Mini, Light Wi-Fi and ProfiLux 4 share this layout:
// sensor N's live value is at 10000 + 8 (N - 1); 10005 + 8 (N - 1) is the same value in the
// alternate unit (°F for a temperature sensor). Confirmed on a Mini; the P4 and Light follow GHL Connect's code table.
constexpr uint16_t REG_PH = 10000;         // sensor 1 on the Mini, ×100
constexpr uint16_t REG_TEMP_C = 10008;     // sensor 2 on the Mini, ×10
constexpr uint16_t REG_TEMP_F = 10013;     // sensor 2 in °F, ×10
constexpr uint16_t REG_RSSI = 10628;       // Wi-Fi dBm, signed
constexpr uint16_t REG_ALARM = 10700;      // sensor alarm mask: bit N-1 set = sensor N in alarm (Mini: 1 = pH, 2 = temperature)
constexpr uint16_t REG_LEAK = 10701;       // level-control alarm mask (leak / level sensors)

}  // namespace ghl

// ---------------------------------------------------------------------------
// GHL Master Doser 2.1 (Ralink Wi-Fi module, raw TCP passthrough on port 10001)
// Read-only.
//
// One TCP connection per pump read: send the fixed 13-byte request, the doser
// echoes the 4-letter field name followed by the remaining volume in mL as a
// little-endian run of ASCII nibbles (0x30..0x3F), ended by a byte outside that
// range. Variable width: 4 nibbles up to 65,535 mL, 5+ for large containers.
// e.g. 1000 mL = 0x3E8 → nibbles 8,E,3,0 → "8>30".
namespace doser {

constexpr uint16_t PORT = 10001;
constexpr int PUMPS = 6;
constexpr int REQ_LEN = 13;

struct Pump { const char* field; uint8_t req[REQ_LEN]; };

// Request frames captured from a Master Doser 2.1, one per pump (tested on one unit)
static const Pump PUMP[PUMPS] = {
  {"GDHB", {0x01,0x50,0x70,0xC1,0x02,0x47,0x44,0x48,0x42,0x05,0x03,0xA1,0x04}},
  {"MDHB", {0x01,0x50,0x70,0xC1,0x02,0x4D,0x44,0x48,0x42,0x05,0x03,0xA7,0x04}},
  {"CEHB", {0x01,0x50,0x70,0xC1,0x02,0x43,0x45,0x48,0x42,0x05,0x03,0x9E,0x04}},
  {"IEHB", {0x01,0x50,0x70,0xC1,0x02,0x49,0x45,0x48,0x42,0x05,0x03,0xA4,0x04}},
  {"OEHB", {0x01,0x50,0x70,0xC1,0x02,0x4F,0x45,0x48,0x42,0x05,0x03,0xAA,0x04}},
  {"EFHB", {0x01,0x50,0x70,0xC1,0x02,0x45,0x46,0x48,0x42,0x05,0x03,0xA1,0x04}},
};

// Looks for the field echo and its nibble run in what has arrived so far.
// Returns 1 with *ml set when complete, 0 when more bytes are needed, -1 if malformed.
inline int decode(const uint8_t* d, size_t n, const char* field, int32_t* ml) {
  size_t i = 0;
  bool found = false;
  for (; i + 4 <= n; i++) if (memcmp(d + i, field, 4) == 0) { found = true; break; }
  if (!found) return 0;
  i += 4;
  int nib = 0; uint32_t v = 0;
  while (i < n && nib < 8) {
    int x = (int)d[i] - 0x30;
    if (x < 0 || x > 0x0F) break;
    v |= (uint32_t)x << (4 * nib);
    nib++; i++;
  }
  bool terminated = i < n && nib < 8;
  if (!terminated && nib < 8) return 0;   // value may continue in the next TCP read
  if (nib == 0) return -1;
  if (v > 3000000u) return -1;            // GHL large-container maximum is 3,000 L
  *ml = (int32_t)v;
  return 1;
}

}  // namespace doser
