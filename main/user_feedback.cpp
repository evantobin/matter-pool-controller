#include "user_feedback.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <led_strip.h>
#include <string.h>

#include "board_pins.h"
#include "relays.h"
#include "state.h"

static const char *TAG = "feedback";

static led_strip_handle_t sLedStrip = nullptr;
static bool sBuzzerInitialized = false;
static bool sButtonConfigured = false;
static bool sFactoryResetInProgress = false;

// ---------- Forward declarations ----------

static void setRgb(uint8_t r, uint8_t g, uint8_t b);
static void chirpTone(uint16_t freq, uint16_t durationMs);

// ---------- LED / Buzzer setup ----------

void setupUserFeedback() {
  // Configure boot button
  gpio_config_t btnCfg = {
    .pin_bit_mask = (1ULL << BoardPins::BootButton),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&btnCfg);
  sButtonConfigured = true;

  if (BoardPins::RgbLed >= 0) {
    led_strip_config_t stripCfg = {
      .strip_gpio_num = (gpio_num_t)BoardPins::RgbLed,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmtCfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000, // 10 MHz
      .mem_block_symbols = 64,
      .flags = { .with_dma = false },
    };
    led_strip_new_rmt_device(&stripCfg, &rmtCfg, &sLedStrip);
    setRgb(0, 0, 24);
  }

  // Configure buzzer via LEDC
  ledc_timer_config_t timerCfg = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_13_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 1000,
    .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&timerCfg);

  ledc_channel_config_t channelCfg = {
    .gpio_num = BoardPins::Buzzer,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = LEDC_TIMER_0,
    .duty = 0,
    .hpoint = 0,
    .flags = { .output_invert = false },
  };
  ledc_channel_config(&channelCfg);
  sBuzzerInitialized = true;

  // Startup chirp
  chirpTone(1800, 80);
}

// ---------- LED helpers ----------

static void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!sLedStrip) return;
  led_strip_set_pixel(sLedStrip, 0, r, g, b);
  led_strip_refresh(sLedStrip);
}

// ---------- Buzzer helpers ----------

static void chirpTone(uint16_t freq, uint16_t durationMs) {
  if (!sBuzzerInitialized) return;
  ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096); // ~50% duty at 13-bit
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  vTaskDelay(pdMS_TO_TICKS(durationMs));
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void chirpOk() {
  chirpTone(2200, 70);
}

void chirpError() {
  chirpTone(440, 180);
}

void chirpStartup() {
  chirpTone(1200, 120);
}

static void chirpResetWarning() {
  chirpTone(700, 80);
}

// ---------- Commissioned check ----------

bool isMatterCommissioned() {
  return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

// ---------- Status ----------

static UserStatus currentUserStatus() {
  if (!isMatterCommissioned()) return UserStatus::Commissioning;
  if (flowLockoutActive()) return UserStatus::FlowLockout;
  return UserStatus::Online;
}

// ---------- LED update ----------

void updateUserFeedback() {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (now - lastLedUpdateMs < 100) return;
  lastLedUpdateMs = now;

  bool blinkSlow = ((now / 700) % 2) == 0;
  bool blinkFast = ((now / 200) % 2) == 0;
  uint8_t pulse = (uint8_t)(16 + ((now / 16) % 48));

  switch (currentUserStatus()) {
    case UserStatus::Commissioning:
      setRgb(blinkSlow ? 48 : 0, blinkSlow ? 28 : 0, 0);
      break;
    case UserStatus::Online:
      setRgb(0, 36, 8);
      break;
    case UserStatus::FlowLockout:
      setRgb(blinkFast ? 64 : 0, 0, blinkFast ? 64 : 0);
      break;
    case UserStatus::Booting:
    default:
      setRgb(0, 0, pulse);
      break;
  }
}

// ---------- Boot button / Factory reset ----------

void performFactoryReset(const char *source) {
  if (sFactoryResetInProgress) return;
  sFactoryResetInProgress = true;

  ESP_LOGW(TAG, "Factory reset requested by %s.", source ? source : "unknown");
  setAllRelaysOff("factory-reset");
  clearNvsConfig();
  chirpError();
  vTaskDelay(pdMS_TO_TICKS(250));
  chirpError();
  esp_matter::factory_reset();
  esp_restart();
}

void handleBootButton() {
  if (!sButtonConfigured) return;

  bool pressed = gpio_get_level((gpio_num_t)BoardPins::BootButton) == 0;
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

  if (!pressed) {
    bootButtonPressedAtMs = 0;
    lastResetWarningMs = 0;
    factoryResetTriggered = false;
    return;
  }

  if (bootButtonPressedAtMs == 0) {
    bootButtonPressedAtMs = now;
  }

  uint32_t heldMs = now - bootButtonPressedAtMs;
  if (heldMs >= 2000 && now - lastResetWarningMs >= 2000) {
    lastResetWarningMs = now;
    chirpResetWarning();
  }

  if (!factoryResetTriggered && heldMs >= FACTORY_RESET_HOLD_MS) {
    factoryResetTriggered = true;
    performFactoryReset("boot-button");
  }
}
