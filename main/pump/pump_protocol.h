// Pump protocol abstraction layer.
//
// The pool controller supports multiple RS-485 pump protocols (Pentair,
// Jandy, Hayward, etc.) through a common function-table interface. Each
// protocol is implemented in a separate .cpp file and exposes its functions
// via the pumpProtocol singleton.
//
// pumpProtocolInit() is called once at boot to select the protocol type
// and wire up the function table. After init, all callers use the function
// pointers in pumpProtocol — they never need to know which protocol is
// active. This pattern supports compile-time protocol selection (via board
// config) without #ifdef pollution in every pump-using module.
//
// The protocol table is intentionally minimal: begin, setPower, setRpm,
// setGpm, requestStatus, readStatus, and poll. These are the lowest common
// denominator across all supported pump types. Protocol-specific features
// (priming, scheduling, freeze protection) are handled by the higher-level
// control module, not the protocol driver.

#pragma once
#include <cstdint>

// Each pump manufacturer uses a different RS-485 command set. The protocol
// type is selected at compile time via the board config. At runtime, the
// type is used to select the correct function table.
enum class PumpProtocolType : uint8_t {
  Pentair = 0,
  Jandy = 1,
  Hayward = 2,
  Emaux = 3,
  Century = 4,
  Speck = 5,
  Neptune = 6,
};

// Function table for the active pump protocol. All callers use these
// pointers — they never call protocol-specific functions directly.
// Unimplemented functions (e.g., setGpm for protocols that don't support
// native GPM mode) are set to nullptr and callers must check before calling.
struct PumpProtocol {
  PumpProtocolType type;

  // Initialize the RS-485 UART and prepare the protocol driver.
  // txPin and rxPin are the ESP32 GPIO numbers for the RS-485 transceiver.
  // pumpAddress is the RS-485 address of the pump (96 for Pentair).
  void (*begin)(int txPin, int rxPin, uint8_t pumpAddress);

  // Send a power-on or power-off command. The pump may take several seconds
  // to start/stop after this command is sent.
  void (*setPower)(bool on);

  // Send a target RPM command. RPM is 450..3450 for most variable-speed pumps.
  void (*setRpm)(uint16_t rpm);

  // Send a target GPM command. Only supported by pumps with built-in flow
  // metering. For pumps without native GPM, this may be nullptr — callers
  // should fall back to RPM control.
  void (*setGpm)(uint8_t gpm);

  // Request a status frame from the pump. The response arrives asynchronously
  // and is queued internally. Read it with readStatus().
  void (*requestStatus)();

  // Pop one status frame from the internal queue. Returns true if a frame
  // was available and populates the output parameters. Returns false if the
  // queue is empty. Called from the main loop repeatedly until it returns false.
  bool (*readStatus)(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault);

  // Poll the protocol driver for incoming data. Called from the protocol's
  // internal FreeRTOS task (if the protocol uses a task) or from the main loop.
  // Some protocols are entirely poll-driven; others use a background task for
  // the UART RX and only need poll() for timeouts and retries.
  void (*poll)();
};

// Selects the protocol type and initializes the RS-485 UART and driver.
// The dirPin is the RS-485 DE/RE control pin for transceivers that don't
// support auto-direction. Pass -1 if auto-direction is used (most modern
// RS-485 transceivers, including the MAX13487E, handle this automatically).
void pumpProtocolInit(PumpProtocolType type, uint8_t pumpAddress, int txPin, int rxPin, int dirPin);

// The active protocol instance. Populated by pumpProtocolInit(), read by
// pump_control.cpp and the main loop. All function pointers are valid after
// init; optional ones may be nullptr.
extern PumpProtocol pumpProtocol;
