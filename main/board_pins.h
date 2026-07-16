#pragma once

#include <stdint.h>

namespace BoardPins {

static constexpr int RS485_RX = 18;
static constexpr int RS485_TX = 17;

static constexpr int RelayCount = 6;
static constexpr int RelayPins[RelayCount] = {46, 45, 42, 41, 2, 1};

static constexpr int SensorCount = 4;
static constexpr int SensorPins[SensorCount] = {4, 5, 6, 7};

static constexpr int BootButton = 0;
static constexpr int Buzzer = 21;
static constexpr int RgbLed = 38;

} // namespace BoardPins
