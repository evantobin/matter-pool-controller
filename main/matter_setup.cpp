#include "matter_setup.h"

#include <string.h>

#include <app-common/zap-generated/ids/Attributes.h>
#include <crypto/CHIPCryptoPAL.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_endpoint.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <protocols/secure_channel/PASESession.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

#include "board_identity.h"
#include "board_pins.h"
#include "pump_control.h"
#include "relays.h"
#include "state.h"

static const char *TAG = "matter";

static constexpr char MATTER_MANUFACTURER[] = "Pool Conductor";
static constexpr char MATTER_MODEL[] = "Conductor";
static constexpr char MATTER_NODE_LABEL[] = "Pool Conductor";

static char RELAY_LABELS[BoardPins::RelayCount][16] = {
  "Relay 1", "Relay 2", "Relay 3", "Relay 4", "Relay 5", "Relay 6"
};

// ---------- Commissionable Data Provider ----------

class PoolConductorCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider,
                                                 public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
  // --- CommissionableDataProvider ---
  CHIP_ERROR GetSetupDiscriminator(uint16_t &setupDiscriminator) override {
    setupDiscriminator = BOARD_MATTER_SETUP_DISCRIMINATOR;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR SetSetupDiscriminator(uint16_t setupDiscriminator) override {
    return setupDiscriminator <= chip::kMaxDiscriminatorValue ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
  }

  CHIP_ERROR GetSpake2pIterationCount(uint32_t &iterationCount) override {
    iterationCount = BOARD_MATTER_SPAKE2P_ITERATIONS;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &saltBuf) override {
    const size_t saltLen = strlen(BOARD_MATTER_SPAKE2P_SALT);
    if (saltBuf.size() < saltLen) {
      return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    memcpy(saltBuf.data(), BOARD_MATTER_SPAKE2P_SALT, saltLen);
    saltBuf.reduce_size(saltLen);
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf, size_t &outVerifierLen) override {
    outVerifierLen = chip::Crypto::kSpake2p_VerifierSerialized_Length;
    if (verifierBuf.size() < outVerifierLen) {
      return CHIP_ERROR_BUFFER_TOO_SMALL;
    }

    chip::Crypto::Spake2pVerifier verifier;
    chip::ByteSpan saltSpan(
      reinterpret_cast<const uint8_t *>(BOARD_MATTER_SPAKE2P_SALT),
      strlen(BOARD_MATTER_SPAKE2P_SALT)
    );
    CHIP_ERROR err = verifier.Generate(BOARD_MATTER_SPAKE2P_ITERATIONS, saltSpan, BOARD_MATTER_SETUP_PIN);
    if (err != CHIP_NO_ERROR) {
      return err;
    }
    return verifier.Serialize(verifierBuf);
  }

  CHIP_ERROR GetSetupPasscode(uint32_t &setupPasscode) override {
    setupPasscode = BOARD_MATTER_SETUP_PIN;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR SetSetupPasscode(uint32_t setupPasscode) override {
    return chip::PayloadContents::IsValidSetupPIN(setupPasscode) ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
  }

  // --- DeviceInstanceInfoProvider ---

  CHIP_ERROR GetVendorName(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, MATTER_MANUFACTURER);
  }

  CHIP_ERROR GetVendorId(uint16_t &vendorId) override {
    vendorId = 0xFFF1;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetProductName(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, MATTER_MODEL);
  }

  CHIP_ERROR GetProductId(uint16_t &productId) override {
    productId = 0x8000;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetPartNumber(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, "");
  }

  CHIP_ERROR GetProductURL(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, "");
  }

  CHIP_ERROR GetProductLabel(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, MATTER_MODEL);
  }

  CHIP_ERROR GetSerialNumber(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, deviceId);
  }

  CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override {
    year = 2024;
    month = 1;
    day = 1;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetHardwareVersion(uint16_t &hardwareVersion) override {
    hardwareVersion = 1;
    return CHIP_NO_ERROR;
  }

  CHIP_ERROR GetHardwareVersionString(char *buf, size_t bufSize) override {
    return CopyString(buf, bufSize, "1.0");
  }

  CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &uniqueIdSpan) override {
    // Use a fixed 16-byte unique ID for rotating device ID
    static const uint8_t kUniqueId[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                          0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    return CopySpan(uniqueIdSpan, chip::ByteSpan(kUniqueId, sizeof(kUniqueId)));
  }

private:
  static CHIP_ERROR CopyString(char *buf, size_t bufSize, const char *src) {
    size_t len = strlen(src);
    if (len + 1 > bufSize) return CHIP_ERROR_BUFFER_TOO_SMALL;
    memcpy(buf, src, len + 1);
    return CHIP_NO_ERROR;
  }

  static CHIP_ERROR CopySpan(chip::MutableByteSpan &dest, chip::ByteSpan src) {
    if (dest.size() < src.size()) return CHIP_ERROR_BUFFER_TOO_SMALL;
    memcpy(dest.data(), src.data(), src.size());
    dest.reduce_size(src.size());
    return CHIP_NO_ERROR;
  }
};

