#include "doctest.h"
#include <cstdint>
#include <cstddef>
#include <cstring>

// ---------- Replicated from pentair_pump.h (no ESP-IDF deps) ----------

struct PentairPumpStatus {
  bool valid = false;
  uint8_t command = 0;
  uint8_t mode = 0;
  uint8_t driveState = 0;
  uint16_t watts = 0;
  uint16_t rpm = 0;
  uint8_t flow = 0;
  uint8_t ppc = 0;
  uint8_t reserved = 0;
  uint8_t pumpError = 0;
  uint16_t statusWord = 0;
  uint8_t clockHour = 0;
  uint8_t clockMinute = 0;
};

static constexpr size_t MAX_PACKET = 96;

// ---------- Replicated from pentair_pump.cpp ----------

static uint16_t checksum(const uint8_t *header, size_t headerLength,
                         const uint8_t *payload, size_t payloadLength) {
  uint16_t sum = 0;
  for (size_t i = 0; i < headerLength; i++) sum += header[i];
  for (size_t i = 0; i < payloadLength; i++) sum += payload[i];
  return sum;
}

static bool parseFrame(const uint8_t *frame, size_t length,
                       PentairPumpStatus &status,
                       uint8_t pumpAddress = 96, uint8_t controllerAddress = 16) {
  if (length < 11) return false;
  if (frame[0] != 255 || frame[1] != 0 || frame[2] != 255) return false;
  if (frame[3] != 165) return false;

  const uint8_t *header = frame + 3;
  uint8_t dest = header[2];
  uint8_t source = header[3];
  uint8_t action = header[4];
  uint8_t payloadLength = header[5];
  const uint8_t *payload = frame + 9;

  if (length != 3 + 6 + payloadLength + 2) return false;

  uint16_t expected = checksum(header, 6, payload, payloadLength);
  uint16_t actual = ((uint16_t)frame[length - 2] << 8) | frame[length - 1];
  if (expected != actual) return false;

  if (source != pumpAddress) return false;
  if (dest != controllerAddress && dest != 16) return false;
  if (action != 7 || payloadLength < 15) return false;

  status.valid = true;
  status.command = payload[0];
  status.mode = payload[1];
  status.driveState = payload[2];
  status.watts = ((uint16_t)payload[3] << 8) | payload[4];
  status.rpm = ((uint16_t)payload[5] << 8) | payload[6];
  status.flow = payload[7];
  status.ppc = payload[8];
  status.reserved = payload[9];
  status.pumpError = payload[10];
  status.statusWord = ((uint16_t)payload[11] << 8) | payload[12];
  status.clockHour = payload[13];
  status.clockMinute = payload[14];
  return true;
}

