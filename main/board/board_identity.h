#pragma once

// Per-controller identity and commissioning defaults. Change these before
// flashing each physical controller; do not reuse the development values.
static constexpr const char *BOARD_DEVICE_ID = "POOL-001";

// Matter commissioning credentials.
// The 11-digit manual pairing code and QR payload are generated from PIN + discriminator.
static constexpr uint32_t BOARD_MATTER_SETUP_PIN = 20202021;
static constexpr uint16_t BOARD_MATTER_SETUP_DISCRIMINATOR = 0xF00;

// SPAKE2+ parameters. Keep stable once printed on a label.
static constexpr const char *BOARD_MATTER_SPAKE2P_SALT = "PoolConductorSalt";
static constexpr uint32_t BOARD_MATTER_SPAKE2P_ITERATIONS = 1000;
