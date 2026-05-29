/**
 * AirGradient Go — BLE Service implementation
 *
 * GATT setup, CBOR encoding/decoding, NimBLE callbacks, and binary
 * history streaming.  Only init() is guarded with #ifndef TEST_HOST
 * (it instantiates the concrete NimbleBleServer). All other methods
 * use the abstract BleServer* interface and compile under host tests.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ble.h"

#include "ag_log.h"
#include "common.h"
#include "go_ble_protocol.h"
#include "go_events.h"
#include "go_storage.h"

#ifndef TEST_HOST
#include "sdkconfig.h"
#include "nimble_ble_server.h"
#endif
#include "rtos.h"

#include <cbor.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <vector>

static constexpr const char *TAG = "BLE";

// Forward declaration — defined in the CBOR decode helpers section.
static const char *ble_command_to_str(BleCommand cmd);

// ---------------------------------------------------------------------------
// GATT UUIDs
// ---------------------------------------------------------------------------

static constexpr const char *SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *MEASURES_CHAR_UUID = "d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *STATUS_CHAR_UUID = "d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *CONFIG_CHAR_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *HISTORY_CHAR_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Advertised name prefix. Final name is "AGo-<serial>" (e.g. "AGo-AABBCCDDEEFF").
static constexpr const char *ADV_NAME_PREFIX = "AGo-";
static constexpr size_t ADV_NAME_MAX_LEN = 20;

/// Minimum negotiated MTU below which notifications are suppressed.
static constexpr size_t MIN_USEFUL_MTU = 128;

/// CBOR encoding buffer size — fits all characteristic payloads.
static constexpr size_t CBOR_BUF_SIZE = 256;

/// RoutePointWire: packed binary format, 55 bytes per point.
static constexpr size_t ROUTE_POINT_WIRE_SIZE = 56;

/// History notification tag bytes.
static constexpr uint8_t HISTORY_TAG_CBOR = 0x00;
static constexpr uint8_t HISTORY_TAG_BINARY = 0x01;

/// Binary data notification header: tag (1) + point_index (2) = 3 bytes.
static constexpr size_t BINARY_HEADER_SIZE = 3;

/// Maximum ATT payload for a single notification (assumes 247-byte MTU).
/// Conservative estimate; actual limit depends on negotiated MTU.
static constexpr size_t MAX_NOTIFY_PAYLOAD = 244;

/// Maximum route points per binary notification.
static constexpr size_t POINTS_PER_NOTIFICATION =
    (MAX_NOTIFY_PAYLOAD - BINARY_HEADER_SIZE) / ROUTE_POINT_WIRE_SIZE;

/// Read batch size for route points from storage.
static constexpr uint16_t ROUTE_READ_BATCH = 4;

/// Backpressure retry delay when notify() returns false.
static constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 1;

/// Sessions per page in paginated list response.
static constexpr uint16_t SESSIONS_PER_PAGE = 6;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

bool BleService::security_enabled() {
#if defined(CONFIG_AGO_BLE_SECURITY_ENABLED) && CONFIG_AGO_BLE_SECURITY_ENABLED
  return true;
#else
  return false;
#endif
}

uint16_t BleService::measures_properties() {
  uint16_t props = AgBleProperty::READ | AgBleProperty::NOTIFY;
  if (security_enabled()) {
    props |= AgBleProperty::READ_AUTHEN;
  }
  return props;
}

uint16_t BleService::status_properties() {
  uint16_t props = AgBleProperty::READ;
  if (security_enabled()) {
    props |= AgBleProperty::READ_AUTHEN;
  }
  return props;
}

uint16_t BleService::config_properties() {
  uint16_t props = AgBleProperty::READ | AgBleProperty::WRITE | AgBleProperty::NOTIFY;
  if (security_enabled()) {
    props |= AgBleProperty::READ_AUTHEN | AgBleProperty::WRITE_AUTHEN;
  }
  return props;
}

uint16_t BleService::history_properties() {
  uint16_t props = AgBleProperty::WRITE | AgBleProperty::NOTIFY;
  if (security_enabled()) {
    props |= AgBleProperty::WRITE_AUTHEN;
  }
  return props;
}

BleService::BleService(RtosQueueHandle event_queue, StorageService &storage)
    : _event_queue(event_queue), _storage(storage) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

#ifndef TEST_HOST

bool BleService::init(const char *serial) {
  if (_server != nullptr) {
    AG_LOGW(TAG, "already initialized");
    return true;
  }

  // Build advertised name: "AGo-AABBCCDDEEFF" (full serial)
  char adv_name[ADV_NAME_MAX_LEN] = {};
  snprintf(adv_name, sizeof(adv_name), "%s%s", ADV_NAME_PREFIX, serial ? serial : "000000");

  // Create NimBLE server (static — lives until deinit)
  static NimbleBleServer ble_server;
  _server = &ble_server;

  if (!_server->init(adv_name)) {
    AG_LOGE(TAG, "BLE stack init failed");
    _server = nullptr;
    return false;
  }

  // --- Security: Passkey Entry (Display Only) ---
  if (security_enabled()) {
    if (!_server->set_security(AgBleIoCapability::DISPLAY_ONLY,
                               AgBleAuth::BOND | AgBleAuth::MITM)) {
      AG_LOGE(TAG, "set_security failed");
      _server->deinit();
      _server = nullptr;
      return false;
    }
  } else {
    AG_LOGW(TAG, "BLE security disabled");
  }

  // --- GATT Service ---
  auto *svc = _server->add_service(SERVICE_UUID);
  if (svc == nullptr) {
    AG_LOGE(TAG, "add_service failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Characteristics ---

  // Measures: Read + Notify, optionally authenticated to force pairing before subscribe/read.
  _measures_char = svc->add_characteristic(MEASURES_CHAR_UUID, measures_properties());
  if (_measures_char == nullptr) {
    AG_LOGE(TAG, "add Measures characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // Status: Read only, optionally authenticated
  _status_char = svc->add_characteristic(STATUS_CHAR_UUID, status_properties());
  if (_status_char == nullptr) {
    AG_LOGE(TAG, "add Status characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // Config: Read + Write + Notify, optionally authenticated
  _config_char = svc->add_characteristic(CONFIG_CHAR_UUID, config_properties());
  if (_config_char == nullptr) {
    AG_LOGE(TAG, "add Config characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // History: Write + Notify, optionally authenticated
  _history_char = svc->add_characteristic(HISTORY_CHAR_UUID, history_properties());
  if (_history_char == nullptr) {
    AG_LOGE(TAG, "add History characteristic failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Write callbacks ---
  _config_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_config_write(data, len); });
  _history_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_history_write(data, len); });

  // --- Start GATT service ---
  if (!svc->start()) {
    AG_LOGE(TAG, "service start failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  // --- Callbacks ---
  _server->set_connect_callback([this](uint16_t conn_handle) { on_connect(conn_handle); });
  _server->set_disconnect_callback(
      [this](uint16_t conn_handle, int reason) { on_disconnect(conn_handle, reason); });
  if (security_enabled()) {
    _server->set_passkey_display_callback(
        [this](uint32_t passkey) { on_passkey_request(passkey); });
    _server->set_auth_complete_callback([this](uint16_t /*conn_handle*/, bool success) {
      AG_LOGI(TAG, "auth %s", success ? "OK" : "FAILED");
      Event evt{};
      evt.type = EventType::BleAuthComplete;
      RTOS::queue_send(_event_queue, &evt);
    });
  }

  // --- Advertising ---
  if (!_server->set_advertising_name(adv_name)) {
    AG_LOGE(TAG, "set_advertising_name failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  if (!_server->add_advertised_service_uuid(SERVICE_UUID)) {
    AG_LOGE(TAG, "add_advertised_service_uuid failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  if (!_server->start_advertising()) {
    AG_LOGE(TAG, "start_advertising failed");
    _server->deinit();
    _server = nullptr;
    return false;
  }

  AG_LOGI(TAG, "initialized, advertising as '%s'", adv_name);
  return true;
}

#endif // TEST_HOST

void BleService::deinit() {
  if (_server == nullptr) {
    return;
  }

  _connected.store(false);
  _export_active = false;
  _export_session_id = 0;

  _server->deinit();
  _server = nullptr;
  _measures_char = nullptr;
  _status_char = nullptr;
  _config_char = nullptr;
  _history_char = nullptr;

  AG_LOGI(TAG, "deinitialized");
}

bool BleService::delete_all_bonds() {
  if (_server == nullptr) {
    return true;
  }

  const bool ok = _server->delete_all_bonds();
  if (!ok) {
    AG_LOGW(TAG, "delete_all_bonds failed");
  }

  return ok;
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool BleService::is_initialized() const { return _server != nullptr; }

bool BleService::is_connected() const { return _connected.load(); }

// ---------------------------------------------------------------------------
// NimBLE callbacks (run in NimBLE task context)
// ---------------------------------------------------------------------------

void BleService::on_connect(uint16_t conn_handle) {
  AG_LOGI(TAG, "client connected: handle=%u", conn_handle);
  _connected.store(true);

  // Stop advertising — single connection device
  if (_server != nullptr) {
    _server->stop_advertising();
  }

  Event evt{};
  evt.type = EventType::BleConnected;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_disconnect(uint16_t conn_handle, int reason) {
  AG_LOGI(TAG, "client disconnected: handle=%u reason=%d", conn_handle, reason);
  _connected.store(false);

  // Clean up active history export
  _export_active = false;
  _export_session_id = 0;

  // Restart advertising
  if (_server != nullptr) {
    _server->start_advertising();
  }

  Event evt{};
  evt.type = EventType::BleDisconnected;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_config_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "config write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _config_write_mutex.lock();
  memcpy(_config_write_buf, data, len);
  _config_write_len = len;
  _config_write_pending = true;
  _config_write_mutex.unlock();

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_history_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "history write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _history_write_mutex.lock();
  memcpy(_history_write_buf, data, len);
  _history_write_len = len;
  _history_write_pending = true;
  _history_write_mutex.unlock();

  Event evt{};
  evt.type = EventType::BleHistoryWrite;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_passkey_request(uint32_t passkey) {
  AG_LOGI(TAG, "passkey display: %06" PRIu32, passkey);

  Event evt{};
  evt.type = EventType::BlePairingRequest;
  evt.ble_passkey = passkey;
  RTOS::queue_send(_event_queue, &evt);
}

// ---------------------------------------------------------------------------
// Pending write retrieval (called by orchestrator task)
// ---------------------------------------------------------------------------

size_t BleService::take_pending_config_write(uint8_t *buf, size_t buf_size) {
  _config_write_mutex.lock();
  if (!_config_write_pending) {
    _config_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_config_write_len, buf_size);
  memcpy(buf, _config_write_buf, len);
  _config_write_pending = false;
  _config_write_len = 0;
  _config_write_mutex.unlock();
  return len;
}

size_t BleService::take_pending_history_write(uint8_t *buf, size_t buf_size) {
  _history_write_mutex.lock();
  if (!_history_write_pending) {
    _history_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_history_write_len, buf_size);
  memcpy(buf, _history_write_buf, len);
  _history_write_pending = false;
  _history_write_len = 0;
  _history_write_mutex.unlock();
  return len;
}

// ---------------------------------------------------------------------------
// Data output: Measures
// ---------------------------------------------------------------------------

void BleService::notify_measures(const MeasuresAGo &measures, const GpsData &gps,
                                 time_t timestamp) {
  if (_measures_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_measures(buf, sizeof(buf), measures, gps, timestamp);
  if (len == 0) {
    AG_LOGW(TAG, "measures encode failed");
    return;
  }

  _measures_char->set_value(buf, len);

  if (_connected.load()) {
    _measures_char->notify();
  }
}

// ---------------------------------------------------------------------------
// Data output: Status
// ---------------------------------------------------------------------------

void BleService::update_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                               uint32_t session_id) {
  if (_status_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_status(buf, sizeof(buf), power, gps, tracking_active, session_id);
  if (len == 0) {
    AG_LOGW(TAG, "status encode failed");
    return;
  }

  _status_char->set_value(buf, len);
}

// ---------------------------------------------------------------------------
// Data output: Config
// ---------------------------------------------------------------------------

void BleService::update_config(const GoSettings &settings) {
  if (_config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_config(buf, sizeof(buf), settings);
  if (len == 0) {
    AG_LOGW(TAG, "config encode failed");
    return;
  }

  _config_char->set_value(buf, len);
}

void BleService::notify_config(const GoSettings &settings) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  // Config notification includes "type": "config" discriminator plus full config
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  // 9 config keys + 1 type discriminator = 10
  cbor_encoder_create_map(&encoder, &map, 10);

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CONFIG);

  cbor_encode_text_stringz(&map, BLE_KEY_MEAS_INT);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.measure_interval_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_TEMP_F);
  cbor_encode_boolean(&map, settings.use_fahrenheit);

  cbor_encode_text_stringz(&map, BLE_KEY_PM_AQI);
  cbor_encode_boolean(&map, settings.pm_use_usaqi);

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_INT);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.gps_interval_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_MODE);
  cbor_encode_text_stringz(&map, gps_mode_to_str(settings.gps_mode));

  cbor_encode_text_stringz(&map, BLE_KEY_INACT_TO);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.inactivity_timeout_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_AUTO_LOCK);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.auto_lock_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_DEV_NAME);
  cbor_encode_text_stringz(&map, settings.device_name.c_str());

  cbor_encode_text_stringz(&map, BLE_KEY_OP_MODE);
  cbor_encode_text_stringz(&map, operating_mode_to_str(settings.operating_mode));

  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  _config_char->set_value(buf, len);
  _config_char->notify();
}

