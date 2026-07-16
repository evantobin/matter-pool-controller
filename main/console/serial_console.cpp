#include "console/serial_console.h"

#include <stdio.h>

#include <esp_console.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "io/user_feedback.h"
#include "matter/matter_setup.h"

static const char *TAG = "console";
static esp_console_repl_t *sRepl = nullptr;

// Small recovery interface for installation and service. Commands that alter
// state defer to the same public functions used by the physical controls.

static int resetCommand(int, char **) {
  printf("Rebooting Pool Conductor...\n");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  return 0;
}

static int factoryResetCommand(int, char **) {
  printf("Factory resetting Pool Conductor...\n");
  fflush(stdout);
  performFactoryReset("serial-command");
  return 0;
}

static int matterInfoCommand(int, char **) {
  printCommissioningInfo();
  return 0;
}

void setupSerialConsole() {
  const esp_console_cmd_t reset = {
    .command = "reset",
    .help = "Reboot without clearing configuration",
    .hint = nullptr,
    .func = &resetCommand,
    .argtable = nullptr,
    .func_w_context = nullptr,
    .context = nullptr,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&reset));

  const esp_console_cmd_t factoryReset = {
    .command = "factory-reset",
    .help = "Clear Matter configuration and restart",
    .hint = nullptr,
    .func = &factoryResetCommand,
    .argtable = nullptr,
    .func_w_context = nullptr,
    .context = nullptr,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&factoryReset));

  const esp_console_cmd_t matterInfo = {
    .command = "matter-info",
    .help = "Print Matter commissioning information",
    .hint = nullptr,
    .func = &matterInfoCommand,
    .argtable = nullptr,
    .func_w_context = nullptr,
    .context = nullptr,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&matterInfo));
  ESP_ERROR_CHECK(esp_console_register_help_command());

  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  replConfig.prompt = "poolkit> ";
  replConfig.max_cmdline_length = 64;

  esp_console_dev_uart_config_t uartConfig = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&uartConfig, &replConfig, &sRepl));
  ESP_ERROR_CHECK(esp_console_start_repl(sRepl));
  ESP_LOGI(TAG, "Serial commands ready: help, reset, factory-reset, matter-info");
}
