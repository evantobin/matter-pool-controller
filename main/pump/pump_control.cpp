// Pump control implementation.
//
// This module manages the pump target state and bridges between the high-level
// control surface (percentages, RPM, GPM) and the low-level RS-485 protocol
// driver. It is intentionally stateless with respect to the bus — it doesn't
// know about RS-485 framing, baud rates, or protocol-specific commands.
// Those details are abstracted behind the PumpProtocol function table.
//
// The "last non-zero percent" pattern solves a UX problem: when a user turns
// the pump off and back on via Matter OnOff, the pump should resume at its
// previous speed, not at zero. Matter LevelControl has no "last level" memory,
// so we track it here.
//
// Keepalive: Pentair (and most) pumps have no watchdog timer. If an RS-485
// frame is corrupted or lost, the pump continues at its last valid command
// indefinitely. The keepalive timer re-sends the current target every
// PUMP_KEEPALIVE_MS to recover from lost frames without user intervention.
//
// Flow lockout integration: this module does NOT check flow. The caller
// (Matter module or main loop) is responsible for deciding whether to stop
// the pump based on flow switch state. This keeps safety logic centralized.

#include "pump/pump_control.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_attribute_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>

#include "app/state.h"
#include "pump/pump_protocol.h"

static const char *TAG = "pump";

// ---- internal state ----

// Guard flag set during Matter attribute report callbacks. The Matter module
// checks this before issuing commands to avoid a race where the report
// callback overwrites a command-initiated attribute value.
static volatile bool pumpMatterReportActive = false;

// Tracks the last non-zero percent so turning the pump back on restores the
// previous speed. Initialized to 45% (a reasonable medium speed) so the
// first OnOff=ON after boot gets a sensible default.
static uint8_t lastNonZeroPumpPercent = 45;

// Pump target state. These are the "desired" values — the pump may take
// several seconds to reach the target RPM/GPM after a command is sent.
// readPumpStatus() updates pumpStatus with the actual pump speed.
static PumpControlMode targetPumpMode = PumpControlMode::Rpm;
static uint16_t targetPumpRpm = DEFAULT_RPM;
static uint8_t targetPumpGpm = DEFAULT_GPM;
static bool targetPumpRunning = false;

// Keepalive timer: last time the target was sent to the pump protocol.
// Initialized to zero so the first readPumpStatus() sends immediately.
static uint32_t lastPumpKeepAliveMs = 0;

// ---- deferred Matter reporting ----

// Coalesced pump state for deferred Matter reports. Only one report is
// scheduled at a time; multiple calls to schedulePumpMatterReport() update
// these fields but don't schedule duplicate callbacks.
static struct {
  bool pending = false;
  bool running = false;
  uint8_t percent = 0;
} deferredReport;

static portMUX_TYPE deferredReportMux = portMUX_INITIALIZER_UNLOCKED;

// ---- logging helpers ----

// Matter attribute update vs. report: "update" changes the local attribute
// value (so subsequent reads reflect it); "report" sends a notification to
// subscribed Matter controllers. We do both so the local data model and
// remote controllers stay consistent.

static void logReportError(const char *name, esp_err_t err) {
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Matter pump %s report failed: %s", name, esp_err_to_name(err));
  }
}

static void logUpdateError(const char *name, esp_err_t err) {
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Matter pump %s update failed: %s", name, esp_err_to_name(err));
  }
}

// ---- public accessor ----

bool isPumpMatterReportActive() {
  return pumpMatterReportActive;
}

// ---- Matter reporting callback ----