void BleService::notify_command_result(BleCommand cmd, bool success, const char *error) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  size_t map_len = 3; // type, cmd, ok
  if (!success && error != nullptr) {
    map_len = 4; // + err
  }
  cbor_encoder_create_map(&encoder, &map, map_len);

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CMD_RESULT);

  cbor_encode_text_stringz(&map, BLE_KEY_CMD);
  cbor_encode_text_stringz(&map, ble_command_to_str(cmd));

  cbor_encode_text_stringz(&map, BLE_KEY_OK);
  cbor_encode_boolean(&map, success);

  if (!success && error != nullptr) {
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, error);
  }

  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  _config_char->set_value(buf, len);
  _config_char->notify();
}

void BleService::notify_command_progress(BleCommand cmd) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CMD_PROGRESS);

  cbor_encode_text_stringz(&map, BLE_KEY_CMD);
  cbor_encode_text_stringz(&map, ble_command_to_str(cmd));

  cbor_encoder_close_container(&encoder, &map);

  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  _config_char->set_value(buf, len);
  _config_char->notify();
}

// ---------------------------------------------------------------------------
// History: session list
// ---------------------------------------------------------------------------

void BleService::handle_history_list() {
  if (_history_char == nullptr) {
    return;
  }

  // Read session list from storage (heap-allocated, no hard cap)
  uint16_t session_count = _storage.session_count();
  std::vector<uint32_t> session_ids(session_count);
  if (session_count > 0) {
    session_count = _storage.list_sessions(session_ids.data(), session_count);
  }

  // Paginate: send one CBOR notification per page
  uint16_t total_pages =
      session_count > 0 ? (session_count + SESSIONS_PER_PAGE - 1) / SESSIONS_PER_PAGE : 1;

  for (uint16_t page = 0; page < total_pages && _connected.load(); page++) {
    uint16_t start = page * SESSIONS_PER_PAGE;
    uint16_t page_count = std::min(static_cast<uint16_t>(SESSIONS_PER_PAGE),
                                   static_cast<uint16_t>(session_count - start));

    // Encode: {"type":"sessions","sessions":[...],"pg":N,"tpg":N,"cnt":N}
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 5);

    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_SESSIONS);

    cbor_encode_text_stringz(&map, BLE_KEY_SESSIONS);
    CborEncoder arr;
    cbor_encoder_create_array(&map, &arr, page_count);

    for (uint16_t i = 0; i < page_count; i++) {
      uint16_t idx = start + i;
      CborEncoder sess_map;
      cbor_encoder_create_map(&arr, &sess_map, 3);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_ID);
      cbor_encode_uint(&sess_map, session_ids[idx]);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_PTS);
      uint32_t pts = _storage.get_session_point_count(session_ids[idx]);
      cbor_encode_uint(&sess_map, pts);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_TS);
      time_t start_ts = _storage.get_session_start_time(session_ids[idx]);
      cbor_encode_uint(&sess_map, static_cast<uint64_t>(start_ts));

      cbor_encoder_close_container(&arr, &sess_map);
    }

    cbor_encoder_close_container(&map, &arr);

    cbor_encode_text_stringz(&map, BLE_KEY_PG);
    cbor_encode_uint(&map, static_cast<uint64_t>(page + 1));

    cbor_encode_text_stringz(&map, BLE_KEY_TPG);
    cbor_encode_uint(&map, static_cast<uint64_t>(total_pages));

    cbor_encode_text_stringz(&map, BLE_KEY_CNT);
    cbor_encode_uint(&map, static_cast<uint64_t>(session_count));

    cbor_encoder_close_container(&encoder, &map);

    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }
}

