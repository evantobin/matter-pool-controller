// Main application entry point and cooperative event loop.
//
// This file owns the startup sequence and the forever loop. It is
// intentionally thin — all hardware and protocol logic lives in their
// respective modules (io/, pump/, matter/, cloud/, schedule/). The main
// loop's job is to call each module's poll/sample function at the right
// rate and in the right order.
//
// Startup ordering matters:
//   1. NVS init (needed by everything)
//   2. PSA crypto init patch (mbedTLS 4.x static-init race fix)
//   3. Network stack (TCP/IP + event loop)
//   4. Hardware I/O (relays OFF first, then sensors, then pump)
//   5. Cloud service (doesn't connect until WiFi is up)
//   6. Schedule (loads from NVS, won't execute without SNTP time)
//   7. Matter (endpoints need relay/sensor config to be loaded)
//
// The main loop has two modes:
//   - Uncommissioned: print commissioning info once, update LED, spin.
//   - Commissioned: sample sensors, read pump, update UI, poll cloud + schedule.

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

#include "app/state.h"
#include "board/board_identity.h"
#include "board/board_pins.h"
#include "io/relays.h"
#include "io/sensors.h"
#include "io/user_feedback.h"
#include "matter/matter_setup.h"
#include "platform/psa_init_patch.h"
#include "pump/pump_control.h"
#include "pump/pump_protocol.h"
#include "cloud/cloud_service.h"
#include "schedule/schedule.h"

static const char *TAG = "main";

// ---- log level configuration ----

// ESP-Matter is extremely verbose by default — every attribute read/write,
// every cluster interaction, every endpoint transaction logs at INFO level.
// For a production pool controller, this noise drowns out actual issues.
// We silence the Matter SDK internals and keep only our own module logs.
static void configureLogLevels() {
  // All the noisy ESP-Matter internal tags — set to NONE to keep the
  // serial console usable during normal operation.
  static constexpr const char *ESP_MATTER_TAGS[] = {
    "esp_matter_attribute", "esp_matter_cluster", "esp_matter_command",
    "esp_matter_core", "esp_matter_endpoint", "esp_matter_feature",
    "esp_matter_client", "esp_matter_identify", "data_model", "mtr_nvs",
    "optional_attr", "weak_functions"
  };
  for (const char *tag : ESP_MATTER_TAGS) {
    esp_log_level_set(tag, ESP_LOG_NONE);
  }

  // The generic "matter" tag still produces warnings (binding failures,
  // subscription errors) which are useful for debugging.
  // CHIP library logging is also silenced — we don't need the CHIP
  // stack's internal progress/error messages in production.
  esp_log_level_set("matter", ESP_LOG_WARN);
  chip::Logging::SetLogFilter(chip::Logging::kLogCategory_None);
}

// ---- network helpers ----

// Checks whether the WiFi station interface has a valid IPv4 address.
// This is a fast, non-blocking check — it reads the netif state directly
// from the lwIP stack. Used by the main loop to gate cloud MQTT connection
// attempts (don't try to connect when WiFi is down).
//
// We check for a non-zero IP address rather than just netif_up because
// the netif can be "up" during the DHCP discovery phase before an address
// is assigned. The cloud MQTT stack needs a routable address to connect.
static bool wifiStationHasAddress() {
  esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t address = {};
  return station && esp_netif_is_netif_up(station) &&
         esp_netif_get_ip_info(station, &address) == ESP_OK && address.ip.addr != 0;
}

// ---- application entry point ----

