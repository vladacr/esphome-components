#include "cometblue.h"
#include "cometblue_const.h"
#include "esphome/core/log.h"

#include "../esp32_ble_clients/esp32_ble.h"
#include "../esp32_ble_clients/esp32_ble_client.h"

using namespace esphome;

static const char *TAG = "cometblue_cmd";

static uint8_t temp_to_dev(const float &value) {
  if (value < COMETBLUEBT_MIN_TEMP)
    return uint8_t(COMETBLUEBT_MIN_TEMP * 2);
  else if (value > COMETBLUEBT_MAX_TEMP)
    return uint8_t(COMETBLUEBT_MAX_TEMP * 2);
  else
    return uint8_t(value * 2);
}

bool CometBlueClimate::with_connection(std::function<bool()> handler) {
  bool success = true;

  success = success && connect();
  success = success && handler();
  
  disconnect();

  return success;
}

bool CometBlueClimate::connect() {
  if (ble_client && ble_client->is_connected()) {
    return true;
  }

  std::unique_ptr<ESP32BLEClient> new_ble_client;

  new_ble_client.swap(ble_client);
  new_ble_client.reset(ESP32BLE::instance().acquire());

  if (!new_ble_client) {
    ESP_LOGW(TAG, "Cannot acquire client for %10llx.", address);
    return false;
  }

  new_ble_client->set_address(address);

  ESP_LOGV(TAG, "Connecting to %10llx...", address);
  
  if (!new_ble_client->connect()) {
    ESP_LOGW(TAG, "Cannot connect to %10llx.", address);
    return false;
  }

  if (!new_ble_client->request_services()) {
    new_ble_client.reset();
    ESP_LOGW(TAG, "Cannot request services from %10llx.", address);
    return false;
  }

  ESP_LOGV(TAG, "Connected to %10llx.", address);
  new_ble_client.swap(ble_client);
  return true;
}

void CometBlueClimate::disconnect() {
  if (!ble_client) {
    return;
  }

  ble_client.reset();
  ESP_LOGV(TAG, "Disconnected from %10llx.", address);
}

bool CometBlueClimate::send_command(void *command, uint16_t length, esp_bt_uuid_t uuid) {
  if (!ble_client) {
    return false;
  }

  uint16_t command_handle = ble_client->get_characteristic(
    PROP_SERVICE_UUID, uuid);
  if (!command_handle) {
    ESP_LOGE(TAG, "Cannot find command handle for %10llx.", address);
    return false;
  }

  bool result = ble_client->write(
    ESP32BLEClient::Characteristic,
    command_handle,
    command, length,
    true);

  if (result) {
    ESP_LOGV(TAG, "Sent of `%s` to %10llx to handle %04x.",
      hexencode((const uint8_t*)command, length).c_str(), address, command_handle);
  } else {
    ESP_LOGV(TAG, "Send of `%s` to %10llx to handle %04x: %d",
      hexencode((const uint8_t*)command, length).c_str(), address, command_handle, result);
  }

  return result;
}

bool CometBlueClimate::read_value(esp_bt_uuid_t uuid) {
  if (!ble_client) {
    return false;
  }

  uint16_t command_handle = ble_client->get_characteristic(
    PROP_SERVICE_UUID, uuid);
  if (!command_handle) {
    ESP_LOGE(TAG, "Cannot find command handle for %10llx.", address);
    return false;
  }

  bool result = ble_client->read(command_handle);
  ESP_LOGV(TAG, "read from %10llx from handle %04x with result %d.", address, command_handle, result);

  return result;
}

bool CometBlueClimate::send_pincode() {
  if (pin != -1) {
    uint8_t pin_encoded[4] = {(uint8_t)(pin & 0xFF), (uint8_t)((pin >> 8) & 0xFF), (uint8_t)((pin >> 16) & 0xFF), (uint8_t)((pin >> 24) & 0xFF) };
    return send_command(pin_encoded, sizeof(pin_encoded), PROP_PIN_CHARACTERISTIC_UUID);
  }
  return true;
}