// ---------------------------------------------------------------------------
// History: start download
// ---------------------------------------------------------------------------

void BleService::handle_history_start(uint32_t session_id) {
  if (_history_char == nullptr) {
    return;
  }

  // End any active download first
  if (_export_active) {
    _export_active = false;
  }

  // Check if session exists
  uint32_t total_points = _storage.get_session_point_count(session_id);
  if (total_points == 0) {
    // Send error: session not found
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, BLE_VAL_ERR_SESSION_NOT_FOUND);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  _export_session_id = session_id;
  _export_active = true;

  // Send "started" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 4);

    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_STARTED);

    cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
    cbor_encode_uint(&map, session_id);

    cbor_encode_text_stringz(&map, BLE_KEY_TOTAL);
    cbor_encode_uint(&map, total_points);

    cbor_encode_text_stringz(&map, BLE_KEY_PT_SIZE);
    cbor_encode_uint(&map, ROUTE_POINT_WIRE_SIZE);

    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }

  // Stream all points as binary notifications
  uint32_t sent = 0;
  RoutePoint batch[ROUTE_READ_BATCH];

  for (uint32_t offset = 0; offset < total_points && _connected.load(); /* incremented below */) {
    uint16_t to_read = static_cast<uint16_t>(
        std::min(static_cast<uint32_t>(ROUTE_READ_BATCH), total_points - offset));
    uint16_t actually_read = _storage.read_route_points(session_id, offset, batch, to_read);

    if (actually_read == 0) {
      // Flash read error — abort stream
      AG_LOGE(TAG, "flash read error at offset %" PRIu32, offset);
      uint8_t buf[CBOR_BUF_SIZE];
      CborEncoder encoder;
      cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
      CborEncoder map;
      cbor_encoder_create_map(&encoder, &map, 2);
      cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
      cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
      cbor_encode_text_stringz(&map, BLE_KEY_ERR);
      cbor_encode_text_stringz(&map, BLE_VAL_ERR_FLASH_ERROR);
      cbor_encoder_close_container(&encoder, &map);
      size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
      send_history_cbor(buf, len);
      _export_active = false;
      return;
    }

    // Convert to wire format and send
    uint8_t wire_buf[ROUTE_READ_BATCH * ROUTE_POINT_WIRE_SIZE];
    for (uint16_t i = 0; i < actually_read; i++) {
      route_point_to_wire(batch[i], &wire_buf[i * ROUTE_POINT_WIRE_SIZE]);
    }

    send_history_binary(static_cast<uint16_t>(offset), wire_buf,
                        actually_read * ROUTE_POINT_WIRE_SIZE);

    offset += actually_read;
    sent += actually_read;
  }

  // Send "done" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DONE);
    cbor_encode_text_stringz(&map, BLE_KEY_SENT);
    cbor_encode_uint(&map, sent);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }
}

