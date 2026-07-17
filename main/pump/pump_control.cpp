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

static volatile bool pumpMatterReportActive = false;
static uint8_t lastNonZeroPumpPercent = 45;
static PumpControlMode targetPumpMode = PumpControlMode::Rpm;
static uint16_t targetPumpRpm = DEFAULT_RPM;
static uint8_t targetPumpGpm = DEFAULT_GPM;
static bool targetPumpRunning = false;
static uint32_t lastPumpKeepAliveMs = 0;

// Deferred Matter reporting data
static struct {
  bool pending = false;
  bool running = false;
  uint8_t percent = 0;
} deferredReport;

static portMUX_TYPE deferredReportMux = portMUX_INITIALIZER_UNLOCKED;

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

bool isPumpMatterReportActive() {
  return pumpMatterReportActive;
}

static void reportToMatter(intptr_t) {
  bool running;
  uint8_t percent;
  taskENTER_CRITICAL(&deferredReportMux);
  running = deferredReport.running;
  percent = deferredReport.percent;
  deferredReport.pending = false;
  taskEXIT_CRITICAL(&deferredReportMux);

  if (pumpEndpointId == 0) return;

  if (!running) {
    percent = 0;
  } else if (percent == 0) {
    percent = lastNonZeroPumpPercent;
  } else {
    lastNonZeroPumpPercent = percent;
  }

  pumpMatterReportActive = true;

  esp_matter_attr_val_t onOffVal = esp_matter_bool(running);
  logUpdateError("OnOff", esp_matter::attribute::update(pumpEndpointId,
      chip::app::Clusters::OnOff::Id,
      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onOffVal));
  logReportError("OnOff", esp_matter::attribute::report(pumpEndpointId,
      chip::app::Clusters::OnOff::Id,
      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onOffVal));

  esp_matter_attr_val_t levelVal = esp_matter_nullable_uint8(nullable<uint8_t>(percentToLevel(percent)));
  logUpdateError("CurrentLevel", esp_matter::attribute::update(pumpEndpointId,
      chip::app::Clusters::LevelControl::Id,
      chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id, &levelVal));
  logReportError("CurrentLevel", esp_matter::attribute::report(pumpEndpointId,
      chip::app::Clusters::LevelControl::Id,
      chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id, &levelVal));

  pumpMatterReportActive = false;
}

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
      taskENTER_CRITICAL(&deferredReportMux);
      deferredReport.pending = false;
      taskEXIT_CRITICAL(&deferredReportMux);
      ESP_LOGW(TAG, "Scheduling Matter pump report failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
  }
}

static void sendPumpTarget(bool logTarget) {
  if (!pumpProtocol.setPower) return;

  if (targetPumpRunning) {
    if (targetPumpMode == PumpControlMode::Gpm) {
      pumpProtocol.setGpm(targetPumpGpm);
    } else {
      pumpProtocol.setRpm(targetPumpRpm);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
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

  lastPumpKeepAliveMs = (uint32_t)(esp_timer_get_time() / 1000);
}

static void reportPumpTargetChanged() {
  schedulePumpMatterReport(targetPumpRunning, pumpTargetPercent());
  notifyControllerStateChanged();
}

void handlePumpChange(uint8_t speedPercent) {
  if (speedPercent > 100) speedPercent = 100;
  if (speedPercent == 0) {
    handlePumpPower(false);
    return;
  }
  lastNonZeroPumpPercent = speedPercent;
  handlePumpRpm(percentToRpm(speedPercent));
}

void handlePumpPower(bool active) {
  const bool changed = targetPumpRunning != active;
  targetPumpRunning = active;
  sendPumpTarget(true);
  if (changed) reportPumpTargetChanged();
}

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

void handlePumpGpm(uint8_t gpm) {
  if (gpm < MIN_GPM) gpm = MIN_GPM;
  if (gpm > MAX_GPM) gpm = MAX_GPM;
  const bool changed = !targetPumpRunning || targetPumpMode != PumpControlMode::Gpm ||
                       targetPumpGpm != gpm;
  targetPumpMode = PumpControlMode::Gpm;
  targetPumpGpm = gpm;
  targetPumpRunning = true;
  lastNonZeroPumpPercent = 1 +
      ((static_cast<uint16_t>(gpm - MIN_GPM) * 99) / (MAX_GPM - MIN_GPM));
  sendPumpTarget(true);
  if (changed) reportPumpTargetChanged();
}

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

uint8_t pumpTargetPercent() {
  if (!targetPumpRunning) return 0;
  return targetPumpMode == PumpControlMode::Rpm
      ? rpmToPercent(targetPumpRpm)
      : 1 + ((static_cast<uint16_t>(targetPumpGpm - MIN_GPM) * 99) /
             (MAX_GPM - MIN_GPM));
}

static void maintainPumpTarget(uint32_t now) {
  if (now - lastPumpKeepAliveMs < PUMP_KEEPALIVE_MS) return;
  sendPumpTarget(false);
}

void readPumpStatus() {
  if (!pumpProtocol.readStatus) return;

  uint16_t rpm = 0, watts = 0;
  uint8_t flow = 0;
  bool fault = false;

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
    if (statusChanged) notifyControllerStateChanged(fault);
  }

  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  maintainPumpTarget(now);
  if (now - lastPumpStatusMs >= STATUS_POLL_MS) {
    pumpProtocol.requestStatus();
    lastPumpStatusMs = now;
  }
}
