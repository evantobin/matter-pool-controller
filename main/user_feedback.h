#pragma once

#include <stdint.h>

void setupUserFeedback();
void updateUserFeedback();
void handleBootButton();
void performFactoryReset(const char *source);
void chirpOk();
void chirpError();
void chirpStartup();
bool isMatterCommissioned();