// ---------------------------------------------------------------------------
// History: fill missing points
// ---------------------------------------------------------------------------

void BleService::handle_history_fill(const uint32_t *point_indices, size_t count) {
  if (_history_char == nullptr) {
    return;
  }

  if (!_export_active) {
    // Send error: no active download
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, BLE_VAL_ERR_NO_ACTIVE_DOWNLOAD);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  uint32_t sent = 0;

  for (size_t i = 0; i < count && _connected.load(); i++) {
    RoutePoint point;
    uint16_t actually_read =
        _storage.read_route_points(_export_session_id, point_indices[i], &point, 1);

    if (actually_read == 0) {
      AG_LOGW(TAG, "fill: could not read point %" PRIu32, point_indices[i]);
      continue;
    }

    uint8_t wire_buf[ROUTE_POINT_WIRE_SIZE];
    route_point_to_wire(point, wire_buf);
    send_history_binary(static_cast<uint16_t>(point_indices[i]), wire_buf, ROUTE_POINT_WIRE_SIZE);
    sent++;
  }

  // Send "done" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DONE);
  cbor_encode_text_stringz(&map, BLE_KEY_SENT);
  cbor_encode_uint(&map, sent);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: end download
// ---------------------------------------------------------------------------

void BleService::handle_history_end() {
  _export_active = false;
  _export_session_id = 0;

  if (_history_char == nullptr) {
    return;
  }

  // Send "ended" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 1);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ENDED);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

void BleService::handle_history_delete(uint32_t session_id) {
  if (_history_char == nullptr) {
    return;
  }

  // Check if session exists
  uint32_t point_count = _storage.get_session_point_count(session_id);
  if (point_count == 0) {
    notify_history_error(BLE_VAL_ERR_SESSION_NOT_FOUND);
    return;
  }

  // End any active export for this session
  if (_export_active && _export_session_id == session_id) {
    _export_active = false;
    _export_session_id = 0;
  }

  // Delete the route file
  if (!_storage.delete_route(session_id)) {
    notify_history_error(BLE_VAL_ERR_DELETE_FAILED);
    return;
  }

  // Send "deleted" CBOR response: {"type": "deleted", "session": <id>}
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DELETED);
  cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
  cbor_encode_uint(&map, session_id);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

void BleService::notify_history_error(const char *err) {
  if (_history_char == nullptr || err == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
  cbor_encode_text_stringz(&map, BLE_KEY_ERR);
  cbor_encode_text_stringz(&map, err);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: notification helpers
// ---------------------------------------------------------------------------

bool BleService::send_history_cbor(const uint8_t *cbor_data, size_t cbor_len) {
  if (_history_char == nullptr || cbor_len == 0) {
    return false;
  }

  // Prepend tag byte 0x00
  uint8_t tagged[CBOR_BUF_SIZE + 1];
  tagged[0] = HISTORY_TAG_CBOR;
  size_t total = 1 + cbor_len;
  if (total > sizeof(tagged)) {
    AG_LOGW(TAG, "history CBOR too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }
  memcpy(&tagged[1], cbor_data, cbor_len);

  _history_char->set_value(tagged, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

bool BleService::send_history_binary(uint16_t first_point_index, const uint8_t *data,
                                     size_t data_len) {
  if (_history_char == nullptr || data_len == 0) {
    return false;
  }

  // Build notification: [tag] [uint16_le index] [data...]
  uint8_t packet[MAX_NOTIFY_PAYLOAD];
  size_t total = BINARY_HEADER_SIZE + data_len;
  if (total > sizeof(packet)) {
    AG_LOGW(TAG, "history binary too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }

  packet[0] = HISTORY_TAG_BINARY;
  // Little-endian uint16
  packet[1] = static_cast<uint8_t>(first_point_index & 0xFF);
  packet[2] = static_cast<uint8_t>((first_point_index >> 8) & 0xFF);
  memcpy(&packet[BINARY_HEADER_SIZE], data, data_len);

  _history_char->set_value(packet, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

// ---------------------------------------------------------------------------
// RoutePointWire conversion
// ---------------------------------------------------------------------------

void BleService::route_point_to_wire(const RoutePoint &point, uint8_t *out) {
  // 55 bytes, packed little-endian.
  // Use memcpy for type-punning safety (platform is little-endian on ESP32).

  size_t offset = 0;

  auto write_u32 = [&](uint32_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f64 = [&](double v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f32 = [&](float v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_u8 = [&](uint8_t v) {
    out[offset] = v;
    offset += 1;
  };
  auto write_i16 = [&](int16_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };

  // timestamp (4)
  write_u32(static_cast<uint32_t>(point.timestamp));

  // GPS position
  if (is_latitude_valid(point.gps.position.latitude)) {
    write_f64(point.gps.position.latitude);
  } else {
    write_f64(GPS_LATITUDE_INVALID);
  }

  if (is_longitude_valid(point.gps.position.longitude)) {
    write_f64(point.gps.position.longitude);
  } else {
    write_f64(GPS_LONGITUDE_INVALID);
  }

  // altitude (4)
  if (is_altitude_valid(point.gps.altitude_m)) {
    write_f32(point.gps.altitude_m);
  } else {
    write_f32(GPS_ALTITUDE_INVALID);
  }

  // gps_fix (1)
  write_u8(static_cast<uint8_t>(point.gps.fix.fix_type));

  // Sensor data — use sentinel values for invalid fields
  const auto &th = point.sensors.temp_hum_a;
  write_f32(th.is_temp_valid() ? th.temperature : MeasuresInvalid::TEMPERATURE);
  write_f32(th.is_hum_valid() ? th.humidity : MeasuresInvalid::HUMIDITY);

  const auto &pm = point.sensors.pm_a;
  write_f32(pm.is_pm_01_valid() ? pm.pm_01 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_25_valid() ? pm.pm_25 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_10_valid() ? pm.pm_10 : MeasuresInvalid::PM);

  const auto &co2 = point.sensors.co2;
  write_i16(co2.is_valid() ? static_cast<int16_t>(co2.co2) : static_cast<int16_t>(-1));

  const auto &tvoc_nox = point.sensors.tvoc_nox;
  write_i16(tvoc_nox.is_tvoc_index_valid() ? static_cast<int16_t>(tvoc_nox.tvoc_index)
                                           : static_cast<int16_t>(-1));
  write_i16(tvoc_nox.is_nox_index_valid() ? static_cast<int16_t>(tvoc_nox.nox_index)
                                          : static_cast<int16_t>(-1));

  // pressure (4)
  const auto &pres = point.sensors.pressure;
  write_f32(pres.is_pressure_valid() ? pres.pressure : MeasuresInvalid::PRESSURE);

  // battery_percentage (1) — 0–100 valid, 255 = invalid
  write_u8(point.battery_percentage >= 0.0f ? static_cast<uint8_t>(point.battery_percentage)
                                            : static_cast<uint8_t>(255));
}

// ---------------------------------------------------------------------------
// CBOR encoding: Measures
// ---------------------------------------------------------------------------

size_t BleService::encode_measures(uint8_t *buf, size_t buf_size, const MeasuresAGo &m,
                                   const GpsData &gps, time_t ts) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Count fields to determine map size (ts is always present)
  size_t field_count = 1; // "ts"

  const auto &th = m.temp_hum_a;
  if (th.is_temp_valid())
    field_count++;
  if (th.is_hum_valid())
    field_count++;

  const auto &pm = m.pm_a;
  if (pm.is_pm_01_valid())
    field_count++;
  if (pm.is_pm_25_valid())
    field_count++;
  if (pm.is_pm_10_valid())
    field_count++;

  if (m.co2.is_valid())
    field_count++;
  if (m.tvoc_nox.is_tvoc_index_valid())
    field_count++;
  if (m.tvoc_nox.is_nox_index_valid())
    field_count++;

  if (m.pressure.is_pressure_valid())
    field_count++;

  // GPS fields are only included when tracking (indicated by fix type check).
  // The caller (orchestrator) calls notify_measures() on every SensorDataReady;
  // we check fix availability as a defensive guard.
  // GPS fields: lat, lon, alt, fix, sat — always included as a group when tracking.
  bool include_gps = is_fix_valid(gps.fix);
  if (include_gps) {
    // lat, lon conditionally based on validity
    if (is_latitude_valid(gps.position.latitude))
      field_count++;
    if (is_longitude_valid(gps.position.longitude))
      field_count++;
    if (is_altitude_valid(gps.altitude_m))
      field_count++;
    field_count++; // "fix" (always when GPS included)
    field_count++; // "sat" (always when GPS included)
  }

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, field_count);

  // Sensor fields — omit invalid
  if (th.is_temp_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_TEMP);
    cbor_encode_float(&map, th.temperature);
  }
  if (th.is_hum_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_HUM);
    cbor_encode_float(&map, th.humidity);
  }

  if (pm.is_pm_01_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM1);
    cbor_encode_float(&map, pm.pm_01);
  }
  if (pm.is_pm_25_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM25);
    cbor_encode_float(&map, pm.pm_25);
  }
  if (pm.is_pm_10_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM10);
    cbor_encode_float(&map, pm.pm_10);
  }

  if (m.co2.is_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_CO2);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.co2.co2));
  }

  if (m.tvoc_nox.is_tvoc_index_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_TVOC);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.tvoc_index));
  }
  if (m.tvoc_nox.is_nox_index_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_NOX);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.nox_index));
  }

  if (m.pressure.is_pressure_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PRES);
    cbor_encode_float(&map, m.pressure.pressure);
  }

  // GPS fields (only when tracking / fix valid)
  if (include_gps) {
    if (is_latitude_valid(gps.position.latitude)) {
      cbor_encode_text_stringz(&map, BLE_KEY_LAT);
      cbor_encode_double(&map, gps.position.latitude);
    }
    if (is_longitude_valid(gps.position.longitude)) {
      cbor_encode_text_stringz(&map, BLE_KEY_LON);
      cbor_encode_double(&map, gps.position.longitude);
    }
    if (is_altitude_valid(gps.altitude_m)) {
      cbor_encode_text_stringz(&map, BLE_KEY_ALT);
      cbor_encode_float(&map, gps.altitude_m);
    }
    cbor_encode_text_stringz(&map, BLE_KEY_FIX);
    cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

    cbor_encode_text_stringz(&map, BLE_KEY_SAT);
    cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                     ? gps.fix.satellite_count
                                                     : 0));
  }

  // Timestamp is always present
  cbor_encode_text_stringz(&map, BLE_KEY_TS);
  cbor_encode_uint(&map, static_cast<uint64_t>(ts));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Status
