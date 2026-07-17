#pragma once
#include <cstdint>
#include "driver/uart.h"

// Jandy RS-485 packet framing
#define JANDY_PKT_START 0x10
#define JANDY_PKT_END   0x10

// Frame position indices (0-based)
#define JANDY_DLE   0  // 0x10
#define JANDY_STX   1  // 0x02
#define JANDY_DEST  2
#define JANDY_CMD   3
#define JANDY_SUBCMD 4
#define JANDY_LEN   5
#define JANDY_DATA  6

// Command types
#define JANDY_CMD_STATUS   0x1F  // ePump status query/response
#define JANDY_CMD_SET_RPM  0x44  // ePump set/read RPM
#define JANDY_CMD_GET_WATTS 0x45 // ePump get watts

// Default Jandy pump address (master controller ID = 0x00)
#define JANDY_PUMP_ID_MIN  0x60
#define JANDY_PUMP_ID_MAX  0x78
#define JANDY_MASTER_ID    0x00

// Jandy pump status structure
struct JandyPumpStatus {
  bool valid = false;
  uint16_t rpm = 0;
  uint16_t watts = 0;
};

// Initialize UART for Jandy RS-485 communication
void jandyPumpBegin(uart_port_t uartNum, int txPin, int rxPin,
                    uint8_t pumpId = JANDY_PUMP_ID_MIN);

// Set pump RPM (0 = stop, 450-3450 RPM)
void jandySetRpm(uint16_t rpm);

// Query pump status (reads any available status frames)
bool jandyReadStatus(JandyPumpStatus &status);

// Poll UART for incoming data (call frequently)
void jandyPoll();
