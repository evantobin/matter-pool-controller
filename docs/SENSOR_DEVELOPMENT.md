# Adding a Sensor Type

The included float switch, flow switch, and DS18B20 are just the starting
point. If you have another sensor that makes sense for your pool, add it here
instead of trying to force it into one of the existing types.

## The Path Through the Firmware

1. Add the new value to `SensorType` in `main/app/state.h`.
2. Give it a choice in `main/board/board_sensor_config.h`. This is where a
   builder selects the sensor for a physical GPIO and gives it a name.
3. Add the board-config type to `toSensorType()` in `main/io/sensors.cpp`.
4. Read the hardware in `sampleSensors()` or a focused driver module under
   `main/io/`. Keep the reading code out of the Matter setup file.
5. Add a Matter endpoint or attribute update in `main/matter/matter_setup.cpp`
   that matches what the sensor reports.
6. Build, flash, and check that the value changes in your Matter controller.

## Start With the Right Signal

- A dry-contact sensor can use the existing debounced GPIO input pattern in
  `main/io/sensors.cpp`.
- A one-wire sensor can follow the DS18B20 path already in that file.
- I2C, SPI, analog, or serial sensors usually deserve a small driver in
  `main/io/` with a narrow API. Call that driver from the sampling loop.

Keep the GPIO choice in `main/board/board_pins.h` and the per-controller sensor
choice in `main/board/board_sensor_config.h`. That keeps the driver reusable
when someone wires the same sensor to a different input.

## Example: Adding pH

The [DFRobot Gravity industrial analog pH meter](https://www.dfrobot.com/product-1110.html)
is a good example of a sensor that does not fit the included switch or DS18B20
patterns. It is an analog pH kit, so give it an ADC-capable input and write a
small driver that reads the voltage, applies your calibration, and turns that
into a pH number.

The basic change looks like this:

1. Add `Ph` to `SensorType` and `BoardSensorConfig::Type`.
2. Assign the pH board to the GPIO you wired in `board_sensor_config.h`.
3. Add the analog read and calibration code under `main/io/`.
4. Sample it from the main loop and report the result through a suitable Matter
   endpoint or attribute.

That same shape works for other analog pool measurements such as ORP, pressure,
or tank level.

## Matter Side

The controller is a Matter bridge, so a sensor needs a Matter device type that
fits its data. The existing examples are useful references:

| Sensor | Matter device created in `main/matter/matter_setup.cpp` |
| --- | --- |
| Water level or flow switch | Contact sensor |
| DS18B20 temperature probe | Temperature sensor |

Create the endpoint when Matter starts, save its endpoint ID in
`main/app/state.cpp`, and update its attribute from the sampling code through
the Matter thread. `main/io/sensors.cpp` already queues these updates so GPIO
work never writes Matter attributes directly.

## Example Shape

For a new dry-contact sensor called `CoverSwitch`, the pieces look like this:

```cpp
// main/app/state.h
enum class SensorType : uint8_t {
  Disabled,
  WaterLevelSwitch,
  FlowSwitch,
  TemperatureDs18b20,
  CoverSwitch,
};

// main/board/board_sensor_config.h
{Type::CoverSwitch, "Pool Cover", false},
```

Then add the matching board config enum, map it in `toSensorType()`, sample the
input, and expose it as another Matter contact sensor. Keep the change focused:
the board config chooses the hardware, `io/` reads it, and `matter/` reports it.

## Before You Open a Pull Request

- Leave every unrelated sensor type working.
- Make sure an unused input stays disabled.
- Verify the sensor comes up with a sensible first value after a reboot.
- Add the new sensor and its wiring notes to the root README.
- Include a short serial log or Matter-controller screenshot showing the new
  value changing.