// ---------------------------------------------------------------------------

size_t BleService::encode_status(uint8_t *buf, size_t buf_size, const PowerSnapshot &power,
                                 const GpsData &gps, bool tracking, uint32_t session_id) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // All 10 keys are always present
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 10);

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_FIX);
  cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_SAT);
  cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                   ? gps.fix.satellite_count
                                                   : 0));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_PCT);
  cbor_encode_uint(&map, static_cast<uint64_t>(power.battery_percentage >= 0.0f
                                                   ? static_cast<uint32_t>(power.battery_percentage)
                                                   : 0));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_V);
  cbor_encode_float(&map, power.battery_voltage >= 0.0f ? power.battery_voltage : 0.0f);

  cbor_encode_text_stringz(&map, BLE_KEY_CHARGING);
  cbor_encode_text_stringz(&map, charging_state_to_str(power.charging_status));

  cbor_encode_text_stringz(&map, BLE_KEY_TRACKING);
  cbor_encode_boolean(&map, tracking);

  cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
  cbor_encode_uint(&map, session_id);

  const std::string firmware_version = build_firmware_version();
  const char *fw_version = firmware_version.empty() ? "unknown" : firmware_version.c_str();

  cbor_encode_text_stringz(&map, BLE_KEY_FLASH_KB);
  cbor_encode_uint(&map, _storage.total_capacity_kb());

  cbor_encode_text_stringz(&map, BLE_KEY_USED_KB);
  cbor_encode_uint(&map, _storage.used_kb());

  cbor_encode_text_stringz(&map, BLE_KEY_FW);
  cbor_encode_text_stringz(&map, fw_version);

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Config
// ---------------------------------------------------------------------------