static PoolConductorCommissionableDataProvider sCommissionableDataProvider;

void configureMatterCommissioning() {
  if (!chip::PayloadContents::IsValidSetupPIN(BOARD_MATTER_SETUP_PIN)) {
    ESP_LOGE(TAG, "Invalid Matter setup PIN: %lu", BOARD_MATTER_SETUP_PIN);
    return;
  }
  if (BOARD_MATTER_SETUP_DISCRIMINATOR > chip::kMaxDiscriminatorValue) {
    ESP_LOGE(TAG, "Invalid Matter discriminator: 0x%X", BOARD_MATTER_SETUP_DISCRIMINATOR);
    return;
  }
  chip::DeviceLayer::SetCommissionableDataProvider(&sCommissionableDataProvider);
  chip::DeviceLayer::SetDeviceInstanceInfoProvider(&sCommissionableDataProvider);
}

// ---------- Attribute Callback ----------

static esp_err_t matterAttributeCallback(
    esp_matter::attribute::callback_type_t type,
    uint16_t endpointId,
    uint32_t clusterId,
    uint32_t attributeId,
    esp_matter_attr_val_t *val,
    void *privData)
{
  if (type != esp_matter::attribute::PRE_UPDATE) return ESP_OK;

  // Relay endpoints: OnOff cluster
  if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
    for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
      if (endpointId == relayEndpointIds[i]) {
        if (!relayConfigs[i].enabled) {
          return ESP_ERR_INVALID_STATE;
        }
        if (flowLockoutActive()) {
          return ESP_ERR_INVALID_STATE;
        }
        setRelay(i, val->val.b, "matter");
        return ESP_OK;
      }
    }
  }

  // Pump endpoint: dimmable plug-in unit
  if (endpointId == pumpEndpointId) {
    if ((clusterId == OnOff::Id || clusterId == LevelControl::Id) && isPumpMatterReportActive()) {
      return ESP_OK;
    }
    if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
      uint8_t percent = val->val.b ? rpmToPercent(DEFAULT_RPM) : 0;
      ESP_LOGI(TAG, "Matter pump power: %d -> %u%%", val->val.b, percent);
      handlePumpChange(percent);
      return ESP_OK;
    }
    if (clusterId == LevelControl::Id && attributeId == LevelControl::Attributes::CurrentLevel::Id) {
      if (val->val.u8 > 254) return ESP_ERR_INVALID_ARG;
      uint8_t percent = levelToPercent(val->val.u8);
      ESP_LOGI(TAG, "Matter pump level: %u -> %u%%", val->val.u8, percent);
      handlePumpChange(percent);
      return ESP_OK;
    }
  }

  return ESP_OK;
}

// ---------- Endpoint Creation ----------

static size_t boundedStringLength(const char *value, size_t maxLength) {
  return value ? strnlen(value, maxLength) : 0;
}

static void addBridgedDeviceMetadata(esp_matter::endpoint_t *endpoint, const char *label, const char *uniqueId) {
  esp_matter::cluster_t *cluster = esp_matter::cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
  if (!cluster) {
    ESP_LOGE(TAG, "Bridged metadata cluster missing for %s", label);
    return;
  }

  size_t labelLength = boundedStringLength(
      label, esp_matter::cluster::bridged_device_basic_information::k_max_node_label_length);
  size_t uniqueIdLength = boundedStringLength(
      uniqueId, esp_matter::cluster::bridged_device_basic_information::k_max_serial_number_length);

  using namespace esp_matter::cluster::bridged_device_basic_information::attribute;
  create_vendor_name(cluster, const_cast<char *>(MATTER_MANUFACTURER), strlen(MATTER_MANUFACTURER));
  create_vendor_id(cluster, 0xFFF1);
  create_product_name(cluster, const_cast<char *>(label), labelLength);
  create_product_id(cluster, 0x8000);
  create_node_label(cluster, const_cast<char *>(label), labelLength);
  create_hardware_version(cluster, 1);
  create_hardware_version_string(cluster, const_cast<char *>("1.0"), 3);
  create_software_version(cluster, 1);
  create_software_version_string(cluster, const_cast<char *>(FIRMWARE_VERSION), strlen(FIRMWARE_VERSION));
  create_product_label(cluster, const_cast<char *>(label), labelLength);
  create_serial_number(cluster, const_cast<char *>(uniqueId), uniqueIdLength);
}

