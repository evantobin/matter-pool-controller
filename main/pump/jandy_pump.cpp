#include "jandy_pump.h"
#include <cstring>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "jandy";

static uart_port_t sUartNum = UART_NUM_2;
static int sDirectionPin = -1;
static uint8_t sPumpId = JANDY_PUMP_ID_MIN;
static constexpr size_t MAX_PACKET = 32;
static uint8_t sRxBuffer[MAX_PACKET];
static size_t sRxLen = 0;
static bool sDirPinHigh = false;

static void setDirection(bool transmit) {
  if (sDirectionPin < 0) return;
  if (transmit && !sDirPinHigh) {
    gpio_set_level((gpio_num_t)sDirectionPin, 1);
    esp_rom_delay_us(50);
    sDirPinHigh = true;
  } else if (!transmit && sDirPinHigh) {
    esp_rom_delay_us(50);
    gpio_set_level((gpio_num_t)sDirectionPin, 0);
    sDirPinHigh = false;
  }
}

static uint16_t checksum(const uint8_t *data, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return sum;
}

static void sendPacket(uint8_t cmd, uint8_t subCmd, const uint8_t *payload, uint8_t payloadLen) {
  uint8_t packet[32];
  packet[JANDY_DLE]   = JANDY_PKT_START;
  packet[JANDY_STX]   = 0x02;
  packet[JANDY_DEST]  = sPumpId;
  packet[JANDY_CMD]   = cmd;
  packet[JANDY_SUBCMD] = 0;
  packet[JANDY_LEN]   = payloadLen;

  size_t pos = JANDY_DATA;
  for (uint8_t i = 0; i < payloadLen; i++) {
    packet[pos++] = payload[i];
  }

  uint16_t csum = checksum(packet + JANDY_DLE, pos - JANDY_DLE);
  packet[pos++] = (uint8_t)((csum >> 8) & 0xFF);
  packet[pos++] = (uint8_t)(csum & 0xFF);
  packet[pos++] = JANDY_PKT_START;
  packet[pos++] = 0x03;

  setDirection(true);
  uart_write_bytes(sUartNum, (const char *)packet, pos);
  uart_wait_tx_done(sUartNum, pdMS_TO_TICKS(100));
  setDirection(false);
}

void jandyPumpBegin(uart_port_t uartNum, int txPin, int rxPin, uint8_t pumpId) {
  sUartNum = uartNum;
  sPumpId = pumpId;

  gpio_config_t cfg = {
    .pin_bit_mask = (1ULL << txPin) | (1ULL << rxPin),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);

  uart_config_t uartConfig = {
    .baud_rate = 9600,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  uart_param_config(uartNum, &uartConfig);
  uart_set_pin(uartNum, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(uartNum, 256, 0, 0, nullptr, 0);

  sDirectionPin = -1;
  sDirPinHigh = false;

  ESP_LOGI(TAG, "Jandy pump initialized on UART %d, pump ID 0x%02X", uartNum, pumpId);
}

void jandySetRpm(uint16_t rpm) {
  uint8_t rawRpm = (rpm * 4) > 1023 ? 255 : (uint8_t)((rpm * 4) / 39);
  uint8_t payload[] = { rawRpm };
  sendPacket(JANDY_CMD_SET_RPM, 0, payload, sizeof(payload));
}

static bool parseFrame(const uint8_t *frame, size_t length, JandyPumpStatus &status) {
  if (length < 10) return false;
  if (frame[JANDY_DLE] != JANDY_PKT_START || frame[JANDY_STX] != 0x02) return false;

  uint8_t dest = frame[JANDY_DEST];
  uint8_t cmd = frame[JANDY_CMD];
  uint8_t subCmd = frame[JANDY_SUBCMD];
  uint8_t dataLen = frame[JANDY_LEN];

  size_t expectedLen = 6 + dataLen + 2 + 2;
  if (length < expectedLen) return false;

  uint16_t expected = checksum(frame + JANDY_DLE, 6 + dataLen);
  uint16_t actual = ((uint16_t)frame[6 + dataLen] << 8) | frame[6 + dataLen + 1];
  if (expected != actual) return false;

  if (dest != JANDY_MASTER_ID) return false;

  if (cmd == JANDY_CMD_STATUS && subCmd == JANDY_CMD_SET_RPM && dataLen >= 2) {
    status.rpm = ((uint16_t)frame[JANDY_DATA + 1] * 256 + frame[JANDY_DATA]) / 4;
    status.valid = true;
    return true;
  }

  if (cmd == JANDY_CMD_STATUS && subCmd == JANDY_CMD_GET_WATTS && dataLen >= 2) {
    status.watts = (uint16_t)frame[JANDY_DATA + 1] * 256 + frame[JANDY_DATA];
    status.valid = true;
    return true;
  }

  return false;
}

bool jandyReadStatus(JandyPumpStatus &status) {
  size_t avail = 0;
  uart_get_buffered_data_len(sUartNum, &avail);

  while (avail > 0) {
    uint8_t b;
    int read = uart_read_bytes(sUartNum, &b, 1, 0);
    if (read != 1) break;
    avail--;

    if (sRxLen == 0 && b != JANDY_PKT_START) continue;
    if (sRxLen == 1 && b != 0x02) { sRxLen = (b == JANDY_PKT_START) ? 1 : 0; continue; }

    if (sRxLen < MAX_PACKET) sRxBuffer[sRxLen++] = b;
    else { sRxLen = 0; continue; }

    if (sRxLen >= 8) {
      uint8_t dataLen = sRxBuffer[JANDY_LEN];
      size_t frameLen = 6 + dataLen + 2 + 2;
      if (frameLen > MAX_PACKET) { sRxLen = 0; continue; }
      if (sRxLen >= frameLen) {
        bool ok = parseFrame(sRxBuffer, frameLen, status);
        sRxLen = 0;
        if (ok) return true;
      }
    }
  }
  return false;
}

void jandyPoll() {
  JandyPumpStatus dummy;
  jandyReadStatus(dummy);
}