size_t BleService::encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // 9 config keys
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 9);

  cbor_encode_text_stringz(&map, BLE_KEY_MEAS_INT);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.measure_interval_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_TEMP_F);
  cbor_encode_boolean(&map, settings.use_fahrenheit);

  cbor_encode_text_stringz(&map, BLE_KEY_PM_AQI);
  cbor_encode_boolean(&map, settings.pm_use_usaqi);

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_INT);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.gps_interval_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_MODE);
  cbor_encode_text_stringz(&map, gps_mode_to_str(settings.gps_mode));

  cbor_encode_text_stringz(&map, BLE_KEY_INACT_TO);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.inactivity_timeout_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_AUTO_LOCK);
  cbor_encode_uint(&map, static_cast<uint64_t>(settings.auto_lock_seconds));

  cbor_encode_text_stringz(&map, BLE_KEY_DEV_NAME);
  cbor_encode_text_stringz(&map, settings.device_name.c_str());

  cbor_encode_text_stringz(&map, BLE_KEY_OP_MODE);
  cbor_encode_text_stringz(&map, operating_mode_to_str(settings.operating_mode));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// String mapping helpers
// ---------------------------------------------------------------------------

const char *BleService::charging_state_to_str(BmsChargingState state) {
  switch (state) {
  case BmsChargingState::NotCharging:
    return BLE_VAL_CHARGE_NONE;
  case BmsChargingState::TrickleCharge:
    return BLE_VAL_CHARGE_TRICKLE;
  case BmsChargingState::PreCharge:
    return BLE_VAL_CHARGE_PRE;
  case BmsChargingState::FastCharge:
    return BLE_VAL_CHARGE_FAST;
  case BmsChargingState::TaperCharge:
    return BLE_VAL_CHARGE_TAPER;
  case BmsChargingState::TopOffTimerActiveCharging:
    return BLE_VAL_CHARGE_TOPOFF;
  case BmsChargingState::ChargeTerminationDone:
    return BLE_VAL_CHARGE_DONE;
  case BmsChargingState::Unknown:
  default:
    return BLE_VAL_CHARGE_UNKNOWN;
  }
}

const char *BleService::gps_mode_to_str(GpsMode mode) {
  switch (mode) {
  case GpsMode::AlwaysOff:
    return BLE_VAL_GPS_OFF;
  case GpsMode::OnWhenTracking:
    return BLE_VAL_GPS_TRACKING;
  case GpsMode::AlwaysOn:
    return BLE_VAL_GPS_ALWAYS;
  default:
    return BLE_VAL_GPS_TRACKING;
  }
}

const char *BleService::operating_mode_to_str(OperatingMode mode) {
  switch (mode) {
  case OperatingMode::Portable:
    return BLE_VAL_MODE_PORTABLE;
  case OperatingMode::Stationary:
    return BLE_VAL_MODE_STATIONARY;
  case OperatingMode::Offline:
    return BLE_VAL_MODE_OFFLINE;
  default:
    return BLE_VAL_MODE_OFFLINE;
  }
}

