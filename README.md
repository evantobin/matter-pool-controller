# Matter Pool Controller

Local-first ESP32-S3 firmware for a pool-equipment controller. It presents a
Pentair IntelliFlo-compatible pump, six relay channels, and optional sensors as
Matter bridged devices for platforms such as Apple Home.

This project has no cloud synchronization, remote logging, telemetry upload, or
over-the-air update client. It communicates only with local hardware and the
Matter fabric that commissions it.

Want a finished Pool Conductor controller instead of building your own? Visit
[poolconductor.com](https://poolconductor.com) for device availability and the
hosted product experience.

## Hardware

The current board target is the Waveshare ESP32-S3 Relay 6CH. Pin assignments
are defined in `main/board_pins.h`:

- RS-485 pump bus: RX GPIO 18, TX GPIO 17
- Relay outputs: GPIO 46, 45, 42, 41, 2, and 1
- Sensor inputs: GPIO 4, 5, 6, and 7
- Boot button: GPIO 0
- Buzzer: GPIO 21
- RGB status LED: GPIO 38

Verify your wiring, relay polarity, and pump protocol before connecting
high-voltage equipment. This firmware starts with every relay off.

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

Edit `main/board_identity.h` for each physical controller. In particular,
change `BOARD_DEVICE_ID`, the Matter setup PIN, and discriminator. The included
values are development defaults and must not be reused across devices exposed to
untrusted people or networks.

The Matter vendor and product IDs in `main/matter_setup.cpp` are development
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
main/                 Application source and Matter endpoint setup
main/board_pins.h     Board-specific GPIO assignments
main/board_identity.h Per-device Matter identity defaults
partitions.csv        Single-app, non-OTA flash layout
sdkconfig.defaults    ESP-IDF project defaults
```

## Security Notes

Do not commit certificates, private keys, generated `sdkconfig`, or build
outputs. The repository `.gitignore` excludes those files along with local
component caches. Keep the controller on a trusted network and use the physical
factory-reset action to remove an existing Matter fabric before transferring it.
