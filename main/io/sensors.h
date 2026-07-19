// Sensor input driver with deferred Matter state reporting.
//
// Manages all physical sensor inputs: water level switches, flow switches,
// and DS18B20 temperature sensors. Digital sensors are debounced in software
// (see DebouncedDigitalInput in state.h). Temperature sensors use bit-banged
// OneWire — no external library dependency.
//
// Sampling is called from the main loop at 100 ms intervals. Temperature
// reads are internally gated at a slower rate (30 s) because DS18B20
// conversions are slow and temperature changes slowly in a pool.
//
// Matter attribute updates follow the same deferred pattern as relays:
// changes are queued on the main loop thread and applied on the Matter
// thread via PlatformMgr().ScheduleWork().

#pragma once

// Configures all sensor GPIOs from the board config and populates the
// sensorConfigs array. Called once at boot, before any sampling.
void setupSensors();

// Called from the main loop at ~10 ms. Internally rate-limits: digital
// sensors are sampled at SENSOR_SAMPLE_MS (100 ms), temperature sensors
// at TEMPERATURE_SAMPLE_MS (30 s). Returns immediately if it's not time
// to sample yet.
void sampleSensors();
