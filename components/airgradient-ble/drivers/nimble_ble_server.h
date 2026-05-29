/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef NIMBLE_BLE_SERVER_H
#define NIMBLE_BLE_SERVER_H

#include "hal/ble_server.h"

#include <NimBLEDevice.h>

#include <memory>
#include <vector>

// Bridges NimBLECharacteristicCallbacks::onWrite() to a AgBleWriteCallback.
// One instance is held per characteristic that has a write callback set.
class NimbleWriteBridge : public NimBLECharacteristicCallbacks {
public:
  explicit NimbleWriteBridge(AgBleWriteCallback callback);

  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override;

private:
  AgBleWriteCallback _callback;
};

// NimBLE-backed AgBleCharacteristic. Wraps a non-owning NimBLECharacteristic*
// managed by the NimBLE stack. Owns the write bridge if one is set.
class NimbleBleCharacteristic : public AgBleCharacteristic {
public:
  explicit NimbleBleCharacteristic(NimBLECharacteristic *characteristic);
  ~NimbleBleCharacteristic() override = default;

  bool set_value(const uint8_t *data, size_t len) override;
  bool notify() override;
  void set_write_callback(AgBleWriteCallback callback) override;

private:
  NimBLECharacteristic *_characteristic;
  std::unique_ptr<NimbleWriteBridge> _write_bridge;
};

// NimBLE-backed AgBleGattService. Wraps a non-owning NimBLEService* managed by
// the NimBLE stack. Owns all NimbleBleCharacteristic wrappers it creates.
//
// Note: in NimBLE v2 service start() has no effect; services are started
// when NimBLEServer::start() is called via AgBleServer::start_advertising().
class NimbleBleGattService : public AgBleGattService {
public:
  explicit NimbleBleGattService(NimBLEService *service);
  ~NimbleBleGattService() override = default;

  AgBleCharacteristic *add_characteristic(const char *uuid, uint16_t properties) override;
  bool start() override;

private:
  NimBLEService *_service;
  std::vector<std::unique_ptr<NimbleBleCharacteristic>> _characteristics;
};

// NimBLE-backed AgBleServer. Uses NimBLEDevice (global singleton) to manage
// the BLE stack. Privately inherits NimBLEServerCallbacks to bridge
// connect/disconnect events without exposing the NimBLE API.
//
// Owns all NimbleBleGattService wrappers it creates. The underlying NimBLE
// objects (NimBLEServer, NimBLEService, NimBLECharacteristic) are owned
// by NimBLEDevice and become invalid after deinit().
class NimbleBleServer : public AgBleServer, private NimBLEServerCallbacks {
public:
  NimbleBleServer() = default;
  ~NimbleBleServer() override;

  bool init(const char *device_name) override;
  void deinit() override;
  bool set_security(AgBleIoCapability io_cap, uint8_t auth_flags) override;
  bool delete_all_bonds() override;
  AgBleGattService *add_service(const char *uuid) override;
  bool set_advertising_name(const char *name) override;
  bool add_advertised_service_uuid(const char *uuid) override;
  bool set_manufacturer_data(const uint8_t *data, size_t len) override;
  bool start_advertising() override;
  bool stop_advertising() override;
  void set_connect_callback(AgBleConnectCallback callback) override;
  void set_disconnect_callback(AgBleDisconnectCallback callback) override;
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback callback) override;
  void set_auth_complete_callback(AgBleAuthCompleteCallback callback) override;

private:
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override;
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override;
  uint32_t onPassKeyDisplay() override;
  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override;

  NimBLEServer *_server{nullptr};
  std::vector<std::unique_ptr<NimbleBleGattService>> _services;
  AgBleConnectCallback _connect_callback;
  AgBleDisconnectCallback _disconnect_callback;
  AgBlePasskeyDisplayCallback _passkey_display_callback;
  AgBleAuthCompleteCallback _auth_complete_callback;
};

#endif // NIMBLE_BLE_SERVER_H
