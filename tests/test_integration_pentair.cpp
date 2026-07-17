#include "doctest.h"
#include "stubs/hal_mock.h"
#include <cstring>

// ============================================================================
// Duplicate PentairPump with mock UART (same bodies as main/pump/pentair_pump.cpp)
// ============================================================================

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

static uint16_t checksum(const uint8_t *header, size_t headerLength,
                         const uint8_t *payload, size_t payloadLength) {
  uint16_t sum = 0;
  for (size_t i = 0; i < headerLength; i++) sum += header[i];
  for (size_t i = 0; i < payloadLength; i++) sum += payload[i];
  return sum;
}

class PentairPump {
public:
  PentairPump(uart_port_t uartNum, uint8_t pumpAddress, uint8_t controllerAddress,
              int directionPin = -1)
    : uartNum(uartNum), pumpAddress(pumpAddress), controllerAddress(controllerAddress),
      directionPin(directionPin) {}

  void begin(uint32_t baud, int rxPin, int txPin) {
    if (directionPin >= 0) {
      gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << directionPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
      };
      gpio_config(&cfg);
      gpio_set_level((gpio_num_t)directionPin, 0);
    }

    uart_config_t uartConfig = {
      .baud_rate = (int)baud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(uartNum, &uartConfig);
    uart_set_pin(uartNum, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uartNum, MAX_PACKET * 2, 0, 0, nullptr, 0);
  }

  void setDebug(bool enabled) { debugEnabled = enabled; }
  void setRemoteControl(bool enabled) {
    uint8_t payload[] = { enabled ? (uint8_t)255 : (uint8_t)0 };
    sendMessage(4, payload, sizeof(payload));
  }
  void setPower(bool enabled) {
    uint8_t payload[] = { enabled ? (uint8_t)10 : (uint8_t)4 };
    sendMessage(6, payload, sizeof(payload));
  }
  void setRpm(uint16_t rpm) {
    uint8_t payload[] = {2, 196, (uint8_t)((rpm >> 8) & 0xff), (uint8_t)(rpm & 0xff)};
    sendMessage(10, payload, sizeof(payload));
  }
  void setGpm(uint8_t gpm) {
    uint8_t payload[] = {2, 196, 0, gpm};
    sendMessage(9, payload, sizeof(payload));
  }
  void requestStatus() {
    sendMessage(7, nullptr, 0);
  }

  bool readStatus(PentairPumpStatus &status) {
    size_t avail = 0;
    uart_get_buffered_data_len(uartNum, &avail);

    while (avail > 0) {
      uint8_t b;
      int read = uart_read_bytes(uartNum, &b, 1, 0);
      if (read != 1) break;
      avail--;

      if (rxLen == 0 && b != 255) continue;
      if (rxLen == 1 && b != 0) { rxLen = (b == 255) ? 1 : 0; continue; }
      if (rxLen == 2 && b != 255) { rxLen = 0; continue; }

      if (rxLen < MAX_PACKET) rxBuffer[rxLen++] = b;
      else { rxLen = 0; continue; }

      if (rxLen >= 9) {
        uint8_t payloadLength = rxBuffer[8];
        size_t frameLength = 3 + 6 + payloadLength + 2;
        if (frameLength > MAX_PACKET) { rxLen = 0; continue; }
        if (rxLen >= frameLength) {
          bool ok = parseFrame(rxBuffer, frameLength, status);
          rxLen = 0;
          if (ok) return true;
        }
      }
    }
    return false;
  }

private:
  uart_port_t uartNum;
  uint8_t pumpAddress;
  uint8_t controllerAddress;
  int directionPin;
  bool debugEnabled = false;
  uint8_t rxBuffer[MAX_PACKET] = {};
  size_t rxLen = 0;

  void sendMessage(uint8_t action, const uint8_t *payload, uint8_t length) {
    uint8_t packet[MAX_PACKET];
    const uint8_t header[] = {165, 0, pumpAddress, controllerAddress, action, length};

    size_t pos = 0;
    packet[pos++] = 255; packet[pos++] = 0; packet[pos++] = 255;
    for (uint8_t b : header) packet[pos++] = b;
    for (uint8_t i = 0; i < length; i++) packet[pos++] = payload[i];

    uint16_t sum = checksum(header, sizeof(header), payload, length);
    packet[pos++] = (uint8_t)((sum >> 8) & 0xff);
    packet[pos++] = (uint8_t)(sum & 0xff);

    sendBytes(packet, pos);
  }

