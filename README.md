# Matter Pool Controller

An open-source, local-only ESP32-S3 Matter bridge for pool equipment. It
controls a Pentair IntelliFlo-compatible pump, six relay channels, and optional
sensors through platforms such as Apple Home.

This repository has no cloud synchronization, telemetry upload, remote logging,
or over-the-air update client. Commands stay between the controller and the
Matter fabric that commissions it.

## Hardware

The current board target is the
[Waveshare ESP32-S3 Relay 6CH](https://www.waveshare.com/product/esp32-s3-relay-6ch.htm).
It combines the ESP32-S3 microcontroller, six relays, and an RS-485 interface.
Pin assignments are defined in `main/board/board_pins.h`:

- RS-485 pump bus: RX GPIO 18, TX GPIO 17
- Relay outputs: GPIO 46, 45, 42, 41, 2, and 1
- Sensor inputs: GPIO 4, 5, 6, and 7
- Boot button: GPIO 0
- Buzzer: GPIO 21
- RGB status LED: GPIO 38

Use the onboard relay channels only for 24 V or lower control circuits. For a
saltwater chlorine generator, pump, heater, or any other mains-voltage load,
use a properly rated external contactor and have its coil controlled by the
controller relay. Verify wiring, relay polarity, and pump protocol before
energizing equipment. This firmware starts with every relay off.

### Hardware References

- **Controller:** [Waveshare ESP32-S3 Relay 6CH](https://www.waveshare.com/product/esp32-s3-relay-6ch.htm)
  is the board this project targets. Its onboard RS-485 terminal connects to
  the pump bus.
- **Temperature probe:** Search Amazon for `waterproof DS18B20 temperature
  sensor`. Confirm it uses the DS18B20 one-wire sensor against the
  [manufacturer specification](https://www.analog.com/en/products/ds18b20.html).
- **Water-level switch:** Search Amazon for `vertical float switch 24V dry
  contact`. It must be a low-voltage dry-contact switch; compare its listing to
  the [Flowline Switch-Tek LV10 specification](https://www.flowline.com/product/switch-tek-lv10-vertical-buoyancy-liquid-level-switch/).
- **Flow switch:** Search Amazon for `inline water flow switch 24V dry
  contact`. Select a model rated for the plumbing, pressure, and fluid in your
  installation; use the [Gems FS-550 specification](https://www.gemssensors.com/products/FS-550/30640)
  as a reference for this type of switch.

The sensor inputs are for low-voltage sensors only. Use equipment appropriate
for pool installations and have mains-voltage work, including contactor
installation, completed by a qualified electrician.

## Matter Devices

The controller appears as a Matter bridge with these bridged devices:

- One dimmable plug-in unit for pump speed, expressed as a percentage of the
  configured RPM range.
- Six on/off plug-in units for relay channels.
- Contact sensors for water level and flow.
- A temperature sensor for an optional DS18B20.

Relay channels are enabled locally by default. Sensor types are disabled until
you configure them in source for the hardware connected to each input.

## Before Flashing

Edit `main/board/board_identity.h` for each physical controller. In particular,
change `BOARD_DEVICE_ID`, the Matter setup PIN, and discriminator. The included
values are development defaults and must not be reused across devices exposed to
untrusted people or networks.

The Matter vendor and product IDs in `main/matter/matter_setup.cpp` are development
placeholders. Obtain and use assigned IDs before distributing a commercial
product.

## Build

This project targets ESP-IDF 6.0.2 with esp-matter installed separately. Set up
ESP-IDF and esp-matter, then source the included environment helper:

```sh
. ./env.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

`env.sh` assumes ESP-IDF at `~/.espressif/v6.0.2/esp-idf` and esp-matter at
`~/esp/esp-matter`; adjust it for your installation. ESP-IDF resolves the sole
third-party component declared in `main/idf_component.yml` during configuration.

## Serial Console

At the serial prompt:

- `help` lists commands.
- `matter-info` prints commissioning information.
- `reset` reboots without clearing configuration.
- `factory-reset` clears Matter state and reboots.

## Project Layout

```text
main/app/             Startup order and shared runtime state
main/board/           Board pins and per-device Matter identity defaults
main/console/         UART recovery and commissioning commands
main/io/              Relays, sensors, LED/buzzer, and reset button
main/matter/          Matter bridge and endpoint setup
main/platform/        ESP-IDF compatibility workarounds
main/pump/            Pentair protocol and pump control
main/README.md        Module ownership and dependency direction
docs/ARCHITECTURE.md  Runtime flow, boundaries, and safety model
partitions.csv        Single-app, non-OTA flash layout
sdkconfig.defaults    ESP-IDF project defaults
```

## Security Notes

Do not commit certificates, private keys, generated `sdkconfig`, or build
outputs. The repository `.gitignore` excludes those files along with local
component caches. Keep the controller on a trusted network and use the physical
factory-reset action to remove an existing Matter fabric before transferring it.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and pull-request guidance,
[SECURITY.md](SECURITY.md) for responsible vulnerability reporting, and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for module boundaries.

## Support Development

This repository remains the complete local-only controller firmware. For a
ready-to-install controller with a hosted dashboard, historical equipment data,
remote firmware updates, and on-demand diagnostics, see
[Pool Conductor](https://poolconductor.com). Choosing the finished controller
helps fund continued work on the open-source firmware.
