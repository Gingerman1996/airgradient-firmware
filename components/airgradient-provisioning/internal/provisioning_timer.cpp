/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "provisioning_timer.h"

#include "ag_log.h"

#ifndef TEST_HOST

#include "esp_timer.h"

namespace {

constexpr const char *TAG = "ProvTimer";

// esp_timer dispatches on its own task; the arg is the address of the
// owning ProvisioningTimer's _cb std::function. ProvisioningManager
// serialises access via its mutex.
void expiry_trampoline(void *arg) {
  auto *cb = static_cast<ProvisioningTimer::Callback *>(arg);
  if (cb != nullptr && *cb) {
    (*cb)();
  }
}

} // namespace

ProvisioningTimer::ProvisioningTimer() {
  esp_timer_create_args_t args = {};
  args.callback = &expiry_trampoline;
  args.arg = &_cb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "ag_prov";
  esp_timer_handle_t handle = nullptr;
  if (esp_timer_create(&args, &handle) != ESP_OK) {
    AG_LOGE(TAG, "esp_timer_create failed");
    handle = nullptr;
  }
  _handle = handle;
}

ProvisioningTimer::~ProvisioningTimer() {
  cancel();
  if (_handle != nullptr) {
    esp_timer_delete(static_cast<esp_timer_handle_t>(_handle));
    _handle = nullptr;
  }
}

void ProvisioningTimer::set_callback(Callback cb) { _cb = std::move(cb); }

bool ProvisioningTimer::arm(uint32_t delay_ms) {
  if (_handle == nullptr) {
    return false;
  }
  if (_armed) {
    esp_timer_stop(static_cast<esp_timer_handle_t>(_handle));
    _armed = false;
  }
  if (esp_timer_start_once(static_cast<esp_timer_handle_t>(_handle),
                           static_cast<uint64_t>(delay_ms) * 1000ULL) != ESP_OK) {
    AG_LOGE(TAG, "esp_timer_start_once failed");
    return false;
  }
  _armed = true;
  return true;
}

void ProvisioningTimer::cancel() {
  if (_handle != nullptr && _armed) {
    esp_timer_stop(static_cast<esp_timer_handle_t>(_handle));
  }
  _armed = false;
}

#else // TEST_HOST

ProvisioningTimer::ProvisioningTimer() = default;
ProvisioningTimer::~ProvisioningTimer() = default;

void ProvisioningTimer::set_callback(Callback cb) { _cb = std::move(cb); }

bool ProvisioningTimer::arm(uint32_t delay_ms) {
  (void)delay_ms;
  _armed = true;
  return true;
}

void ProvisioningTimer::cancel() { _armed = false; }

void ProvisioningTimer::fire_for_test() {
  if (!_armed) {
    return;
  }
  _armed = false;
  if (_cb) {
    _cb();
  }
}

#endif // TEST_HOST