  void sendBytes(const uint8_t *bytes, size_t length) {
    if (directionPin >= 0) {
      gpio_set_level((gpio_num_t)directionPin, 1);
      esp_rom_delay_us(100);
    }
    uart_write_bytes(uartNum, (const char *)bytes, length);
    uart_wait_tx_done(uartNum, pdMS_TO_TICKS(100));
    if (directionPin >= 0) {
      esp_rom_delay_us(100);
      gpio_set_level((gpio_num_t)directionPin, 0);
    }
  }

  bool parseFrame(const uint8_t *frame, size_t length, PentairPumpStatus &status) {
    if (length < 11) return false;
    if (frame[0] != 255 || frame[1] != 0 || frame[2] != 255) return false;
    if (frame[3] != 165) return false;

    const uint8_t *header = frame + 3;
    uint8_t dest = header[2];
    uint8_t source = header[3];
    uint8_t action = header[4];
    uint8_t payloadLength = header[5];
    const uint8_t *data = frame + 9;

    if (length != 3 + 6 + payloadLength + 2) return false;

    uint16_t expected = checksum(header, 6, data, payloadLength);
    uint16_t actual = ((uint16_t)frame[length - 2] << 8) | frame[length - 1];
    if (expected != actual) return false;

    if (source != pumpAddress) return false;
    if (dest != controllerAddress && dest != 16) return false;
    if (action != 7 || payloadLength < 15) return false;

    status.valid = true;
    status.command    = data[0];
    status.mode       = data[1];
    status.driveState = data[2];
    status.watts      = ((uint16_t)data[3] << 8) | data[4];
    status.rpm        = ((uint16_t)data[5] << 8) | data[6];
    status.flow       = data[7];
    status.ppc        = data[8];
    status.reserved   = data[9];
    status.pumpError  = data[10];
    status.statusWord = ((uint16_t)data[11] << 8) | data[12];
    status.clockHour  = data[13];
    status.clockMinute = data[14];
    return true;
  }
};

// ============================================================================
// Helper: build a valid 15-byte status payload with correct checksum
// ============================================================================

static void makeStatusFrame(uint8_t *frame, size_t *len,
                            uint8_t pumpAddr, uint8_t ctrlAddr,
                            uint8_t command, uint8_t driveState,
                            uint16_t rpm, uint16_t watts, uint8_t flow) {
  const uint8_t header[] = {165, 0, ctrlAddr, pumpAddr, 7, 15};
  const uint8_t data[15] = {
    command,        // 0
    2,              // mode
    driveState,     // 2
    (uint8_t)(watts >> 8), (uint8_t)(watts & 0xff),  // 3-4
    (uint8_t)(rpm >> 8), (uint8_t)(rpm & 0xff),      // 5-6
    flow,           // 7
    0,              // ppc
    0,              // reserved
    0,              // pumpError
    0, 0,           // statusWord
    0,              // clockHour
    0               // clockMinute
  };
  uint16_t sum = checksum(header, 6, data, 15);

  size_t pos = 0;
  frame[pos++] = 255; frame[pos++] = 0; frame[pos++] = 255;
  for (uint8_t b : header) frame[pos++] = b;
  for (uint8_t b : data) frame[pos++] = b;
  frame[pos++] = (uint8_t)((sum >> 8) & 0xff);
  frame[pos++] = (uint8_t)(sum & 0xff);
  *len = pos;
}

// ============================================================================
// Integration scenarios
// ============================================================================

static void resetAll() {
  mockGpio.reset();
  mockUart.reset();
}

TEST_CASE("Integration: pump begin → UART configured, direction pin low") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16, 5);  // direction pin = GPIO 5
  pump.begin(9600, 18, 17);

  // Direction pin should be output and LOW (receive mode)
  CHECK(mockGpio.mode[5] == GPIO_MODE_OUTPUT);
  CHECK(mockGpio.getLevel(5) == 0);
}

