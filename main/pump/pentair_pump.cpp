#include "pump/pentair_pump.h"

#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "pentair";

// Low-level Pentair RS-485 frame transport. This class knows the wire protocol;
// pump_control.cpp owns policy such as speed targets and keepalives.
PentairPump::PentairPump(uart_port_t uartNum, uint8_t pumpAddress, uint8_t controllerAddress, int directionPin)
  : uartNum(uartNum),
    pumpAddress(pumpAddress),
    controllerAddress(controllerAddress),
    directionPin(directionPin) {
}

void PentairPump::begin(uint32_t baud, int rxPin, int txPin) {
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
  uart_driver_install(uartNum, MAX_PACKET * 2, 0, 0, NULL, 0);
}

void PentairPump::setDebug(bool enabled) {
  debugEnabled = enabled;
}

void PentairPump::setRemoteControl(bool enabled) {
  uint8_t payload[] = { enabled ? (uint8_t)255 : (uint8_t)0 };
  sendMessage(4, payload, sizeof(payload));
}

void PentairPump::setPower(bool enabled) {
  uint8_t payload[] = { enabled ? (uint8_t)10 : (uint8_t)4 };
  sendMessage(6, payload, sizeof(payload));
}

void PentairPump::setRpm(uint16_t rpm) {
  uint8_t payload[] = {
    2,
    196,
    (uint8_t)((rpm >> 8) & 0xff),
    (uint8_t)(rpm & 0xff)
  };
  sendMessage(10, payload, sizeof(payload));
}

void PentairPump::setGpm(uint8_t gpm) {
  uint8_t payload[] = {2, 196, 0, gpm};
  sendMessage(9, payload, sizeof(payload));
}

void PentairPump::requestStatus() {
  sendMessage(7, nullptr, 0);
}

void PentairPump::sendMessage(uint8_t action, const uint8_t *payload, uint8_t length) {
  uint8_t packet[MAX_PACKET];
  const uint8_t header[] = {165, 0, pumpAddress, controllerAddress, action, length};

  size_t pos = 0;
  packet[pos++] = 255;
  packet[pos++] = 0;
  packet[pos++] = 255;

  for (uint8_t b : header) {
    packet[pos++] = b;
  }

  for (uint8_t i = 0; i < length; i++) {
    packet[pos++] = payload[i];
  }

  uint16_t sum = checksum(header, sizeof(header), payload, length);
  packet[pos++] = (uint8_t)((sum >> 8) & 0xff);
  packet[pos++] = (uint8_t)(sum & 0xff);

  sendBytes(packet, pos);
}

void PentairPump::sendBytes(const uint8_t *bytes, size_t length) {
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

  if (debugEnabled) {
    char buf[256] = {0};
    int off = snprintf(buf, sizeof(buf), "TX:");
    for (size_t i = 0; i < length && off < (int)sizeof(buf) - 6; i++) {
      off += snprintf(buf + off, sizeof(buf) - off, " %u", bytes[i]);
    }
    ESP_LOGD(TAG, "%s", buf);
  }
}

bool PentairPump::readStatus(PentairPumpStatus &status) {
  size_t avail = 0;
  uart_get_buffered_data_len(uartNum, &avail);

  while (avail > 0) {
    uint8_t b;
    int read = uart_read_bytes(uartNum, &b, 1, 0);
    if (read != 1) break;
    avail--;

    if (rxLen == 0 && b != 255) continue;
    if (rxLen == 1 && b != 0) {
      rxLen = b == 255 ? 1 : 0;
      continue;
    }
    if (rxLen == 2 && b != 255) {
      rxLen = 0;
      continue;
    }

    if (rxLen < MAX_PACKET) {
      rxBuffer[rxLen++] = b;
    } else {
      rxLen = 0;
      continue;
    }

    if (rxLen >= 9) {
      uint8_t payloadLength = rxBuffer[8];
      size_t frameLength = 3 + 6 + payloadLength + 2;
      if (frameLength > MAX_PACKET) {
        rxLen = 0;
        continue;
      }

      if (rxLen >= frameLength) {
        bool ok = parseFrame(rxBuffer, frameLength, status);
        rxLen = 0;
        if (ok) return true;
      }
    }
  }

  return false;
}

bool PentairPump::parseFrame(const uint8_t *frame, size_t length, PentairPumpStatus &status) {
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
  if (expected != actual) {
    if (debugEnabled) {
      ESP_LOGW(TAG, "Ignoring packet with bad checksum: expected=%u actual=%u", expected, actual);
    }
    return false;
  }

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

uint16_t PentairPump::checksum(const uint8_t *header, size_t headerLength, const uint8_t *payload, size_t payloadLength) {
  uint16_t sum = 0;
  for (size_t i = 0; i < headerLength; i++) {
    sum += header[i];
  }
  for (size_t i = 0; i < payloadLength; i++) {
    sum += payload[i];
  }
  return sum;
}
