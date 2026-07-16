#pragma once

#include <stdint.h>

// Relay GPIO driver plus deferred Matter state reporting.
void setupRelaysSafe();
void setAllRelaysOff(const char *source);
void setRelay(uint8_t index, bool active, const char *source);
bool flowLockoutActive();
