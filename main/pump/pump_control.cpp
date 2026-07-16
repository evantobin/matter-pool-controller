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
#include "pump/pentair_pump.h"

static const char *TAG = "pump";

// Converts Matter-level on/off and dimmer requests into a maintained local pump
// target, then reports observed pump state back to Matter.
extern PentairPump pentairPump;

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
  pentairPump.setRemoteControl(true);
  vTaskDelay(pdMS_TO_TICKS(50));

  if (targetPumpRunning) {
    if (targetPumpMode == PumpControlMode::Gpm) {
      pentairPump.setGpm(targetPumpGpm);
    } else {
      pentairPump.setRpm(targetPumpRpm);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    pentairPump.setPower(true);
    if (logTarget) {
      if (targetPumpMode == PumpControlMode::Gpm) {
        ESP_LOGI(TAG, "Pump target: ON, %u GPM", targetPumpGpm);
      } else {
        ESP_LOGI(TAG, "Pump target: ON, %u RPM", targetPumpRpm);
      }
    }
  } else {
    pentairPump.setPower(false);
    if (logTarget) {
      ESP_LOGI(TAG, "Pump target: OFF");
    }
  }

  lastPumpKeepAliveMs = (uint32_t)(esp_timer_get_time() / 1000);
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
  targetPumpRunning = active;
  sendPumpTarget(true);
}

void handlePumpRpm(uint16_t rpm) {
  if (rpm < MIN_RPM) rpm = MIN_RPM;
  if (rpm > maxRpm) rpm = maxRpm;
  targetPumpMode = PumpControlMode::Rpm;
  targetPumpRpm = rpm;
  targetPumpRunning = true;
  lastNonZeroPumpPercent = rpmToPercent(rpm);
  sendPumpTarget(true);
}

void handlePumpGpm(uint8_t gpm) {
  if (gpm < MIN_GPM) gpm = MIN_GPM;
  if (gpm > MAX_GPM) gpm = MAX_GPM;
  targetPumpMode = PumpControlMode::Gpm;
  targetPumpGpm = gpm;
  targetPumpRunning = true;
  sendPumpTarget(true);
}

PumpControlMode pumpControlMode() {
  return targetPumpMode;
}

static void maintainPumpTarget(uint32_t now) {
  if (now - lastPumpKeepAliveMs < PUMP_KEEPALIVE_MS) return;
  sendPumpTarget(false);
}

void readPumpStatus() {
  PentairPumpStatus status;
  while (pentairPump.readStatus(status)) {
    pumpStatus.rpm = status.rpm;
    pumpStatus.watts = status.watts;
    pumpStatus.flow = status.flow;
    pumpStatus.mode = status.mode;
    pumpStatus.ppc = status.ppc;
    pumpStatus.reserved = status.reserved;
    pumpStatus.command = status.command;
    pumpStatus.driveState = status.driveState;
    pumpStatus.pumpError = status.pumpError;
    pumpStatus.statusWord = status.statusWord;
    pumpStatus.clockHour = status.clockHour;
    pumpStatus.clockMinute = status.clockMinute;
    pumpStatus.valid = true;

    bool running = status.command == 10 || status.driveState == 2 || status.rpm >= MIN_RPM;
    uint8_t percent = running ? rpmToPercent(status.rpm) : 0;

    ESP_LOGI(TAG, "Status: command=%u drive=%u rpm=%u flow=%u watts=%u target=%s",
             status.command, status.driveState, status.rpm, status.flow, status.watts,
             targetPumpMode == PumpControlMode::Gpm ? "GPM" : "RPM");
    schedulePumpMatterReport(running, percent);
  }

  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  maintainPumpTarget(now);
  if (now - lastPumpStatusMs >= STATUS_POLL_MS) {
    pentairPump.requestStatus();
    lastPumpStatusMs = now;
  }
}
