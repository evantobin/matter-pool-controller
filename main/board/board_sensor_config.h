#pragma once

#include <stdint.h>

#include "board/board_pins.h"

// Per-controller sensor setup. Leave a port Disabled when no sensor is wired.
// Configure at most one port for each sensor type: the Matter bridge exposes
// one water-level contact, one flow contact, and one temperature reading.
namespace BoardSensorConfig {

enum class Type : uint8_t {
  Disabled,
  WaterLevelSwitch,
  FlowSwitch,
  TemperatureDs18b20,
};

struct Config {
  Type type;
  const char *name;
  bool flowLockoutEnabled;
};

// Example: {Type::FlowSwitch, "Pump Flow", true}
// A flow lockout stops relay actions when this dry-contact flow switch is open.
static constexpr Config Sensors[BoardPins::SensorCount] = {
  {Type::Disabled, "Water Level", false},
  {Type::Disabled, "Water Flow", false},
  {Type::Disabled, "Water Temperature", false},
  {Type::Disabled, "Sensor 4", false},
};

} // namespace BoardSensorConfig
