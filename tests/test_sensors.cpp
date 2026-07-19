#include "doctest.h"
#include <cstdint>

// ---------- Replicated types ----------

enum class SensorType : uint8_t {
  Disabled, WaterLevelSwitch, FlowSwitch, TemperatureDs18b20
};

// Mimic the BoardSensorConfig enum from board_sensor_config.h
namespace BoardSensorConfig {
  enum class Type : uint8_t {
    Disabled,
    WaterLevelSwitch,
    FlowSwitch,
    TemperatureDs18b20,
  };
}

// ---------- Replicated from sensors.cpp ----------

static SensorType toSensorType(BoardSensorConfig::Type type) {
  switch (type) {
    case BoardSensorConfig::Type::WaterLevelSwitch:
      return SensorType::WaterLevelSwitch;
    case BoardSensorConfig::Type::FlowSwitch:
      return SensorType::FlowSwitch;
    case BoardSensorConfig::Type::TemperatureDs18b20:
      return SensorType::TemperatureDs18b20;
    case BoardSensorConfig::Type::Disabled:
    default:
      return SensorType::Disabled;
  }
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("toSensorType — explicit mappings") {
  CHECK(toSensorType(BoardSensorConfig::Type::Disabled) == SensorType::Disabled);
  CHECK(toSensorType(BoardSensorConfig::Type::WaterLevelSwitch) == SensorType::WaterLevelSwitch);
  CHECK(toSensorType(BoardSensorConfig::Type::FlowSwitch) == SensorType::FlowSwitch);
  CHECK(toSensorType(BoardSensorConfig::Type::TemperatureDs18b20) == SensorType::TemperatureDs18b20);
}

TEST_CASE("toSensorType — unknown value falls back to Disabled") {
  CHECK(toSensorType(static_cast<BoardSensorConfig::Type>(99)) == SensorType::Disabled);
}