// Build a sendMessage-style packet (without sendBytes).
// Returns packet length, or 0 on overflow.
static size_t buildTxPacket(uint8_t *packet, size_t capacity,
                            uint8_t pumpAddress, uint8_t controllerAddress,
                            uint8_t action, const uint8_t *payload, uint8_t payloadLength) {
  if (capacity < 3 + 6 + payloadLength + 2) return 0;

  const uint8_t header[] = {165, 0, pumpAddress, controllerAddress, action, payloadLength};

  size_t pos = 0;
  packet[pos++] = 255;
  packet[pos++] = 0;
  packet[pos++] = 255;

  for (uint8_t b : header) packet[pos++] = b;
  for (uint8_t i = 0; i < payloadLength; i++) packet[pos++] = payload[i];

  uint16_t sum = checksum(header, sizeof(header), payload, payloadLength);
  packet[pos++] = (uint8_t)((sum >> 8) & 0xff);
  packet[pos++] = (uint8_t)(sum & 0xff);

  return pos;
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("checksum") {
  // Empty
  CHECK(checksum(nullptr, 0, nullptr, 0) == 0);

  // Single buffer
  const uint8_t hdr[] = {165, 0, 96, 16, 7, 15};
  uint16_t hdrSum = 0;
  for (uint8_t b : hdr) hdrSum += b;
  CHECK(checksum(hdr, sizeof(hdr), nullptr, 0) == hdrSum);

  // Header + payload
  const uint8_t payload[] = {0, 2, 2, 0, 30, 5, 160, 0, 0, 0, 0, 0, 0, 0, 0};
  uint16_t totalSum = hdrSum;
  for (uint8_t b : payload) totalSum += b;
  CHECK(checksum(hdr, sizeof(hdr), payload, sizeof(payload)) == totalSum);
}

TEST_CASE("parseFrame") {
  PentairPumpStatus s;

  // Too short
  uint8_t tooShort[10] = {};
  CHECK(!parseFrame(tooShort, 10, s));

  // Bad preamble
  uint8_t badPreamble[] = {0, 0, 0, 165, 0, 96, 16, 7, 15,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  CHECK(!parseFrame(badPreamble, sizeof(badPreamble), s));

  // Bad destination byte (not 165)
  uint8_t badDest[] = {255,0,255, 0, 0,96,16,7,15,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  CHECK(!parseFrame(badDest, sizeof(badDest), s));

  // Wrong length
  uint8_t wrongLen[] = {255,0,255,165,0,96,16,7,99,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  CHECK(!parseFrame(wrongLen, sizeof(wrongLen), s));

  // Bad checksum
  uint8_t badChecksum[] = {255,0,255,165,0,96,16,7,15,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  CHECK(!parseFrame(badChecksum, sizeof(badChecksum), s));

  // Valid status frame
  uint8_t valid[] = {255,0,255,165,0,16,96,7,15,
    10,        // command
    2,         // mode
    2,         // driveState
    0,30,      // watts = 30
    5,160,     // rpm = 1440
    8,         // flow
    3,         // ppc
    9,         // reserved
    0,         // pumpError
    0,1,       // statusWord = 1
    14,        // clockHour
    30,        // clockMinute
    2,61};     // checksum = 573
  CHECK(parseFrame(valid, sizeof(valid), s));
  CHECK(s.valid);
  CHECK(s.command == 10);
  CHECK(s.mode == 2);
  CHECK(s.driveState == 2);
  CHECK(s.watts == 30);
  CHECK(s.rpm == 1440);
  CHECK(s.flow == 8);
  CHECK(s.ppc == 3);
  CHECK(s.reserved == 9);
  CHECK(s.pumpError == 0);
  CHECK(s.statusWord == 1);
  CHECK(s.clockHour == 14);
  CHECK(s.clockMinute == 30);

  // Wrong source address → rejected
  uint8_t wrongSrc[] = {255,0,255,165,0,0,16,7,15,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  uint16_t ws = checksum(wrongSrc+3, 6, wrongSrc+9, 15);
  wrongSrc[sizeof(wrongSrc)-2] = (ws >> 8) & 0xff;
  wrongSrc[sizeof(wrongSrc)-1] = ws & 0xff;
  CHECK(!parseFrame(wrongSrc, sizeof(wrongSrc), s));

  // Wrong action (not 7) → rejected
  uint8_t wrongAction[] = {255,0,255,165,0,96,16,99,15,
    0,2,2,0,30,5,160,0,0,0,0,0,0,0,0, 0,0};
  uint16_t wa = checksum(wrongAction+3, 6, wrongAction+9, 15);
  wrongAction[sizeof(wrongAction)-2] = (wa >> 8) & 0xff;
  wrongAction[sizeof(wrongAction)-1] = wa & 0xff;
  CHECK(!parseFrame(wrongAction, sizeof(wrongAction), s));
}

TEST_CASE("buildTxPacket — setPower") {
  uint8_t buf[MAX_PACKET];
  uint8_t payload[] = {10};  // ON
  size_t len = buildTxPacket(buf, sizeof(buf), 96, 16, 6, payload, 1);
  CHECK(len == 12);  // 3 preamble + 6 header + 1 payload + 2 checksum
  CHECK(buf[0] == 255); CHECK(buf[1] == 0); CHECK(buf[2] == 255);
  CHECK(buf[3] == 165); CHECK(buf[4] == 0);
  CHECK(buf[5] == 96);  // pumpAddress
  CHECK(buf[6] == 16);  // controllerAddress
  CHECK(buf[7] == 6);   // action (setPower)
  CHECK(buf[8] == 1);   // payload length
  CHECK(buf[9] == 10);  // payload: ON
  // checksum verified by roundtrip
  uint16_t sum = checksum(buf+3, 6, buf+9, 1);
  CHECK(((uint16_t)buf[10] << 8 | buf[11]) == sum);
}

TEST_CASE("buildTxPacket — setRpm") {
  uint8_t buf[MAX_PACKET];
  uint8_t payload[] = {2, 196, 0x0B, 0xB8};  // 3000 RPM
  size_t len = buildTxPacket(buf, sizeof(buf), 96, 16, 10, payload, 4);
  CHECK(len == 15);
  CHECK(buf[7] == 10);  // action (setRpm)
  CHECK(buf[8] == 4);   // payload length
  CHECK(buf[9] == 2); CHECK(buf[10] == 196);
  CHECK(buf[11] == 0x0B); CHECK(buf[12] == 0xB8);
  uint16_t sum = checksum(buf+3, 6, buf+9, 4);
  CHECK(((uint16_t)buf[13] << 8 | buf[14]) == sum);
}

TEST_CASE("buildTxPacket — setGpm") {
  uint8_t buf[MAX_PACKET];
  uint8_t payload[] = {2, 196, 0, 40};  // 40 GPM
  size_t len = buildTxPacket(buf, sizeof(buf), 96, 16, 9, payload, 4);
  CHECK(len == 15);
  CHECK(buf[7] == 9);   // action (setGpm)
  CHECK(buf[8] == 4);
  CHECK(buf[9] == 2); CHECK(buf[10] == 196);
  CHECK(buf[11] == 0); CHECK(buf[12] == 40);
  uint16_t sum = checksum(buf+3, 6, buf+9, 4);
  CHECK(((uint16_t)buf[13] << 8 | buf[14]) == sum);
}

TEST_CASE("buildTxPacket — requestStatus") {
  uint8_t buf[MAX_PACKET];
  size_t len = buildTxPacket(buf, sizeof(buf), 96, 16, 7, nullptr, 0);
  CHECK(len == 11);  // 3 + 6 + 0 + 2
  CHECK(buf[7] == 7);   // action (requestStatus)
  CHECK(buf[8] == 0);   // payload length
  uint16_t sum = checksum(buf+3, 6, nullptr, 0);
  CHECK(((uint16_t)buf[9] << 8 | buf[10]) == sum);
}

TEST_CASE("buildTxPacket — overflow") {
  uint8_t buf[10];  // Too small for any packet
  size_t len = buildTxPacket(buf, sizeof(buf), 96, 16, 7, nullptr, 0);
  CHECK(len == 0);
}