bool CometBlueClimate::get_time() {

  return read_value(PROP_TIME_CHARACTERISTIC_UUID);
}

bool CometBlueClimate::get_flags() {
  if (read_value(PROP_FLAGS_CHARACTERISTIC_UUID)) {
    uint32_t status = (((uint32_t)ble_client->readresult_value[2]) << 16) | (((uint32_t)ble_client->readresult_value[1]) << 8) | ((uint32_t)ble_client->readresult_value[0]);
    if (status && 1==1) {
      mode = climate::CLIMATE_MODE_HEAT;
    } else {
      mode = climate::CLIMATE_MODE_AUTO;
    }

    return true;
  }
  return false;
}

bool CometBlueClimate::get_temperature() {
  bool result = read_value(PROP_TEMPERATURE_CHARACTERISTIC_UUID);
  if (result) {
    current_temperature = ((float)ble_client->readresult_value[0]) / 2.0f; 
  }
  
  return result;
}



bool CometBlueClimate::query_state() {

  send_pincode();

  get_flags();

  get_temperature();

  return true;
}

bool CometBlueClimate::set_temperature(float temperature) {
  target_temperature = temperature;
  if (mode != climate::ClimateMode::CLIMATE_MODE_OFF) {
    uint8_t command[] = {
        0x80, uint8_t(temperature*2.0f), 0x80, 0x80, uint8_t(temperature_offset*2.0f), window_open_sensitivity, window_open_minutes
    };

    return send_command(command, sizeof(command), PROP_TEMPERATURE_CHARACTERISTIC_UUID);
  }
  return true;
}

bool CometBlueClimate::set_auto_mode() {
  return true;
}

bool CometBlueClimate::set_manual_mode() {
  if (read_value(PROP_FLAGS_CHARACTERISTIC_UUID)) {
    uint32_t status = (((uint32_t)ble_client->readresult_value[2]) << 16) | (((uint32_t)ble_client->readresult_value[1]) << 8) | ((uint32_t)ble_client->readresult_value[0]);
    status |=1; // Set manual mode flag    
    uint8_t statusencoded[3] = {(uint8_t)(status & 0xFF), (uint8_t)((status >> 8) & 0xFF), (uint8_t)((status >> 16) & 0xFF)};
    send_command(statusencoded, sizeof(statusencoded), PROP_FLAGS_CHARACTERISTIC_UUID);
  
    uint8_t command[] = {
        0x80, uint8_t(target_temperature*2.0f), 0x80, 0x80, uint8_t(temperature_offset*2.0f), window_open_sensitivity, window_open_minutes
    };
    send_command(command, sizeof(command), PROP_TEMPERATURE_CHARACTERISTIC_UUID);

    mode = climate::ClimateMode::CLIMATE_MODE_HEAT;
    return true;
  }
  
  return false;

}

bool CometBlueClimate::set_off_mode() {  
    if (read_value(PROP_FLAGS_CHARACTERISTIC_UUID)) {
    uint32_t status = (((uint32_t)ble_client->readresult_value[2]) << 16) | (((uint32_t)ble_client->readresult_value[1]) << 8) | ((uint32_t)ble_client->readresult_value[0]);
    status |=1; // Set manual mode flag
    uint8_t statusencoded[3] = {(uint8_t)(status & 0xFF), (uint8_t)((status >> 8) & 0xFF), (uint8_t)((status >> 16) & 0xFF)};
    send_command(statusencoded, sizeof(statusencoded), PROP_FLAGS_CHARACTERISTIC_UUID);

    uint8_t command[] = {
        0x80, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80
    };
    send_command(command, sizeof(command), PROP_TEMPERATURE_CHARACTERISTIC_UUID);

    mode = climate::ClimateMode::CLIMATE_MODE_OFF;

    return true;
  }
  
  return false;
}