template <typename Config>
static esp_matter::endpoint_t *createBridgedDevice(
    esp_matter::node_t *node,
    esp_matter::endpoint_t *aggregator,
    const char *label,
    const char *uniqueId,
    Config *deviceConfig,
    esp_err_t (*addDeviceType)(esp_matter::endpoint_t *, Config *))
{
  esp_matter::endpoint::bridged_node::config_t bridgedConfig;
  snprintf(bridgedConfig.bridged_device_basic_information.unique_id,
           sizeof(bridgedConfig.bridged_device_basic_information.unique_id), "%s", uniqueId);

  esp_matter::endpoint_t *endpoint = esp_matter::endpoint::bridged_node::create(
      node, &bridgedConfig, esp_matter::ENDPOINT_FLAG_BRIDGE, nullptr);
  if (!endpoint) {
    ESP_LOGE(TAG, "Failed to create bridged node for %s", label);
    return nullptr;
  }

  esp_err_t err = addDeviceType(endpoint, deviceConfig);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add device type for %s: %s", label, esp_err_to_name(err));
    return nullptr;
  }

  err = esp_matter::endpoint::set_parent_endpoint(endpoint, aggregator);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to parent %s to bridge: %s", label, esp_err_to_name(err));
    return nullptr;
  }

  addBridgedDeviceMetadata(endpoint, label, uniqueId);
  return endpoint;
}

static void makeBridgedUniqueId(char *output, size_t outputSize, const char *suffix) {
  snprintf(output, outputSize, "%.19s-%.12s", deviceId[0] ? deviceId : DEVICE_ID, suffix);
}

static const char *relayMatterLabel(uint8_t index) {
  return relayConfigs[index].name.empty() ? RELAY_LABELS[index] : relayConfigs[index].name.c_str();
}

static const char *sensorMatterLabel(SensorType type, const char *fallback) {
  for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
    if (sensorConfigs[i].enabled && sensorConfigs[i].type == type && !sensorConfigs[i].name.empty()) {
      return sensorConfigs[i].name.c_str();
    }
  }
  return fallback;
}

static void updateBridgedName(uint16_t endpointId, const char *name) {
  if (endpointId == 0 || !name) return;

  size_t length = boundedStringLength(
      name, esp_matter::cluster::bridged_device_basic_information::k_max_node_label_length);
  esp_matter_attr_val_t value = esp_matter_char_str(const_cast<char *>(name), length);

  const uint32_t attributes[] = {
    BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
    BridgedDeviceBasicInformation::Attributes::ProductName::Id,
    BridgedDeviceBasicInformation::Attributes::ProductLabel::Id,
  };
  for (uint32_t attributeId : attributes) {
    esp_err_t err = esp_matter::attribute::update(
        endpointId, BridgedDeviceBasicInformation::Id, attributeId, &value);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to update bridged name on endpoint %u: %s",
               endpointId, esp_err_to_name(err));
    }
  }
}

static void refreshBridgedNamesOnMatterThread(intptr_t) {
  updateBridgedName(pumpEndpointId, "Pool Pump");
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    updateBridgedName(relayEndpointIds[i], relayMatterLabel(i));
  }
  updateBridgedName(
      waterLevelSensorEndpointId, sensorMatterLabel(SensorType::WaterLevelSwitch, "Water Level"));
  updateBridgedName(flowSensorEndpointId, sensorMatterLabel(SensorType::FlowSwitch, "Water Flow"));
  updateBridgedName(
      temperatureSensorEndpointId, sensorMatterLabel(SensorType::TemperatureDs18b20, "Water Temperature"));
}

void refreshMatterBridgedNames() {
  if (pumpEndpointId == 0) return;
  CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(refreshBridgedNamesOnMatterThread, 0);
  if (err != CHIP_NO_ERROR) {
    ESP_LOGW(TAG, "Scheduling bridged name refresh failed: %" CHIP_ERROR_FORMAT, err.Format());
  }
}

