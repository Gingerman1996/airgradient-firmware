/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_TIMER_H
#define AG_PROVISIONING_TIMER_H

#include <cstdint>
#include <functional>

// One-shot timer abstraction owned by ProvisioningManager.
//
// On firmware the implementation wraps esp_timer. Under TEST_HOST the
// timer is a no-op skeleton with a public fire_for_test() hook so the
// inactivity-timeout state-machine path stays host-testable without a
// real clock source.
class ProvisioningTimer {
public:
  using Callback = std::function<void()>;

  ProvisioningTimer();
  ~ProvisioningTimer();

  ProvisioningTimer(const ProvisioningTimer &) = delete;
  ProvisioningTimer &operator=(const ProvisioningTimer &) = delete;

  // Register the expiry callback. Must be set before arm().
  void set_callback(Callback cb);

  // Arm a one-shot timer that fires `cb` after `delay_ms`. Re-arming
  // replaces any pending expiry.
  bool arm(uint32_t delay_ms);

  // Cancel a pending expiry. Safe to call when nothing is armed.
  void cancel();

  bool is_armed() const { return _armed; }

#ifdef TEST_HOST
  // Synchronously invoke the callback as if the timer expired. Test
  // hook — not present on firmware builds.
  void fire_for_test();
#endif

private:
  Callback _cb;
  void *_handle = nullptr; // esp_timer_handle_t on hardware
  bool _armed = false;
};

#endif // AG_PROVISIONING_TIMER_H
