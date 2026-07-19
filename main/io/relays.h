// Relay GPIO driver with deferred Matter state reporting.
//
// Relays are the primary outputs of the pool controller — they switch mains
// power to pumps, heaters, salt-water generators, acid pumps, and valves.
// Because relays control line-voltage equipment, safety is the top priority:
//
//   - All relays are driven to inactive at boot before Matter is running.
//   - Flow lockout forces all relays off when a flow switch reports no flow.
//   - Matter state changes are deferred onto the Matter thread to avoid
//     blocking the GPIO write path.
//
// The setRelay() function accepts a "source" parameter used to prevent
// infinite Matter re-reporting loops: when Matter commands a relay change,
// the source is "matter" and the change is NOT re-reported back to Matter.
// All other sources (schedule, cloud, flow lockout) DO trigger a report.

#pragma once

#include <stdint.h>

// Configures all relay GPIOs as outputs and drives them to their inactive
// levels (activeHigh ? LOW : HIGH). Called once at boot, before Matter
// endpoints are created, so relays are safe before any user can connect.
void setupRelaysSafe();

// Forces all relays to their inactive state. Used at boot and during flow
// lockout. The "source" parameter is logged for debugging — knowing which
// subsystem triggered an all-off helps diagnose unexpected shutdowns.
void setAllRelaysOff(const char *source);

// Sets a single relay to the given state. If the state changed, notifies
// the controller state observer and (if source != "matter") schedules a
// Matter attribute report. The active flag means "relay energized," not
// "GPIO high" — the pin level is computed from activeHigh in the config.
void setRelay(uint8_t index, bool active, const char *source);

// Returns true when any enabled flow-switch sensor with lockout enabled
// reports no flow. When true, the Matter module disables the pump and all
// relays to prevent equipment damage from running dry.
bool flowLockoutActive();