// Runs on the Matter thread via PlatformMgr().ScheduleWork(). Updates both
// the OnOff cluster (running / not running) and the LevelControl cluster
// (CurrentLevel = speed percent). Both attributes are reported so subscribed
// controllers like HomeKit and Google Home reflect the current pump state.
//
// The percent → level mapping uses percentToLevel() from state.h to convert
// 0–100% to Matter's 1–254 range (0 = off, 1–254 = on at varying intensity).
static void reportToMatter(intptr_t) {
  bool running;
  uint8_t percent;
  taskENTER_CRITICAL(&deferredReportMux);
  running = deferredReport.running;
  percent = deferredReport.percent;
  deferredReport.pending = false;
  taskEXIT_CRITICAL(&deferredReportMux);

  if (pumpEndpointId == 0) return;  // No pump endpoint — nothing to report

  // Normalize: if running but percent is 0, use the last non-zero percent.
  // This handles the case where handlePumpPower(true) is called without a
  // prior speed command — we need a non-zero level for the LevelControl
  // cluster to show a meaningful value.
  if (!running) {
    percent = 0;
  } else if (percent == 0) {
    percent = lastNonZeroPumpPercent;
  } else {
    lastNonZeroPumpPercent = percent;
  }

  pumpMatterReportActive = true;

  // Update + report OnOff attribute
  esp_matter_attr_val_t onOffVal = esp_matter_bool(running);
  logUpdateError("OnOff", esp_matter::attribute::update(pumpEndpointId,
      chip::app::Clusters::OnOff::Id,
      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onOffVal));
  logReportError("OnOff", esp_matter::attribute::report(pumpEndpointId,
      chip::app::Clusters::OnOff::Id,
      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onOffVal));

  // Update + report LevelControl CurrentLevel attribute
  esp_matter_attr_val_t levelVal = esp_matter_nullable_uint8(nullable<uint8_t>(percentToLevel(percent)));
  logUpdateError("CurrentLevel", esp_matter::attribute::update(pumpEndpointId,
      chip::app::Clusters::LevelControl::Id,
      chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id, &levelVal));
  logReportError("CurrentLevel", esp_matter::attribute::report(pumpEndpointId,
      chip::app::Clusters::LevelControl::Id,
      chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id, &levelVal));

  pumpMatterReportActive = false;
}

// ---- public API: schedule deferred Matter report ----

