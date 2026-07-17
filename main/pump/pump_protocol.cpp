#include "pump/pump_protocol.h"
#include "pump/pentair_pump.h"
#include "pump/jandy_pump.h"
#include "esp_log.h"

static const char *TAG = "pump_proto";

PumpProtocol pumpProtocol = {};
static PentairPump sPentairPump;
static uint8_t sJandyPumpId = JANDY_PUMP_ID_MIN;

// ---- Pentair wrappers ----

static void pentairBegin(int txPin, int rxPin, uint8_t address) {
  sPentairPump.begin(9600, txPin, rxPin, address);
}

static void pentairSetPower(bool on) {
  if (on) {
    sPentairPump.setRemoteControl(true);
    sPentairPump.setPower(true);
  } else {
    sPentairPump.setPower(false);
  }
}

static void pentairSetRpm(uint16_t rpm) {
  sPentairPump.setRemoteControl(true);
  sPentairPump.setRpm(rpm);
}

static void pentairSetGpm(uint8_t gpm) {
  sPentairPump.setRemoteControl(true);
  sPentairPump.setGpm(gpm);
}

static void pentairRequestStatus() {
  sPentairPump.requestStatus();
}

static bool pentairReadStatus(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault) {
  PentairPumpStatus status;
  if (!sPentairPump.readStatus(status)) return false;
  rpm = status.rpm;
  watts = status.watts;
  flow = status.flow;
  fault = status.pumpError != 0;
  return true;
}

static void pentairPoll() {
  PentairPumpStatus s;
  sPentairPump.readStatus(s);
}

// ---- Jandy wrappers ----

static void jandyBegin(int txPin, int rxPin, uint8_t address) {
  if (address >= JANDY_PUMP_ID_MIN && address <= JANDY_PUMP_ID_MAX) {
    sJandyPumpId = address;
  }
  jandyPumpBegin(UART_NUM_2, txPin, rxPin, sJandyPumpId);
}

static void jandySetPower(bool on) {
  jandySetRpm(on ? 1800 : 0);
}

static void jandySetRpm(uint16_t rpm) {
  jandySetRpm(rpm);
}

static void jandySetGpm(uint8_t /*gpm*/) {
  jandySetRpm(2600);
}

static void jandyRequestStatus() {}

static bool jandyReadStatus(uint16_t &rpm, uint16_t &watts, uint8_t &flow, bool &fault) {
  JandyPumpStatus status;
  if (!jandyReadStatus(status)) return false;
  rpm = status.rpm;
  watts = status.watts;
  flow = 0;
  fault = false;
  return true;
}

static void jandyPoll() {
  jandyPoll();
}

// ---- Public ----

void pumpProtocolInit(PumpProtocolType type, uint8_t pumpAddress, int txPin, int rxPin, int /*dirPin*/) {
  switch (type) {
    case PumpProtocolType::Pentair:
      pumpProtocol.type = type;
      pumpProtocol.begin = pentairBegin;
      pumpProtocol.setPower = pentairSetPower;
      pumpProtocol.setRpm = pentairSetRpm;
      pumpProtocol.setGpm = pentairSetGpm;
      pumpProtocol.requestStatus = pentairRequestStatus;
      pumpProtocol.readStatus = pentairReadStatus;
      pumpProtocol.poll = pentairPoll;
      ESP_LOGI(TAG, "Pentair pump protocol selected");
      break;
    case PumpProtocolType::Jandy:
      pumpProtocol.type = type;
      pumpProtocol.begin = jandyBegin;
      pumpProtocol.setPower = jandySetPower;
      pumpProtocol.setRpm = jandySetRpm;
      pumpProtocol.setGpm = jandySetGpm;
      pumpProtocol.requestStatus = jandyRequestStatus;
      pumpProtocol.readStatus = jandyReadStatus;
      pumpProtocol.poll = jandyPoll;
      ESP_LOGI(TAG, "Jandy pump protocol selected");
      break;
  }
  if (pumpProtocol.begin) {
    pumpProtocol.begin(txPin, rxPin, pumpAddress);
  }
}
