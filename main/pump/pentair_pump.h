#pragma once

#include <stdint.h>
#include <stddef.h>

#include <driver/uart.h>

// Raw status fields decoded from the Pentair IntelliFlo-compatible protocol.
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

// Thin UART/RS-485 protocol client. Keep Matter concepts out of this layer.
class PentairPump {
public:
  PentairPump(uart_port_t uartNum, uint8_t pumpAddress, uint8_t controllerAddress, int directionPin = -1);

  void begin(uint32_t baud, int rxPin, int txPin);
  void setDebug(bool enabled);

  void setRemoteControl(bool enabled);
  void setPower(bool enabled);
  void setRpm(uint16_t rpm);
  void setGpm(uint8_t gpm);
  void requestStatus();

  bool readStatus(PentairPumpStatus &status);

private:
  static constexpr size_t MAX_PACKET = 96;

  uart_port_t uartNum;
  const uint8_t pumpAddress;
  const uint8_t controllerAddress;
  const int directionPin;
  bool debugEnabled = false;

  uint8_t rxBuffer[MAX_PACKET];
  size_t rxLen = 0;

  void sendMessage(uint8_t action, const uint8_t *payload, uint8_t length);
  void sendBytes(const uint8_t *bytes, size_t length);
  bool parseFrame(const uint8_t *frame, size_t length, PentairPumpStatus &status);
  static uint16_t checksum(const uint8_t *header, size_t headerLength, const uint8_t *payload, size_t payloadLength);
};
