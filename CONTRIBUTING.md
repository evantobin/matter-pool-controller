# Contributing

Thanks for helping improve Pool Conductor.

## Before You Start

- Read the root [README](README.md) and [architecture guide](docs/ARCHITECTURE.md).
- Do not commit `build/`, `sdkconfig`, managed components, certificates, keys,
  or device-specific commissioning values.
- Use an isolated controller or disconnected relay outputs when developing a
  change that can switch pool equipment.

## Development Workflow

1. Start from `main` and make one focused change.
2. Keep hardware-specific values in `main/board/` and preserve module
   boundaries described in `main/README.md`.
3. Build with the ESP-IDF and esp-matter versions listed in the README.
4. Test the affected Matter behavior and, when applicable, verify that relays
   initialize inactive and that the factory-reset path is safe.
5. Open a pull request using the included template.

## Code Style

- Follow the surrounding ESP-IDF C/C++ style and keep changes narrowly scoped.
- Prefer clear names and short comments that explain hardware, protocol, or
  concurrency decisions.
- Do not add cloud dependencies, remote control endpoints, or telemetry upload
  to this public firmware project.

## Pull Requests

Describe the hardware affected, behavior before and after the change, and the
testing performed. Include serial logs only after removing Wi-Fi credentials,
Matter pairing values, and device identifiers.
