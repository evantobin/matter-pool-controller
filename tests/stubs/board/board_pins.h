#pragma once

#include <cstdint>

struct BoardPins {
  static constexpr int RelayCount = 6;
  static constexpr int SensorCount = 4;
  static constexpr int RS485_RX = 18;
  static constexpr int RS485_TX = 17;
  static constexpr int BootButton = 0;
};
