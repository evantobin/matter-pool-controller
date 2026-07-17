#pragma once
#include <cstdint>

enum class PumpProtocolType : uint8_t {
  Pentair = 0,
  Jandy = 1,
};

struct PumpProtocol {
  PumpProtocolType type;
  void (*begin)(int txPin, int rxPin, uint8_t pumpAddress);
  void (*setPower)(bool on);
  void (*setRpm)(uint16_t rpm);
  void (*setGpm)(uint8_t gpm);
  void (*requestStatus)();
  bool (*readStatus)(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault);
  void (*poll)();
};

void pumpProtocolInit(PumpProtocolType type, uint8_t pumpAddress, int txPin, int rxPin, int dirPin);
extern PumpProtocol pumpProtocol;