TEST_CASE("Integration: setRpm → correct packet transmitted") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();  // clear begin's UART install noise

  pump.setRpm(3000);

  // Verify TX buffer contains the setRpm packet
  // Expect: FF 00 FF A5 00 60 10 0A 04 02 C4 0B B8 <checksum_hi> <checksum_lo>
  CHECK(mockUart.txLen == 15);
  CHECK(mockUart.txBuf[0] == 0xFF);
  CHECK(mockUart.txBuf[3] == 0xA5);
  CHECK(mockUart.txBuf[5] == 96);   // pumpAddress
  CHECK(mockUart.txBuf[6] == 16);   // controllerAddress
  CHECK(mockUart.txBuf[7] == 10);   // action (setRpm)
  CHECK(mockUart.txBuf[8] == 4);    // payload length
  CHECK(mockUart.txBuf[9] == 2);    // payload[0]
  CHECK(mockUart.txBuf[10] == 196); // payload[1]
  CHECK(mockUart.txBuf[11] == 0x0B);// RPM high byte (3000 = 0x0BB8)
  CHECK(mockUart.txBuf[12] == 0xB8);// RPM low byte
}

TEST_CASE("Integration: setPower ON → correct packet") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();

  pump.setPower(true);
  CHECK(mockUart.txLen == 12);
  CHECK(mockUart.txBuf[7] == 6);   // action (setPower)
  CHECK(mockUart.txBuf[8] == 1);   // payload length
  CHECK(mockUart.txBuf[9] == 10);  // ON
}

TEST_CASE("Integration: setPower OFF → correct packet") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();

  pump.setPower(false);
  CHECK(mockUart.txLen == 12);
  CHECK(mockUart.txBuf[7] == 6);   // action
  CHECK(mockUart.txBuf[8] == 1);   // payload length
  CHECK(mockUart.txBuf[9] == 4);   // OFF
}

TEST_CASE("Integration: setGpm → correct packet") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();

  pump.setGpm(40);
  CHECK(mockUart.txLen == 15);
  CHECK(mockUart.txBuf[7] == 9);   // action (setGpm)
  CHECK(mockUart.txBuf[8] == 4);   // payload length
  CHECK(mockUart.txBuf[12] == 40); // GPM value
}

TEST_CASE("Integration: requestStatus → empty packet") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();

  pump.requestStatus();
  CHECK(mockUart.txLen == 11);
  CHECK(mockUart.txBuf[7] == 7);   // action (requestStatus)
  CHECK(mockUart.txBuf[8] == 0);   // payload length
}

TEST_CASE("Integration: feed status bytes → readStatus returns parsed data") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);

  // Build a status frame: pump running at 1800 RPM, 500W, 40 GPM flow
  uint8_t frame[64];
  size_t len;
  makeStatusFrame(frame, &len, 96, 16, 10, 2, 1800, 500, 40);

  // Feed it into the mock RX buffer
  mockUart.feedRx(frame, len);

  // Read should succeed
  PentairPumpStatus s;
  bool ok = pump.readStatus(s);
  CHECK(ok);
  CHECK(s.valid);
  CHECK(s.command == 10);
  CHECK(s.driveState == 2);
  CHECK(s.rpm == 1800);
  CHECK(s.watts == 500);
  CHECK(s.flow == 40);
}

TEST_CASE("Integration: noise bytes before valid frame are skipped") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);

  // Prepend garbage
  uint8_t noise[] = {0x00, 0xFF, 0x01, 0x02, 0x00};
  mockUart.feedRx(noise, sizeof(noise));

  // Then a valid frame
  uint8_t frame[64];
  size_t len;
  makeStatusFrame(frame, &len, 96, 16, 10, 2, 1440, 300, 20);
  mockUart.feedRx(frame, len);

  PentairPumpStatus s;
  bool ok = pump.readStatus(s);
  CHECK(ok);
  CHECK(s.valid);
  CHECK(s.rpm == 1440);
}

TEST_CASE("Integration: frame from wrong pump address is rejected") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);

  // Frame from pump address 32 (not 96)
  uint8_t frame[64];
  size_t len;
  makeStatusFrame(frame, &len, 32, 16, 10, 2, 1440, 300, 20);
  mockUart.feedRx(frame, len);

  PentairPumpStatus s;
  CHECK(!pump.readStatus(s));
}

TEST_CASE("Integration: setRemoteControl → correct packet") {
  resetAll();
  PentairPump pump(UART_NUM_1, 96, 16);
  pump.begin(9600, 18, 17);
  mockUart.reset();

  pump.setRemoteControl(true);
  CHECK(mockUart.txLen == 12);
  CHECK(mockUart.txBuf[7] == 4);    // action (setRemoteControl)
  CHECK(mockUart.txBuf[9] == 255);  // enabled
}
