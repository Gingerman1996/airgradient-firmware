#include "sdkconfig.h"

#include "test_ble.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED

// Uncomment to enable passkey-entry security (Display Only + MITM + bonding).
// When commented out the test runs without security (open access).
#define TEST_BLE_SECURITY

#include "esp_log.h"

#include <cinttypes>

#include "hal/ble_server.h"
#include "nimble_ble_server.h"
#include "rtos.h"

static constexpr const char *TAG = "test_ble";

// Notify interval in milliseconds.
static constexpr uint32_t NOTIFY_INTERVAL_MS = 2000;

// Hardcoded read/notify value: ASCII "AG\0".
static constexpr uint8_t READ_VALUE[] = {'A', 'G', '\0'};

// Fallback advertised device name when the configured product name is empty.
static constexpr const char *DEFAULT_BLE_DEVICE_NAME = "GO";

// Test GATT service and characteristic UUIDs (128-bit).
static constexpr const char *TEST_SVC_UUID = "0000ABCD-0000-1000-8000-00805F9B34FB";
static constexpr const char *TEST_CHR_UUID = "00001234-0000-1000-8000-00805F9B34FB";

void run_test_ble(const char *device_name) {
  ESP_LOGI(TAG, "--- BLE test start (runs indefinitely) ---");

  const char *advertising_name = DEFAULT_BLE_DEVICE_NAME;
  if (device_name != nullptr && device_name[0] != '\0') {
    advertising_name = device_name;
  }

  NimbleBleServer ble;

  if (!ble.init(advertising_name)) {
    ESP_LOGE(TAG, "BLE init failed");
    return;
  }

#ifdef TEST_BLE_SECURITY
  if (!ble.set_security(AgBleIoCapability::DISPLAY_ONLY, AgBleAuth::BOND | AgBleAuth::MITM)) {
    ESP_LOGE(TAG, "set_security failed");
    ble.deinit();
    return;
  }
  ESP_LOGI(TAG, "security enabled: Display Only + MITM + bonding");
#endif

  AgBleGattService *svc = ble.add_service(TEST_SVC_UUID);
  if (svc == nullptr) {
    ESP_LOGE(TAG, "add_service failed");
    ble.deinit();
    return;
  }

  // Base properties; security build adds authentication requirements.
  constexpr uint16_t chr_props = AgBleProperty::READ | AgBleProperty::WRITE | AgBleProperty::NOTIFY
#ifdef TEST_BLE_SECURITY
                                 | AgBleProperty::READ_AUTHEN | AgBleProperty::WRITE_AUTHEN
#endif
      ;

  AgBleCharacteristic *ch = svc->add_characteristic(TEST_CHR_UUID, chr_props);
  if (ch == nullptr) {
    ESP_LOGE(TAG, "add_characteristic failed");
    ble.deinit();
    return;
  }

  // Seed the characteristic with the hardcoded read value.
  ch->set_value(READ_VALUE, sizeof(READ_VALUE));

  // Print any value written by a client.
  ch->set_write_callback([](const uint8_t *data, size_t len) {
    ESP_LOGI(TAG, "write received (%u byte%s):", static_cast<unsigned>(len), len == 1 ? "" : "s");
    for (size_t i = 0; i < len; i++) {
      ESP_LOGI(TAG, "  [%u] 0x%02X  '%c'", static_cast<unsigned>(i), data[i],
               (data[i] >= 0x20 && data[i] < 0x7F) ? static_cast<char>(data[i]) : '.');
    }
  });

  if (!svc->start()) {
    ESP_LOGE(TAG, "service start failed");
    ble.deinit();
    return;
  }

  if (!ble.set_advertising_name(advertising_name)) {
    ESP_LOGE(TAG, "set_advertising_name failed");
    ble.deinit();
    return;
  }

  if (!ble.add_advertised_service_uuid(TEST_SVC_UUID)) {
    ESP_LOGE(TAG, "add_advertised_service_uuid failed");
    ble.deinit();
    return;
  }

#ifdef TEST_BLE_SECURITY
  ble.set_passkey_display_callback(
      [](uint32_t passkey) { ESP_LOGI(TAG, "*** PASSKEY: %06" PRIu32 " ***", passkey); });
  ble.set_auth_complete_callback([](uint16_t handle, bool success) {
    ESP_LOGI(TAG, "auth %s: handle=%u", success ? "OK" : "FAILED", handle);
  });
#endif

  ble.set_connect_callback([&ble](uint16_t handle) {
    ESP_LOGI(TAG, "client connected: handle=%u — stopping advertising", handle);
    ble.stop_advertising();
  });
  ble.set_disconnect_callback([&ble](uint16_t handle, int reason) {
    ESP_LOGI(TAG, "client disconnected: handle=%u reason=%d — restarting advertising", handle,
             reason);
    ble.start_advertising();
  });

  if (!ble.start_advertising()) {
    ESP_LOGE(TAG, "start_advertising failed");
    ble.deinit();
    return;
  }

  ESP_LOGI(TAG,
           "advertising as '%s' (svc=%s chr=%s) — connect with a BLE "
           "scanner to verify",
           advertising_name, TEST_SVC_UUID, TEST_CHR_UUID);
  ESP_LOGI(TAG, "notifying every %u ms with hardcoded value",
           static_cast<unsigned>(NOTIFY_INTERVAL_MS));

  // Run indefinitely: notify subscribed clients on each interval, keep the
  // read value fixed so any read returns the same hardcoded bytes.
  while (true) {
    RTOS::delay_ms(NOTIFY_INTERVAL_MS);
    ch->set_value(READ_VALUE, sizeof(READ_VALUE));
    ch->notify();
  }
}

#endif // CONFIG_BT_NIMBLE_ENABLED
