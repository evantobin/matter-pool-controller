// High-level pump control surface.
//
// This module sits between the Matter/cloud command sources and the low-level
// pump protocol driver. It owns the pump target state (mode, RPM, GPM, on/off)
// and is responsible for:
//
//   - Translating Matter LevelControl percent commands into RPM/GPM targets.
//   - Enforcing operating limits (MIN_RPM, maxRpm, MIN_GPM, MAX_GPM).
//   - Maintaining the keepalive timer that re-sends targets periodically.
//   - Deferred Matter attribute reporting (OnOff + LevelControl clusters).
//
// The pump itself is slow (RS-485 at 9600 baud) and has no watchdog — it will
// run at the last commanded speed indefinitely. The keepalive timer is a
// defense against lost RS-485 frames: if a command was corrupted, the next
// keepalive resend (every PUMP_KEEPALIVE_MS) corrects it.
//
// The control mode (RPM vs GPM) is persistent: once the user or schedule sets
// a mode, subsequent percent commands are interpreted in that mode.

#pragma once

#include <stdint.h>

// The pump can be controlled by RPM (direct speed) or GPM (flow rate).
// The protocol layer translates GPM to the pump-specific command format.
enum class PumpControlMode : uint8_t {
  Rpm = 0,
  Gpm = 1,
};

// Primary entry point from the Matter LevelControl cluster. Accepts 0–100
// percent, where 0 means OFF and 1–100 maps linearly to MIN_RPM..maxRpm.
// Also called by the schedule engine to apply a pre-computed percent.
void handlePumpChange(uint8_t speedPercent);

// Direct on/off control. Used by Matter OnOff cluster and the cloud
// action handler. When turning on, the pump resumes at the last non-zero
// percent (persisted in lastNonZeroPumpPercent).
void handlePumpPower(bool active);

// Sets the pump speed directly in RPM. Enforces MIN_RPM..maxRpm bounds.
// Switches the control mode to RPM. Used by the schedule and cloud.
void handlePumpRpm(uint16_t rpm);

// Sets the pump flow rate in GPM. Enforces MIN_GPM..MAX_GPM bounds.
// Switches the control mode to GPM. Used by the cloud action handler.
void handlePumpGpm(uint8_t gpm);

// Accessors for the current target state. These are read by the cloud
// telemetry publisher and the Matter module.
PumpControlMode pumpControlMode();
bool pumpTargetRunning();
uint16_t pumpTargetRpm();
uint8_t pumpTargetGpm();
uint8_t pumpTargetPercent();

// Schedules a deferred Matter attribute report for the pump endpoint.
// The report includes OnOff (running state) and CurrentLevel (percent).
// Coalesces multiple calls — only one report runs per Matter task callback.
void schedulePumpMatterReport(bool running, uint8_t percent);

// Returns true while a Matter attribute report callback is in flight.
// Used by the Matter module to avoid sending commands while a report
// is still updating the attributes (prevents stale-data race conditions).
bool isPumpMatterReportActive();

// Called from the main loop. Drains the pump protocol's status queue,
// updates the pumpStatus global, and fires the keepalive timer.
void readPumpStatus();