// ---------------------------------------------------------------------------
// CBOR decode helpers (called by orchestrator)
// ---------------------------------------------------------------------------

/// Reverse mapping: text string -> GpsMode.
static GpsMode str_to_gps_mode(const char *s) {
  if (strcmp(s, BLE_VAL_GPS_OFF) == 0) {
    return GpsMode::AlwaysOff;
  }
  if (strcmp(s, BLE_VAL_GPS_ALWAYS) == 0) {
    return GpsMode::AlwaysOn;
  }
  return GpsMode::OnWhenTracking; // "tracking" or unrecognized
}

/// Reverse mapping: text string -> OperatingMode.
static OperatingMode str_to_operating_mode(const char *s) {
  if (strcmp(s, BLE_VAL_MODE_PORTABLE) == 0) {
    return OperatingMode::Portable;
  }
  if (strcmp(s, BLE_VAL_MODE_STATIONARY) == 0) {
    return OperatingMode::Stationary;
  }
  return OperatingMode::Offline; // "offline" or unrecognized
}

/// Reverse mapping: CBOR "cmd" string -> BleCommand enum.
static BleCommand str_to_ble_command(const char *s) {
  if (strcmp(s, BLE_VAL_CMD_CO2_CAL) == 0) {
    return BleCommand::Co2Calibration;
  }
  if (strcmp(s, BLE_VAL_CMD_CLEAR_DATA) == 0) {
    return BleCommand::ClearData;
  }
  if (strcmp(s, BLE_VAL_CMD_FACTORY_RST) == 0) {
    return BleCommand::FactoryReset;
  }
  if (strcmp(s, BLE_VAL_CMD_START_TRACKING) == 0) {
    return BleCommand::StartTracking;
  }
  if (strcmp(s, BLE_VAL_CMD_STOP_TRACKING) == 0) {
    return BleCommand::StopTracking;
  }
  if (strcmp(s, BLE_VAL_CMD_SET_AIDING) == 0) {
    return BleCommand::SetAiding;
  }
  return BleCommand::Unknown;
}

/// Forward mapping: BleCommand enum -> wire string for CBOR responses.
static const char *ble_command_to_str(BleCommand cmd) {
  switch (cmd) {
  case BleCommand::Co2Calibration:
    return BLE_VAL_CMD_CO2_CAL;
  case BleCommand::ClearData:
    return BLE_VAL_CMD_CLEAR_DATA;
  case BleCommand::FactoryReset:
    return BLE_VAL_CMD_FACTORY_RST;
  case BleCommand::StartTracking:
    return BLE_VAL_CMD_START_TRACKING;
  case BleCommand::StopTracking:
    return BLE_VAL_CMD_STOP_TRACKING;
  case BleCommand::SetAiding:
    return BLE_VAL_CMD_SET_AIDING;
  case BleCommand::Set:
    return BLE_VAL_CMD_SET;
  case BleCommand::Unknown:
    return BLE_VAL_CMD_UNKNOWN;
  }
  return BLE_VAL_CMD_UNKNOWN;
}