void beginMatterEndpoints() {
  // Create node
  esp_matter::node::config_t nodeConfig;
  esp_matter::node_t *node = esp_matter::node::create(&nodeConfig, matterAttributeCallback, nullptr, nullptr);
  if (!node) {
    ESP_LOGE(TAG, "Failed to create Matter node");
    return;
  }

  // --- Bridge aggregator ---
  esp_matter::endpoint::aggregator::config_t aggregatorConfig;
  esp_matter::endpoint_t *aggregator = esp_matter::endpoint::aggregator::create(
      node, &aggregatorConfig, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (!aggregator) {
    ESP_LOGE(TAG, "Failed to create bridge aggregator");
    return;
  }
  ESP_LOGI(TAG, "Bridge aggregator endpoint: %u", esp_matter::endpoint::get_id(aggregator));

  char uniqueId[33];

  // --- Pump bridged device (Dimmable Plug-in Unit) ---
  esp_matter::endpoint::dimmable_plug_in_unit::config_t pumpConfig;
  pumpConfig.on_off.on_off = false;
  pumpConfig.level_control.current_level = nullable<uint8_t>(percentToLevel(0));
  pumpConfig.level_control.min_level = 1;
  pumpConfig.level_control.max_level = 254;
  pumpConfig.level_control.on_level = nullable<uint8_t>(percentToLevel(rpmToPercent(DEFAULT_RPM)));

  makeBridgedUniqueId(uniqueId, sizeof(uniqueId), "pump");
  esp_matter::endpoint_t *pumpEp = createBridgedDevice(
      node, aggregator, "Pool Pump", uniqueId, &pumpConfig,
      esp_matter::endpoint::dimmable_plug_in_unit::add);
  if (pumpEp) {
    pumpEndpointId = esp_matter::endpoint::get_id(pumpEp);
    ESP_LOGI(TAG, "Pump endpoint: %u", pumpEndpointId);
  } else {
    ESP_LOGE(TAG, "Failed to create pump endpoint");
  }

  // --- Relay bridged devices (On/Off Plug-in Units) ---
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    esp_matter::endpoint::on_off_plug_in_unit::config_t relayConfig;
    relayConfig.on_off.on_off = false;

    char suffix[16];
    snprintf(suffix, sizeof(suffix), "relay-%u", i + 1);
    makeBridgedUniqueId(uniqueId, sizeof(uniqueId), suffix);
    const char *label = relayMatterLabel(i);
    esp_matter::endpoint_t *ep = createBridgedDevice(
        node, aggregator, label, uniqueId, &relayConfig,
        esp_matter::endpoint::on_off_plug_in_unit::add);
    if (ep) {
      relayEndpointIds[i] = esp_matter::endpoint::get_id(ep);
      ESP_LOGI(TAG, "%s bridged endpoint: %u", label, relayEndpointIds[i]);
    } else {
      ESP_LOGE(TAG, "Failed to create %s endpoint", label);
    }
  }

  // --- Water level bridged device (Contact Sensor) ---
  esp_matter::endpoint::contact_sensor::config_t waterConfig;
  waterConfig.boolean_state.state_value = false;
  makeBridgedUniqueId(uniqueId, sizeof(uniqueId), "water-level");
  esp_matter::endpoint_t *waterEp = createBridgedDevice(
      node, aggregator, sensorMatterLabel(SensorType::WaterLevelSwitch, "Water Level"), uniqueId,
      &waterConfig, esp_matter::endpoint::contact_sensor::add);
  if (waterEp) {
    waterLevelSensorEndpointId = esp_matter::endpoint::get_id(waterEp);
    ESP_LOGI(TAG, "Water level sensor endpoint: %u", waterLevelSensorEndpointId);
  }

  // --- Flow bridged device (Contact Sensor) ---
  esp_matter::endpoint::contact_sensor::config_t flowConfig;
  flowConfig.boolean_state.state_value = false;
  makeBridgedUniqueId(uniqueId, sizeof(uniqueId), "flow");
  esp_matter::endpoint_t *flowEp = createBridgedDevice(
      node, aggregator, sensorMatterLabel(SensorType::FlowSwitch, "Water Flow"), uniqueId,
      &flowConfig, esp_matter::endpoint::contact_sensor::add);
  if (flowEp) {
    flowSensorEndpointId = esp_matter::endpoint::get_id(flowEp);
    ESP_LOGI(TAG, "Flow sensor endpoint: %u", flowSensorEndpointId);
  }

  // --- Temperature bridged device ---
  esp_matter::endpoint::temperature_sensor::config_t tempConfig;
  tempConfig.temperature_measurement.measured_value = (int16_t)0;
  tempConfig.temperature_measurement.min_measured_value = (int16_t)(-5500);   // -55.00 C
  tempConfig.temperature_measurement.max_measured_value = (int16_t)12500;   // 125.00 C
  makeBridgedUniqueId(uniqueId, sizeof(uniqueId), "temperature");
  esp_matter::endpoint_t *tempEp = createBridgedDevice(
      node, aggregator, sensorMatterLabel(SensorType::TemperatureDs18b20, "Water Temperature"), uniqueId,
      &tempConfig, esp_matter::endpoint::temperature_sensor::add);
  if (tempEp) {
    temperatureSensorEndpointId = esp_matter::endpoint::get_id(tempEp);
    ESP_LOGI(TAG, "Temperature sensor endpoint: %u", temperatureSensorEndpointId);
  }
}

