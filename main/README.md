# Firmware Modules

`main/` is the ESP-IDF application component. Files are arranged by the layer
they own rather than by when they happened to be created.

| Directory | Responsibility |
| --- | --- |
| `app/` | Startup order, main loop, and shared runtime state. |
| `board/` | Per-controller Matter identity, GPIO assignments, and compile-time sensor setup. |
| `console/` | UART recovery and commissioning commands. |
| `io/` | Relay outputs, physical sensors, status LED/buzzer, and reset button. |
| `matter/` | Matter bridge, endpoints, metadata, and commissioning output. |
| `platform/` | Small ESP-IDF or SDK compatibility workarounds. |
| `pump/` | Pentair RS-485 frames and high-level pump control policy. |

New code should depend toward the hardware: Matter calls the pump and I/O
interfaces, but those lower-level modules should not include Matter setup code.
