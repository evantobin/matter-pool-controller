# Architecture

Pool Conductor is a local Matter bridge that runs on an ESP32-S3. It has no
cloud client, remote update mechanism, or required external service.

## Runtime Flow

1. `app/main.cpp` initializes NVS, the network base, hardware I/O, the serial
   console, the Pentair bus, and Matter.
2. `matter/matter_setup.cpp` creates one bridge aggregator and child endpoints
   for the pump, six relays, and the available sensor types.
3. The main loop samples sensors, reads pump status, updates physical feedback,
   handles the boot button, and yields to FreeRTOS.
4. Pump, relay, and sensor modules schedule Matter attribute changes onto the
   Matter thread. They do not mutate Matter attributes directly from I/O work.

## Boundaries

- `board/` is the only location for GPIO and per-controller commissioning
  defaults.
- `pump/pentair_pump.*` owns frame encoding and decoding. Keep product policy,
  targets, and Matter percentage conversion in `pump/pump_control.*`.
- `io/` owns physical signals and factory-reset behavior. Relay outputs are
  initialized inactive before Matter starts.
- `matter/` maps Matter commands to local interfaces. It should not contain
  UART framing or direct GPIO operations.
- `app/state.*` is shared mutable runtime state. Prefer a narrow module API
  before adding another global.

## Adding Hardware

1. Add the GPIO assignment to `board/board_pins.h`.
2. Put the hardware driver in `io/` or a new focused module.
3. Expose a small API to `matter/` rather than importing driver internals.
4. Create or update the corresponding Matter endpoint in
   `matter/matter_setup.cpp`.
5. Document wiring, failure behavior, and any safety interlock in `README.md`.

## Safety Model

The device starts by driving every relay inactive. The factory-reset path turns
relays off before clearing Matter state and rebooting. Changes affecting relay
polarity, pump commands, or flow lockouts require hardware review before merge.
