#pragma once

#include <stdint.h>

// Physical status feedback and local factory-reset entry points.
void setupUserFeedback();
void updateUserFeedback();
void handleBootButton();
void performFactoryReset(const char *source);
void chirpOk();
void chirpError();
void chirpStartup();
bool isMatterCommissioned();
