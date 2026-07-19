#pragma once
// Mock hardware abstraction layer for integration tests.
// Tracks GPIO pin state, UART buffers, and FreeRTOS primitives
// so the real firmware source can compile and run on the host.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>

// ============================================================================
// FreeRTOS stubs
// ============================================================================

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE  1
#define pdPASS  1
#define pdFALSE 0

typedef void * TaskHandle_t;
typedef void * SemaphoreHandle_t;
typedef uint32_t BaseType_t;

#define portMUX_TYPE              uint32_t
#define portMUX_INITIALIZER_UNLOCKED 0

inline void taskENTER_CRITICAL(volatile uint32_t *) {}
inline void taskEXIT_CRITICAL(volatile uint32_t *) {}
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)1; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (SemaphoreHandle_t)1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline void vTaskDelay(TickType_t) {}
inline void vTaskDelete(TaskHandle_t) {}

// ============================================================================
// ESP logging stubs
// ============================================================================

#define ESP_LOGE(tag, fmt, ...)   std::fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)   std::fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)   std::fprintf(stderr, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)   std::fprintf(stderr, "[D][%s] " fmt "\n", tag, ##__VA_ARGS__)

inline const char *esp_err_to_name(int err) { return "ESP_OK"; }

// ============================================================================
// ESP return codes
// ============================================================================

#define ESP_OK                      0
#define ESP_FAIL                    -1
#define ESP_ERR_INVALID_ARG         -2
#define ESP_ERR_NVS_NO_FREE_PAGES   -3
#define ESP_ERR_NVS_NEW_VERSION_FOUND -4
#define ESP_ERR_NOT_SUPPORTED       -5

// ============================================================================
// GPIO mock — tracks pin state
// ============================================================================

#define GPIO_MODE_OUTPUT          (1)
#define GPIO_MODE_INPUT           (2)
#define GPIO_MODE_OUTPUT_OD       (3)
#define GPIO_PULLUP_ENABLE        (4)
#define GPIO_PULLUP_DISABLE       (8)
#define GPIO_PULLDOWN_DISABLE     (8)
#define GPIO_INTR_DISABLE         (16)

#define UART_PIN_NO_CHANGE        (-1)

typedef int gpio_num_t;

struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode;
  int pull_up_en;
  int pull_down_en;
  int intr_type;
};

struct MockGPIO {
  int mode[64] = {};
  int level[64] = {};
  bool configured = false;

  void reset() {
    configured = false;
    for (int i = 0; i < 64; i++) { mode[i] = 0; level[i] = 0; }
  }
  void setInputLevel(int pin, int val) { level[pin] = val; }
  int  getLevel(int pin) const { return level[pin]; }
};
extern MockGPIO mockGpio;

inline int gpio_config(gpio_config_t *cfg) {
  if (!cfg) return ESP_FAIL;
  for (int i = 0; i < 64; i++) {
    if (cfg->pin_bit_mask & (1ULL << i)) {
      mockGpio.mode[i] = cfg->mode;
    }
  }
  mockGpio.configured = true;
  return ESP_OK;
}

inline int gpio_set_level(gpio_num_t pin, int level) {
  mockGpio.level[pin] = level;
  return ESP_OK;
}

inline int gpio_get_level(gpio_num_t pin) {
  return mockGpio.level[pin];
}

inline int gpio_reset_pin(gpio_num_t) { return ESP_OK; }

// ============================================================================
// UART mock — tracks TX/RX buffers
// ============================================================================

typedef int uart_port_t;
#define UART_NUM_1 1
#define UART_NUM_2 2

#define UART_DATA_8_BITS        (3)
#define UART_PARITY_DISABLE     (0)
#define UART_STOP_BITS_1        (1)
#define UART_HW_FLOWCTRL_DISABLE (0)
#define UART_SCLK_DEFAULT       (0)

struct uart_config_t {
  int baud_rate;
  int data_bits;
  int parity;
  int stop_bits;
  int flow_ctrl;
  int source_clk;
};

struct MockUART {
  static constexpr size_t BUF_SIZE = 4096;
  uint8_t txBuf[BUF_SIZE] = {};
  size_t txLen = 0;
  uint8_t rxBuf[BUF_SIZE] = {};
  size_t rxLen = 0;
  size_t rxRead = 0;

  void reset() {
    txLen = 0; rxLen = 0; rxRead = 0;
    memset(txBuf, 0, BUF_SIZE);
    memset(rxBuf, 0, BUF_SIZE);
  }

  // Feed bytes into the receive buffer (simulate incoming RS-485 data)
  void feedRx(const uint8_t *data, size_t len) {
    if (rxLen + len <= BUF_SIZE) {
      memcpy(rxBuf + rxLen, data, len);
      rxLen += len;
    }
  }

  // Read back what was transmitted
  std::string txHex() const {
    std::string s;
    char buf[8];
    for (size_t i = 0; i < txLen; i++) {
      snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", txBuf[i]);
      s += buf;
    }
    return s;
  }
};
extern MockUART mockUart;

inline int uart_param_config(uart_port_t, uart_config_t *) { return ESP_OK; }
inline int uart_set_pin(uart_port_t, int, int, int, int) { return ESP_OK; }
inline int uart_driver_install(uart_port_t, int, int, int, void *, int) { return ESP_OK; }

inline int uart_write_bytes(uart_port_t, const char *src, size_t size) {
  if (mockUart.txLen + size <= MockUART::BUF_SIZE) {
    memcpy(mockUart.txBuf + mockUart.txLen, src, size);
    mockUart.txLen += size;
  }
  return (int)size;
}

inline int uart_wait_tx_done(uart_port_t, TickType_t) { return ESP_OK; }

inline int uart_read_bytes(uart_port_t, uint8_t *dst, size_t size, TickType_t) {
  if (mockUart.rxRead >= mockUart.rxLen) return 0;
  size_t avail = mockUart.rxLen - mockUart.rxRead;
  size_t toRead = avail < size ? avail : size;
  memcpy(dst, mockUart.rxBuf + mockUart.rxRead, toRead);
  mockUart.rxRead += toRead;
  return (int)toRead;
}

inline int uart_get_buffered_data_len(uart_port_t, size_t *len) {
  if (mockUart.rxRead < mockUart.rxLen) {
    *len = mockUart.rxLen - mockUart.rxRead;
  } else {
    *len = 0;
  }
  return ESP_OK;
}

inline int uart_flush_input(uart_port_t) {
  mockUart.rxLen = 0;
  mockUart.rxRead = 0;
  return ESP_OK;
}

// ============================================================================
// ESP timer stub
// ============================================================================

inline uint64_t esp_timer_get_time() { return 0; }

// ============================================================================
// ESP rom delay stub
// ============================================================================

inline void esp_rom_delay_us(uint32_t) {}

// ============================================================================
// NVS stubs
// ============================================================================

inline int nvs_flash_init() { return ESP_OK; }
inline int nvs_flash_erase() { return ESP_OK; }

// ============================================================================
// Task interrupt stubs
// ============================================================================

inline void taskDISABLE_INTERRUPTS() {}
inline void taskENABLE_INTERRUPTS() {}
