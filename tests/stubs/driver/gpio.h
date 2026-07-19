#pragma once

#include <cstdint>

typedef int gpio_num_t;

#define GPIO_MODE_INPUT         (1)
#define GPIO_PULLUP_ENABLE      (2)
#define GPIO_PULLDOWN_DISABLE   (4)
#define GPIO_INTR_DISABLE       (8)

struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode;
  int pull_up_en;
  int pull_down_en;
  int intr_type;
};

inline int gpio_config(gpio_config_t *) { return 0; }
inline int gpio_get_level(gpio_num_t) { return 1; }