// Called by handlePumpPower, handlePumpRpm, handlePumpGpm, and
// handlePumpChange after they modify the pump target. Coalesces multiple
// rapid changes into a single Matter report callback.
//
// Clamps percent to 0–100. If not running, percent is forced to 0 regardless
// of the input — a pump that's off shows 0% on the Matter controller.
void schedulePumpMatterReport(bool running, uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  if (!running) {
    percent = 0;
  } else if (percent == 0) {
    percent = lastNonZeroPumpPercent;
  } else {
    lastNonZeroPumpPercent = percent;
  }

  bool shouldSchedule = false;
  taskENTER_CRITICAL(&deferredReportMux);
  deferredReport.running = running;
  deferredReport.percent = percent;
  if (!deferredReport.pending) {
    deferredReport.pending = true;
    shouldSchedule = true;
  }
  taskEXIT_CRITICAL(&deferredReportMux);

  if (shouldSchedule) {
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(reportToMatter, 0);
    if (err != CHIP_NO_ERROR) {
      // Scheduling failed — reset pending so next change retries.
      taskENTER_CRITICAL(&deferredReportMux);
      deferredReport.pending = false;
      taskEXIT_CRITICAL(&deferredReportMux);
      ESP_LOGW(TAG, "Scheduling Matter pump report failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
  }
}

// ---- sending targets to the pump ----

// Sends the current target (RPM/GPM + power) to the pump protocol driver.
// If logTarget is true, logs the target at INFO level — used for user-
// initiated changes but suppressed for keepalive resends to reduce log spam.
//
// The 50 ms delay between setRpm/setGpm and setPower ensures the pump's
// RS-485 command buffer has been processed before the power command arrives.
// Some pumps queue commands and need a gap between speed and power.
static void sendPumpTarget(bool logTarget) {
  if (!pumpProtocol.setPower) return;  // Protocol not initialized

  if (targetPumpRunning) {
    // Send speed command first, then power. Order matters: setting power
    // first would start the pump at its last speed before the new speed
    // command has been processed.
    if (targetPumpMode == PumpControlMode::Gpm) {
      pumpProtocol.setGpm(targetPumpGpm);
    } else {
      pumpProtocol.setRpm(targetPumpRpm);
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // Let the speed command settle
    pumpProtocol.setPower(true);
    if (logTarget) {
      if (targetPumpMode == PumpControlMode::Gpm) {
        ESP_LOGI(TAG, "Pump target: ON, %u GPM", targetPumpGpm);
      } else {
        ESP_LOGI(TAG, "Pump target: ON, %u RPM", targetPumpRpm);
      }
    }
  } else {
    pumpProtocol.setPower(false);
    if (logTarget) {
      ESP_LOGI(TAG, "Pump target: OFF");
    }
  }

  // Reset the keepalive timer — we just sent a command, so the pump has
  // the latest target.
  lastPumpKeepAliveMs = (uint32_t)(esp_timer_get_time() / 1000);
}

// Helper called after any pump target change. Schedules the Matter report
// and notifies the controller state observer so the cloud service publishes
// updated telemetry.
static void reportPumpTargetChanged() {
  schedulePumpMatterReport(targetPumpRunning, pumpTargetPercent());
  notifyControllerStateChanged();
}

// ---- public API: control functions ----

// Entry point for Matter LevelControl MoveToLevel commands and schedule
// engine percent changes. 0% → off, 1–100% → on at that percent.
// The percent is always interpreted in the current control mode (RPM or GPM).
void handlePumpChange(uint8_t speedPercent) {
  if (speedPercent > 100) speedPercent = 100;
  if (speedPercent == 0) {
    handlePumpPower(false);
    return;
  }
  lastNonZeroPumpPercent = speedPercent;
  handlePumpRpm(percentToRpm(speedPercent));
}

// Toggles pump on/off. When turning on without a speed command, the pump
// resumes at lastNonZeroPumpPercent (or 45% if never set). This preserves
// the user's last speed setting across OnOff cycles.
void handlePumpPower(bool active) {
  const bool changed = targetPumpRunning != active;
  targetPumpRunning = active;
  sendPumpTarget(true);
  if (changed) reportPumpTargetChanged();
}

// Direct RPM target. Enforces MIN_RPM..maxRpm bounds. Always sets the pump
// to running — there's no "set RPM but stay off" mode.
// Used by the schedule engine and cloud action handler.
void handlePumpRpm(uint16_t rpm) {
  if (rpm < MIN_RPM) rpm = MIN_RPM;
  if (rpm > maxRpm) rpm = maxRpm;
  const bool changed = !targetPumpRunning || targetPumpMode != PumpControlMode::Rpm ||
                       targetPumpRpm != rpm;
  targetPumpMode = PumpControlMode::Rpm;
  targetPumpRpm = rpm;
  targetPumpRunning = true;
  lastNonZeroPumpPercent = rpmToPercent(rpm);
  sendPumpTarget(true);
  if (changed) reportPumpTargetChanged();
}

// Direct GPM target. Enforces MIN_GPM..MAX_GPM bounds. The pump protocol
// driver translates GPM to protocol-specific commands (some pumps support
// native GPM mode, others approximate it with RPM).
void handlePumpGpm(uint8_t gpm) {
  if (gpm < MIN_GPM) gpm = MIN_GPM;
  if (gpm > MAX_GPM) gpm = MAX_GPM;
  const bool changed = !targetPumpRunning || targetPumpMode != PumpControlMode::Gpm ||
                       targetPumpGpm != gpm;
  targetPumpMode = PumpControlMode::Gpm;
  targetPumpGpm = gpm;
  targetPumpRunning = true;
  // Map GPM to a percent for the Matter LevelControl display.
  // This is an approximate linear mapping: MIN_GPM→1%, MAX_GPM→100%.
  lastNonZeroPumpPercent = 1 +
      ((static_cast<uint16_t>(gpm - MIN_GPM) * 99) / (MAX_GPM - MIN_GPM));
  sendPumpTarget(true);
  if (changed) reportPumpTargetChanged();
}

// ---- accessors ----

PumpControlMode pumpControlMode() {
  return targetPumpMode;
}

bool pumpTargetRunning() {
  return targetPumpRunning;
}

uint16_t pumpTargetRpm() {
  return targetPumpRpm;
}

uint8_t pumpTargetGpm() {
  return targetPumpGpm;
}

// Computes the current target as a percent (0–100) for Matter and cloud
// telemetry. Returns 0 if the pump is off.
uint8_t pumpTargetPercent() {
  if (!targetPumpRunning) return 0;
  return targetPumpMode == PumpControlMode::Rpm
      ? rpmToPercent(targetPumpRpm)
      : 1 + ((static_cast<uint16_t>(targetPumpGpm - MIN_GPM) * 99) /
             (MAX_GPM - MIN_GPM));
}

// ---- keepalive ----

// Called from readPumpStatus(). If the keepalive interval has elapsed since
// the last target was sent, re-send it without logging. This handles the
// case where an RS-485 frame was lost — the pump keeps running at its last
// valid command, and the next keepalive restores the correct target.
static void maintainPumpTarget(uint32_t now) {
  if (now - lastPumpKeepAliveMs < PUMP_KEEPALIVE_MS) return;
  sendPumpTarget(false);  // false = suppress log (keepalive, not user action)
}

// ---- pump status polling ----

// Called from the main loop at ~100 Hz. Drains the pump protocol's status
// queue (there may be multiple status frames buffered), updates the global
// pumpStatus struct, and notifies observers if the status changed.
//
// A status change is significant when:
//   - The pump transitions between running and stopped (rpm crosses MIN_RPM)
//   - The RPM changes by ≥50 (covers ~1.4% of 3450 RPM — fine enough for UI)
//   - The flow rate or fault state changes
//   - The status was previously invalid (first valid frame)
//
// After processing status frames, the keepalive timer is checked and a new
// status request is sent if the polling interval has elapsed.
void readPumpStatus() {
  if (!pumpProtocol.readStatus) return;  // Protocol not initialized

  uint16_t rpm = 0, watts = 0;
  uint8_t flow = 0;
  bool fault = false;

  // Drain all queued status frames. The protocol driver buffers incoming
  // RS-485 responses and readStatus() pops one frame per call. Multiple
  // frames can accumulate between main loop iterations.
  while (pumpProtocol.readStatus(rpm, watts, flow, fault)) {
    const bool wasRunning = pumpStatus.rpm >= MIN_RPM;
    const bool isRunning = rpm >= MIN_RPM;
    const uint16_t rpmDelta = rpm > pumpStatus.rpm
        ? rpm - pumpStatus.rpm
        : pumpStatus.rpm - rpm;
    const bool statusChanged = !pumpStatus.valid || wasRunning != isRunning ||
                               rpmDelta >= 50 || pumpStatus.flow != flow ||
                               pumpStatus.pumpError != (fault ? 1u : 0u);

    pumpStatus.rpm = rpm;
    pumpStatus.watts = watts;
    pumpStatus.flow = flow;
    pumpStatus.pumpError = fault ? 1 : 0;
    pumpStatus.valid = true;
    // Notify with urgent=true on fault so the cloud service publishes
    // an immediate telemetry report instead of waiting for coalescing.
    if (statusChanged) notifyControllerStateChanged(fault);
  }

  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

  // Re-send the current pump target if the keepalive interval has elapsed.
  maintainPumpTarget(now);

  // Request a new status frame if the polling interval has elapsed.
  // The pump sends one status frame per request; the response arrives
  // asynchronously and is queued by the protocol driver.
  if (now - lastPumpStatusMs >= STATUS_POLL_MS) {
    pumpProtocol.requestStatus();
    lastPumpStatusMs = now;
  }
}
