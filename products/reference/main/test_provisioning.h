#ifndef TEST_PROVISIONING_H
#define TEST_PROVISIONING_H

// Smoke test for airgradient-provisioning (checkpoint 1, Wi-Fi portal).
//
// Brings up WifiManager + IdfHttpServer + ProvisioningManager, starts
// provisioning, and logs every lifecycle event. The device:
//   1. Comes up as soft-AP "airgradient-prov" (open by default — override
//      via TEST_PROVISIONING_AP_PASSWORD to set a WPA2 password).
//   2. Serves the captive portal at http://192.168.4.1/.
//   3. Accepts credentials from the portal, attempts STA connection,
//      and logs success/failure.
//   4. On Connected, calls send_ble_status() to exercise the API (no-op
//      until checkpoint 2 wires the BLE transport) and then stop()s.
//   5. Idles after stop() so the device stays attached for log
//      inspection.
//
// Connect from a phone:
//   1. Join Wi-Fi network "airgradient-prov" (open).
//   2. Captive portal pops up automatically (DNS redirects everywhere).
//      If not, open http://192.168.4.1/ manually.
//   3. Pick your home Wi-Fi, enter the password, submit.
void run_test_provisioning();

#endif // TEST_PROVISIONING_H