extern "C" void app_main() {
  configureLogLevels();

  // ---- NVS initialization ----
  // Non-volatile storage holds Matter fabric credentials, user config,
  // schedule data, and cloud settings. If the flash is brand new or the
  // NVS format has changed (ESP-IDF version upgrade), erase and re-init.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // ---- PSA crypto mutex patch ----
  // mbedTLS 4.x in ESP-IDF v6.0 has a static-initialization race: the
  // PSA crypto mutexes are used before the C++ runtime has initialized
  // them. This patch manually initializes them early in the boot sequence.
  psa_crypto_init_patched();

  // ---- Network stack ----
  // The Matter stack manages WiFi connection internally, but we need the
  // netif layer and event loop to be initialized before Matter starts.
  esp_netif_init();
  esp_event_loop_create_default();

  // ---- Device identity ----
  // Copy the compile-time device ID into the runtime buffer. The cloud
  // module may modify this (e.g., truncating for MQTT client ID limits)
  // so we need a mutable copy.
  strncpy(deviceId, DEVICE_ID, sizeof(deviceId) - 1);
  ESP_LOGI(TAG, "Device ID: %s", deviceId);

  // ---- Hardware setup ----
  // Order is important: user feedback first (LED/buzzer for error indication
  // during init), then sensors (configure GPIOs), then relays OFF (safety:
  // all outputs inactive before anything else can turn them on).
  setupUserFeedback();
  setupSensors();
  setupRelaysSafe();
  setAllRelaysOff("boot");  // Explicit all-off for safety

  // ---- Cloud service ----
  // Initialize the MQTT client and Device Shadow subscriptions. Does not
  // connect yet — connection happens in the main loop once WiFi is up.
  initializeCloudService();

  // ---- Schedule engine ----
  // Load schedule blocks from NVS. Won't execute until SNTP time is synced.
  initializeSchedule();

  // ---- Pump protocol ----
  // Initialize the RS-485 UART for the selected pump protocol. dirPin = -1
  // means the RS-485 transceiver uses auto-direction (no DE/RE control pin).
  pumpProtocolInit(PUMP_PROTOCOL, PUMP_ADDRESS, BoardPins::RS485_RX, BoardPins::RS485_TX, -1);

  // ---- Matter stack ----
  // Commissioning must be configured before endpoints are created because
  // endpoint creation reads the commissionable node provider. Metadata
  // (vendor, product, firmware version) is set after endpoints so it can
  // reference endpoint IDs if needed.
  configureMatterCommissioning();
  beginMatterEndpoints();
  configureMatterMetadata();
  startMatter();

  // ---- Commissioning state ----
  // If the device was previously commissioned, the Matter fabric data is
  // stored in NVS and the stack starts advertising automatically.
  // Uncommissioned devices wait for the user to scan the QR code.
  if (isMatterCommissioned()) {
    ESP_LOGI(TAG, "Matter node is already commissioned.");
  } else {
    ESP_LOGI(TAG, "Waiting for Matter commissioning...");
    chirpStartup();  // Audible feedback that the device is ready to commission
  }

  // ---- Initial pump state ----
  // Send the default pump target (OFF, 0 RPM) so the pump protocol has
  // a known starting state. The 50ms delay ensures the protocol module
  // has processed any pending init before we request status.
  handlePumpChange(0);
  vTaskDelay(pdMS_TO_TICKS(50));
  pumpProtocol.requestStatus();
  lastPumpStatusMs = (uint32_t)(esp_timer_get_time() / 1000);

  // ---- Main cooperative loop ----
  // This runs forever at ~100 Hz (10 ms tick). Every module's work is
  // designed to be non-blocking — each poll/sample call returns immediately
  // if there's nothing to do, or does a bounded amount of work.
  //
  // The uncommissioned branch is intentionally minimal: it updates the LED
  // and prints commissioning info once. No sensors, no relays, no pump, no
  // cloud, no schedule — nothing should run until the device is claimed.
  while (1) {
    if (!isMatterCommissioned()) {
      // Print the QR code and manual pairing code to the UART console on
      // the first loop iteration after boot.
      if (!printedCommissioningInfo) {
        printCommissioningInfo();
        printedCommissioningInfo = true;
      }
      updateUserFeedback();  // Blink LED in commissioning pattern
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;  // Skip all operational logic
    }

    // ---- Operational loop (commissioned) ----

    // Sample all digital sensors (water level, flow switches) at the
    // configured rate. Temperature sampling is handled internally at a
    // slower rate (TEMPERATURE_SAMPLE_MS).
    sampleSensors();

    // Read queued pump status frames and maintain the keepalive timer.
    // This is the only function that talks to the RS-485 bus — the pump
    // protocol is polled, not interrupt-driven.
    readPumpStatus();

    // Update the RGB LED and piezoelectric buzzer based on current state
    // (fault, flow lockout, cloud connectivity).
    updateUserFeedback();

    // Check the boot button for a factory-reset long-press.
    handleBootButton();

    // Tell the cloud service whether WiFi is up so it can start/stop
    // the MQTT connection accordingly.
    cloudServiceSetConnected(wifiStationHasAddress());

    // Evaluate the schedule against the current wall-clock time. May call
    // handlePumpRpm() and setRelay() if a new block is entered.
    pollSchedule();

    // Process incoming MQTT messages (shadow delta, actions) and publish
    // outstanding telemetry reports.
    pollCloudService();

    // 10 ms tick = 100 Hz. Fast enough for responsive Matter command
    // handling (commands arrive on a separate FreeRTOS task), slow enough
    // to keep the idle CPU time high for power efficiency.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
