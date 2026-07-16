#pragma once

#include <stdint.h>

void setupRelaysSafe();
void setAllRelaysOff(const char *source);
void setRelay(uint8_t index, bool active, const char *source);
bool flowLockoutActive();
