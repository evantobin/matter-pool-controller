#include "pump/pump_protocol.h"
#include "pump/pentair_pump.h"
#include "pump/jandy_pump.h"
#include "esp_log.h"

static const char *TAG = "pump_proto";

PumpProtocol pumpProtocol = {};

static PentairPump *sPentairPump = nullptr;

// ---- Pentair wrappers ----

static bool pInitPentair(int txPin, int rxPin, uint8_t address) {
  if (sPentairPump) { delete sPentairPump; sPentairPump = nullptr; }
  sPentairPump = new PentairPump(UART_NUM_1, address, 16);
  if (!sPentairPump) return false;
  sPentairPump->begin(9600, rxPin, txPin);
  return true;
}

static void pSetPowerPentair(bool on) {
  if (!sPentairPump) return;
  if (on) { sPentairPump->setRemoteControl(true); sPentairPump->setPower(true); }
  else sPentairPump->setPower(false);
}

static void pSetRpmPentair(uint16_t rpm) {
  if (!sPentairPump) return;
  sPentairPump->setRemoteControl(true);
  sPentairPump->setRpm(rpm);
}

static void pSetGpmPentair(uint8_t gpm) {
  if (!sPentairPump) return;
  sPentairPump->setRemoteControl(true);
  sPentairPump->setGpm(gpm);
}

static void pReqStatusPentair() { if (sPentairPump) sPentairPump->requestStatus(); }

static bool pReadStatusPentair(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault) {
  if (!sPentairPump) return false;
  PentairPumpStatus status;
  if (!sPentairPump->readStatus(status)) return false;
  rpm = status.rpm; watts = status.watts; flow = status.flow; fault = status.pumpError != 0;
  return true;
}

static void pPollPentair() { if (sPentairPump) { PentairPumpStatus s; sPentairPump->readStatus(s); } }

// ---- Jandy wrappers ----

static void pSetPowerJandy(bool on) { ::jandySetRpm(on ? 1800 : 0); }
static void pSetRpmJandy(uint16_t rpm) { ::jandySetRpm(rpm); }
static void pSetGpmJandy(uint8_t) { ::jandySetRpm(2600); }
static void pReqStatusJandy() {}
static bool pReadStatusJandy(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault) {
  JandyPumpStatus status;
  if (!::jandyReadStatus(status)) return false;
  rpm = status.rpm; watts = status.watts; flow = 0; fault = false;
  return true;
}
static void pPollJandy() { ::jandyPoll(); }

// ---- Public ----

void pumpProtocolInit(PumpProtocolType type, uint8_t pumpAddress, int txPin, int rxPin, int /*dirPin*/) {
  switch (type) {
    case PumpProtocolType::Pentair:
      pumpProtocol.type = type;
      pumpProtocol.setPower = pSetPowerPentair;
      pumpProtocol.setRpm = pSetRpmPentair;
      pumpProtocol.setGpm = pSetGpmPentair;
      pumpProtocol.requestStatus = pReqStatusPentair;
      pumpProtocol.readStatus = pReadStatusPentair;
      pumpProtocol.poll = pPollPentair;
      pInitPentair(txPin, rxPin, pumpAddress);
      ESP_LOGI(TAG, "Pentair pump protocol selected");
      return;
    case PumpProtocolType::Jandy:
      pumpProtocol.type = type;
      pumpProtocol.setPower = pSetPowerJandy;
      pumpProtocol.setRpm = pSetRpmJandy;
      pumpProtocol.setGpm = pSetGpmJandy;
      pumpProtocol.requestStatus = pReqStatusJandy;
      pumpProtocol.readStatus = pReadStatusJandy;
      pumpProtocol.poll = pPollJandy;
      jandyPumpBegin(UART_NUM_2, txPin, rxPin, pumpAddress >= JANDY_PUMP_ID_MIN ? pumpAddress : JANDY_PUMP_ID_MIN);
      ESP_LOGI(TAG, "Jandy pump protocol selected");
      return;
  }
}