BleConfigDecodeResult BleService::decode_config_write(const uint8_t *buf, size_t len,
                                                      GoSettings &settings) {
  BleConfigDecodeResult result{};
  if (buf == nullptr || len == 0) {
    return result;
  }

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError) {
    return result;
  }
  if (!cbor_value_is_map(&root)) {
    return result;
  }

  CborValue it;
  if (cbor_value_enter_container(&root, &it) != CborNoError) {
    return result;
  }

  char op_str[16] = {};

  // Key comparison helper
  auto key_is = [&it](const char *name) -> bool {
    bool match = false;
    cbor_value_text_string_equals(&it, name, &match);
    return match;
  };

  while (!cbor_value_at_end(&it)) {
    if (!cbor_value_is_text_string(&it)) {
      // Skip non-string key + its value
      cbor_value_advance(&it);
      if (!cbor_value_at_end(&it)) {
        cbor_value_advance(&it);
      }
      continue;
    }

    bool handled = false;

    // --- "op" field ---
    if (key_is(BLE_KEY_OP)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(op_str) - 1;
        cbor_value_copy_text_string(&it, op_str, &slen, nullptr);
        op_str[slen] = '\0';
      }
      handled = true;
    }
    // --- "cmd" field ---
    else if (key_is(BLE_KEY_CMD)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        char cmd_str[32] = {};
        size_t slen = sizeof(cmd_str) - 1;
        cbor_value_copy_text_string(&it, cmd_str, &slen, nullptr);
        cmd_str[slen] = '\0';
        result.cmd = str_to_ble_command(cmd_str);
      }
      handled = true;
    }
    // --- uint config fields ---
    else if (key_is(BLE_KEY_MEAS_INT)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.measure_interval_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    }
    // Deprecated keys — skip value, do not modify settings
    else if (key_is(BLE_KEY_PM_INT) || key_is(BLE_KEY_OTHER_INT) || key_is(BLE_KEY_DISP_INT)) {
      cbor_value_advance(&it);
      handled = true;
    } else if (key_is(BLE_KEY_GPS_INT)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.gps_interval_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_INACT_TO)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.inactivity_timeout_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_AUTO_LOCK)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.auto_lock_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    }
    // --- bool config fields ---
    else if (key_is(BLE_KEY_TEMP_F)) {
      cbor_value_advance(&it);
      bool v = false;
      if (cbor_value_is_boolean(&it) && cbor_value_get_boolean(&it, &v) == CborNoError) {
        settings.use_fahrenheit = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_PM_AQI)) {
      cbor_value_advance(&it);
      bool v = false;
      if (cbor_value_is_boolean(&it) && cbor_value_get_boolean(&it, &v) == CborNoError) {
        settings.pm_use_usaqi = v;
      }
      handled = true;
    }
    // --- text config fields ---
    else if (key_is(BLE_KEY_GPS_MODE)) {
      cbor_value_advance(&it);
      char text[16] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.gps_mode = str_to_gps_mode(text);
      }
      handled = true;
    } else if (key_is(BLE_KEY_OP_MODE)) {
      cbor_value_advance(&it);
      char text[16] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.operating_mode = str_to_operating_mode(text);
      }
      handled = true;
    } else if (key_is(BLE_KEY_DEV_NAME)) {
      cbor_value_advance(&it);
      char text[65] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.device_name = text;
      }
      handled = true;
    }
    // --- Aiding command payload fields ---
    else if (key_is(BLE_KEY_LAT)) {
      cbor_value_advance(&it);
      double v = 0;
      if (cbor_value_is_double(&it) && cbor_value_get_double(&it, &v) == CborNoError) {
        result.aiding.latitude = v;
      } else if (cbor_value_is_float(&it)) {
        float fv = 0;
        if (cbor_value_get_float(&it, &fv) == CborNoError) {
          result.aiding.latitude = static_cast<double>(fv);
        }
      }
      handled = true;
    } else if (key_is(BLE_KEY_LON)) {
      cbor_value_advance(&it);
      double v = 0;
      if (cbor_value_is_double(&it) && cbor_value_get_double(&it, &v) == CborNoError) {
        result.aiding.longitude = v;
      } else if (cbor_value_is_float(&it)) {
        float fv = 0;
        if (cbor_value_get_float(&it, &fv) == CborNoError) {
          result.aiding.longitude = static_cast<double>(fv);
        }
      }
      handled = true;
    } else if (key_is(BLE_KEY_ALT)) {
      cbor_value_advance(&it);
      float v = 0;
      if (cbor_value_is_double(&it)) {
        double dv = 0;
        if (cbor_value_get_double(&it, &dv) == CborNoError) {
          v = static_cast<float>(dv);
        }
        result.aiding.altitude_m = v;
      } else if (cbor_value_is_float(&it)) {
        cbor_value_get_float(&it, &v);
        result.aiding.altitude_m = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_POS_ACC)) {
      cbor_value_advance(&it);
      float v = 0;
      if (cbor_value_is_double(&it)) {
        double dv = 0;
        if (cbor_value_get_double(&it, &dv) == CborNoError) {
          v = static_cast<float>(dv);
        }
        result.aiding.pos_acc_m = v;
      } else if (cbor_value_is_float(&it)) {
        cbor_value_get_float(&it, &v);
        result.aiding.pos_acc_m = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_EPOCH)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.aiding.epoch_s = static_cast<int64_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_TIME_ACC)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.aiding.time_acc_ms = static_cast<uint32_t>(v);
      }
      handled = true;
    }

    if (!handled) {
      // Unknown config key — flag it and skip the value
      result.has_unknown_keys = true;
      cbor_value_advance(&it);
    }

    // Advance past value to next key
    if (!cbor_value_at_end(&it)) {
      cbor_value_advance(&it);
    }
  }

  // Determine operation type
  if (strcmp(op_str, BLE_VAL_OP_SET) == 0) {
    result.op = BleConfigOp::Set;
  } else if (strcmp(op_str, BLE_VAL_OP_CMD) == 0) {
    result.op = BleConfigOp::Command;
  }

  return result;
}

BleHistoryDecodeResult BleService::decode_history_write(const uint8_t *buf, size_t len) {
  BleHistoryDecodeResult result{};
  if (buf == nullptr || len == 0) {
    return result;
  }

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError) {
    return result;
  }
  if (!cbor_value_is_map(&root)) {
    return result;
  }

  CborValue it;
  if (cbor_value_enter_container(&root, &it) != CborNoError) {
    return result;
  }

  char op_str[16] = {};

  auto key_is = [&it](const char *name) -> bool {
    bool match = false;
    cbor_value_text_string_equals(&it, name, &match);
    return match;
  };

  while (!cbor_value_at_end(&it)) {
    if (!cbor_value_is_text_string(&it)) {
      cbor_value_advance(&it);
      if (!cbor_value_at_end(&it)) {
        cbor_value_advance(&it);
      }
      continue;
    }

    bool handled = false;

    if (key_is(BLE_KEY_OP)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(op_str) - 1;
        cbor_value_copy_text_string(&it, op_str, &slen, nullptr);
        op_str[slen] = '\0';
      }
      handled = true;
    } else if (key_is(BLE_KEY_SESSION)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.session_id = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_PTS)) {
      cbor_value_advance(&it);
      if (cbor_value_is_array(&it)) {
        CborValue arr;
        if (cbor_value_enter_container(&it, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) &&
                 result.point_count < BleHistoryDecodeResult::MAX_FILL_POINTS) {
            uint64_t v = 0;
            if (cbor_value_is_unsigned_integer(&arr) &&
                cbor_value_get_uint64(&arr, &v) == CborNoError) {
              result.point_indices[result.point_count++] = static_cast<uint32_t>(v);
            }
            cbor_value_advance(&arr);
          }
          cbor_value_leave_container(&it, &arr);
        }
        // After leaving container, it already points past the array — skip
        // the trailing advance below.
        if (!cbor_value_at_end(&it)) {
          cbor_value_advance(&it);
        }
        continue;
      }
      handled = true;
    }

    if (!handled) {
      cbor_value_advance(&it);
    }

    if (!cbor_value_at_end(&it)) {
      cbor_value_advance(&it);
    }
  }

  // Determine operation type
  if (strcmp(op_str, BLE_VAL_OP_LIST) == 0) {
    result.op = BleHistoryOp::List;
  } else if (strcmp(op_str, BLE_VAL_OP_START) == 0) {
    result.op = BleHistoryOp::Start;
  } else if (strcmp(op_str, BLE_VAL_OP_FILL) == 0) {
    result.op = BleHistoryOp::Fill;
  } else if (strcmp(op_str, BLE_VAL_OP_END) == 0) {
    result.op = BleHistoryOp::End;
  } else if (strcmp(op_str, BLE_VAL_OP_DELETE) == 0) {
    result.op = BleHistoryOp::Delete;
  }

  return result;
}
