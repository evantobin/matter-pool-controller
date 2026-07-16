#pragma once

#include <stdint.h>

// High-level local pump control surface used by Matter and the main loop.
enum class PumpControlMode : uint8_t {
  Rpm = 0,
  Gpm = 1,
};

void handlePumpChange(uint8_t speedPercent);
void handlePumpPower(bool active);
void handlePumpRpm(uint16_t rpm);
void handlePumpGpm(uint8_t gpm);
PumpControlMode pumpControlMode();
void schedulePumpMatterReport(bool running, uint8_t percent);
bool isPumpMatterReportActive();
void readPumpStatus();
