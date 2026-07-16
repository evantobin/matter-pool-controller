#include <string.h>

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lib/support/logging/TextOnlyLogging.h>
#include <nvs_flash.h>

#include "board_identity.h"
#include "board_pins.h"
#include "matter_setup.h"
#include "pentair_pump.h"
#include "psa_init_patch.h"
#include "pump_control.h"
#include "relays.h"
#include "sensors.h"
#include "serial_console.h"
#include "state.h"
#include "user_feedback.h"

static const char *TAG = "main";

static void configureLogLevels() {
  static constexpr const char *ESP_MATTER_TAGS[] = {
    "esp_matter_attribute", "esp_matter_cluster", "esp_matter_command",
    "esp_matter_core", "esp_matter_endpoint", "esp_matter_feature",
    "esp_matter_client", "esp_matter_identify", "data_model", "mtr_nvs",
    "optional_attr", "weak_functions"
  };
  for (const char *tag : ESP_MATTER_TAGS) {
    esp_log_level_set(tag, ESP_LOG_NONE);
  }

  // Keep only Pool Conductor Matter warnings/errors; endpoint and command
  // transaction details are not useful during normal operation.
  esp_log_level_set("matter", ESP_LOG_WARN);
  chip::Logging::SetLogFilter(chip::Logging::kLogCategory_None);
}

// ---------- Pump ----------

PentairPump pentairPump(UART_NUM_1, 96, 16);

// ---------- Main ----------

extern "C" void app_main() {
  configureLogLevels();

  // Init NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // Patch: init PSA crypto mutexes properly before first use
  // (fixes mbedTLS 4.x static-init race in ESP-IDF v6.0)
  psa_crypto_init_patched();

  // Init network (Matter stack handles WiFi init)
  esp_netif_init();
  esp_event_loop_create_default();

  // Load device ID
  strncpy(deviceId, DEVICE_ID, sizeof(deviceId) - 1);
  ESP_LOGI(TAG, "Device ID: %s", deviceId);

  // Hardware setup
  setupUserFeedback();
  setupSensors();
  setupRelaysSafe();
  setAllRelaysOff("boot");
  setupSerialConsole();

  // Pentair pump on UART1
  pentairPump.begin(9600, BoardPins::RS485_RX, BoardPins::RS485_TX);
  pentairPump.setDebug(false);

  // Matter
  configureMatterCommissioning();
  beginMatterEndpoints();
  configureMatterMetadata();
  startMatter();

  // Check if commissioned
  if (isMatterCommissioned()) {
    ESP_LOGI(TAG, "Matter node is already commissioned.");
  } else {
    ESP_LOGI(TAG, "Waiting for Matter commissioning...");
    chirpStartup();
  }

  // Start pump control
  handlePumpChange(0);
  vTaskDelay(pdMS_TO_TICKS(50));
  pentairPump.requestStatus();
  lastPumpStatusMs = (uint32_t)(esp_timer_get_time() / 1000);

  // Main loop
  while (1) {
    if (!isMatterCommissioned()) {
      uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
      if (!printedCommissioningInfo || now - lastCommissioningLogMs >= COMMISSIONING_LOG_MS) {
        printCommissioningInfo();
        printedCommissioningInfo = true;
        lastCommissioningLogMs = now;
      }
      updateUserFeedback();
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    sampleSensors();
    readPumpStatus();
    updateUserFeedback();
    handleBootButton();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