// ---------- Metadata ----------

void configureMatterMetadata() {
  char uniqueId[65];
  snprintf(uniqueId, sizeof(uniqueId), "%s", deviceId[0] ? deviceId : DEVICE_ID);

  esp_matter_attr_val_t val;

  val = esp_matter_char_str(const_cast<char*>(MATTER_MANUFACTURER), strlen(MATTER_MANUFACTURER));
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::VendorName::Id, &val);

  val = esp_matter_uint16(0xFFF1);
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::VendorID::Id, &val);

  val = esp_matter_char_str(const_cast<char*>(MATTER_MODEL), strlen(MATTER_MODEL));
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::ProductName::Id, &val);

  val = esp_matter_uint16(0x8000);
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::ProductID::Id, &val);

  val = esp_matter_char_str(const_cast<char*>(MATTER_NODE_LABEL), strlen(MATTER_NODE_LABEL));
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::NodeLabel::Id, &val);

  val = esp_matter_char_str(const_cast<char*>(MATTER_MODEL), strlen(MATTER_MODEL));
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::ProductLabel::Id, &val);

  val = esp_matter_char_str(uniqueId, strlen(uniqueId));
  esp_matter::attribute::update(0, chip::app::Clusters::BasicInformation::Id,
      chip::app::Clusters::BasicInformation::Attributes::UniqueID::Id, &val);
}

// ---------- Matter Start ----------

void startMatter() {
  esp_err_t err = esp_matter::start(nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start Matter: %d", err);
  }
}

// ---------- Commissioning Output ----------

static chip::RendezvousInformationFlags matterRendezvousFlags() {
  chip::RendezvousInformationFlags flags;
  flags.Set(chip::RendezvousInformationFlag::kBLE);
  flags.Set(chip::RendezvousInformationFlag::kOnNetwork);
  return flags;
}

void printCommissioningInfo() {
  char qrPayload[128] = {0};
  chip::MutableCharSpan qrSpan(qrPayload);
  CHIP_ERROR qrErr = GetQRCode(qrSpan, matterRendezvousFlags());
  if (qrErr != CHIP_NO_ERROR) {
    ESP_LOGW(TAG, "Failed to generate QR code: %" CHIP_ERROR_FORMAT, qrErr.Format());
  }

  char manualCode[32] = {0};
  chip::MutableCharSpan manualSpan(manualCode);
  CHIP_ERROR manualErr = GetManualPairingCode(manualSpan, matterRendezvousFlags());
  if (manualErr != CHIP_NO_ERROR) {
    ESP_LOGW(TAG, "Failed to generate manual pairing code: %" CHIP_ERROR_FORMAT, manualErr.Format());
  }

  printf("\n");
  printf("Pool Conductor is not commissioned yet.\n");
  printf("Open the Home app, add accessory, choose Matter accessory, then scan/use:\n");
  printf("Manual pairing code: %s\n", manualCode);
  printf("QR payload for printed label: %s\n", qrPayload);
  printf("QR code URL: https://project-chip.github.io/connectedhomeip/qrcode.html?data=%s\n", qrPayload);
  printf("Setup PIN: %lu\n", BOARD_MATTER_SETUP_PIN);
  printf("Setup discriminator: 0x%03X\n", BOARD_MATTER_SETUP_DISCRIMINATOR);
}
