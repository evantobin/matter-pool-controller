#!/bin/bash
# PoolConductor — ESP-IDF + esp-matter build environment
# Source this file: . ./env.sh

# ESP-IDF v6.0.2
. ~/.espressif/v6.0.2/esp-idf/export.sh

# esp-matter: gn / pigweed toolchain
export _PW_ACTUAL_ENVIRONMENT_ROOT="$HOME/esp/esp-matter/connectedhomeip/connectedhomeip/.environment"
export PATH="$HOME/esp/esp-matter/connectedhomeip/connectedhomeip/.environment/cipd/packages/pigweed:$PATH"

# esp-matter SDK path (used by CMakeLists.txt)
export ESP_MATTER_PATH="$HOME/esp/esp-matter"

echo "PoolConductor environment ready."
echo "  idf.py build   — compile"
echo "  idf.py flash   — write to device"
