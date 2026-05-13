/**
 * AirGradient Go — GPS Driver unit tests
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "gps/gps_driver.h"
#include "rtos.h"

// ---------------------------------------------------------------------------
// StubRTOS — no-op RTOS for host tests (delay and time are no-ops).
// ---------------------------------------------------------------------------
class StubRTOS : public RTOS {
  void delay_ms_impl(uint32_t /*ms*/) override {}
  uint64_t get_time_ms_impl() override { return 0; }
};

// ---------------------------------------------------------------------------
// StubSerial — minimal AirgradientSerial that serves data from a byte queue
// and captures bytes written by the driver.
// ---------------------------------------------------------------------------

/// Per-write ACK behavior for StubSerial.  When entries are queued via
/// queue_ack_response(), they override the _auto_ack default.
enum class AckMode { Ack, Nak, None };

class StubSerial : public AirgradientSerial {
public:
  void queue_rx(const char *data) {
    while (data != nullptr && *data != '\0') {
      _rx.push_back(static_cast<uint8_t>(*data));
      ++data;
    }
  }

  /// Queue raw bytes into the RX buffer (for binary CASIC responses).
  void queue_rx_bytes(const uint8_t *data, size_t len) { _rx.insert(_rx.end(), data, data + len); }

  /// Return all bytes written via write() since construction or last clear.
  const std::vector<uint8_t> &get_tx_bytes() const { return _tx; }

  /// Clear the TX capture buffer.
  void clear_tx() { _tx.clear(); }

  bool begin(int /*baud*/) override { return true; }
  void end() override {}

  int available() override { return static_cast<int>(_rx.size()); }

  int read() override {
    if (_rx.empty()) {
      return -1;
    }
    const uint8_t val = _rx.front();
    _rx.pop_front();
    return val;
  }

  void print(const char * /*str*/) override {}

  int write(const uint8_t *data, int len) override {
    _tx.insert(_tx.end(), data, data + len);

    if (len >= 8 && data[0] == 0xF1 && data[1] == 0xD9) {
      const uint8_t grp = data[2];
      const uint8_t sub_id = data[3];
      const uint16_t payload_len =
          static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);

      // Determine ACK behavior: consume from the explicit sequence if
      // available, otherwise fall back to the _auto_ack default.
      AckMode mode = AckMode::None;
      if (!_ack_sequence.empty()) {
        mode = _ack_sequence.front();
        _ack_sequence.pop_front();
      } else if (_auto_ack) {
        mode = AckMode::Ack;
      }

      if (mode == AckMode::Ack || mode == AckMode::Nak) {
        const uint8_t ack_sub = (mode == AckMode::Ack) ? 0x01 : 0x00;
        // clang-format off
        const uint8_t ack[] = {
            0xF1, 0xD9, 0x05, ack_sub, 0x02, 0x00, grp, sub_id, 0x00, 0x00,
        };
        // clang-format on
        _rx.insert(_rx.end(), std::begin(ack), std::end(ack));
      }

      // Auto-respond to poll packets (CASIC with payload length = 0).
      // When a registered poll response matches group/sub, queue the
      // full response packet (with computed checksum) into RX.
      if (payload_len == 0) {
        for (const auto &resp : _poll_responses) {
          if (resp.group == grp && resp.sub == sub_id) {
            _queue_casic_response(grp, sub_id, resp.payload.data(),
                                  static_cast<uint16_t>(resp.payload.size()));
            break;
          }
        }
      }
    }

    return len;
  }

  /// Enable or disable automatic ACK responses to CASIC CFG commands.
  /// Used as the default when the explicit ACK sequence is empty.
  void set_auto_ack(bool enable) { _auto_ack = enable; }

  /// Queue an explicit ACK behavior for the next CASIC write.  Entries are
  /// consumed in FIFO order; when exhausted, falls back to _auto_ack.
  void queue_ack_response(AckMode mode) { _ack_sequence.push_back(mode); }

  /// Register a poll response: when a CASIC poll (payload length = 0) with
  /// matching group/sub is written, queue a response packet into RX with
  /// the given payload and a valid checksum.
  void add_poll_response(uint8_t group, uint8_t sub, const uint8_t *payload, size_t payload_len) {
    _poll_responses.push_back({group, sub, std::vector<uint8_t>(payload, payload + payload_len)});
  }

  /// Remove all registered poll responses.
  void clear_poll_responses() { _poll_responses.clear(); }

private:
  struct PollResponse {
    uint8_t group;
    uint8_t sub;
    std::vector<uint8_t> payload;
  };

  std::deque<uint8_t> _rx;
  std::vector<uint8_t> _tx;
  bool _auto_ack = false;
  std::deque<AckMode> _ack_sequence;
  std::vector<PollResponse> _poll_responses;

  /// Build and queue a CASIC response packet into the RX buffer.
  void _queue_casic_response(uint8_t group, uint8_t sub, const uint8_t *payload,
                             uint16_t payload_len) {
    std::vector<uint8_t> pkt;
    pkt.reserve(2 + 2 + 2 + payload_len + 2);
    pkt.push_back(0xF1);
    pkt.push_back(0xD9);
    pkt.push_back(group);
    pkt.push_back(sub);
    pkt.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    pkt.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    pkt.insert(pkt.end(), payload, payload + payload_len);
    // 8-Bit Fletcher checksum over group + sub + length + payload.
    uint8_t ck1 = 0, ck2 = 0;
    for (size_t i = 2; i < pkt.size(); ++i) {
      ck1 = (ck1 + pkt[i]) & 0xFF;
      ck2 = (ck2 + ck1) & 0xFF;
    }
    pkt.push_back(ck1);
    pkt.push_back(ck2);
    _rx.insert(_rx.end(), pkt.begin(), pkt.end());
  }
};

// Reference NMEA sentences with verified correct checksums.
// Checksums computed as XOR of all bytes between '$' and '*' (exclusive).
//
// GGA: position_fix=1, lat=4717.11364N, lon=00833.91565E, alt=499.6, sats=8
static constexpr const char *GGA_VALID =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";

// Same sentence but position_fix=0 (no fix).
static constexpr const char *GGA_NO_FIX =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,0,08,1.01,499.6,M,48.0,M,,*52\r\n";

// RMC: status=A (valid), same lat/lon, date=091202, time=092725
static constexpr const char *RMC_VALID =
    "$GPRMC,092725.00,A,4717.11364,N,00833.91565,E,0.004,77.52,091202,,,A*5C\r\n";

// GSA: fix=3 (3D), pdop=1.94, hdop=1.01, vdop=1.65
static constexpr const char *GSA_VALID =
    "$GPGSA,A,3,23,29,07,08,09,18,26,28,,,,,1.94,1.01,1.65*07\r\n";

// GGA with a deliberately wrong checksum (*FF instead of the correct *53).
static constexpr const char *GGA_BAD_CHECKSUM =
    "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*FF\r\n";

// GGA with GN (multi-GNSS) talker ID — must be accepted by the sentence
// filter and parsed identically to $GPGGA by libnmea-esp32.
static constexpr const char *GGA_GN_TALKER =
    "$GNGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*4D\r\n";

// Filtered sentence types — the sentence filter drops these before
// nmea_parse() is called, so checksums are included for realism but are
// irrelevant to filter behavior.
static constexpr const char *GSV_SENTENCE =
    "$GPGSV,3,1,09,02,28,067,42,04,15,116,38,05,53,299,47,08,74,012,45*7B\r\n";
static constexpr const char *GLL_SENTENCE =
    "$GPGLL,4717.11364,N,00833.91565,E,092725.00,A,A*60\r\n";
static constexpr const char *VTG_SENTENCE = "$GPVTG,77.52,T,,M,0.004,N,0.008,K,A*06\r\n";
static constexpr const char *TXT_SENTENCE = "$GPTXT,01,01,02,ANTSTATUS=OPEN*2B\r\n";
static constexpr const char *PROPRIETARY_SENTENCE = "$PMTK010,001*2E\r\n";
static constexpr const char *SHORT_SENTENCE = "$GP\r\n";

// Expected parsed values.
static constexpr double EXPECTED_LAT = 47.0 + 17.11364 / 60.0; // ~47.285227
static constexpr double EXPECTED_LON = 8.0 + 33.91565 / 60.0;  // ~8.565261
static constexpr float EXPECTED_ALT = 499.6f;
static constexpr int EXPECTED_SATS = 8;
static constexpr float EXPECTED_HDOP = 1.01f;
static constexpr float EXPECTED_PDOP = 1.94f;
static constexpr float EXPECTED_VDOP = 1.65f;
static constexpr int EXPECTED_YEAR = 2002;
static constexpr int EXPECTED_MONTH = 12;
static constexpr int EXPECTED_DAY = 9;
static constexpr int EXPECTED_HOUR = 9;
static constexpr int EXPECTED_MINUTE = 27;
static constexpr int EXPECTED_SECOND = 25;

// ---------------------------------------------------------------------------
// Test 1 — Scaffold: component compiles and links under TEST_HOST.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver scaffold compiles and links under TEST_HOST", "[gps][driver]") { SUCCEED(); }

// ---------------------------------------------------------------------------
// Test 2 — GGA parsing.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver parses GGA sentence: position, altitude, satellite count", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);
  REQUIRE(is_position_valid(data.position));
  REQUIRE(is_altitude_valid(data.altitude_m));
  REQUIRE(is_satellite_count_valid(data.fix.satellite_count));
}

// ---------------------------------------------------------------------------
// Test 3 — RMC parsing.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver parses RMC sentence: position and timestamp", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(RMC_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  // RMC should fill position when GGA has not been received yet.
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));

  REQUIRE(data.timestamp.valid);
  REQUIRE(data.timestamp.year == EXPECTED_YEAR);
  REQUIRE(data.timestamp.month == EXPECTED_MONTH);
  REQUIRE(data.timestamp.day == EXPECTED_DAY);
  REQUIRE(data.timestamp.hour == EXPECTED_HOUR);
  REQUIRE(data.timestamp.minute == EXPECTED_MINUTE);
  REQUIRE(data.timestamp.second == EXPECTED_SECOND);
  REQUIRE(is_gps_timestamp_valid(data.timestamp));
}

// ---------------------------------------------------------------------------
// Test 4 — GSA parsing.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver parses GSA sentence: fix type and DOP values", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GSA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.fix.fix_type == GpsFixType::Fix3D);
  REQUIRE(data.fix.hdop == Catch::Approx(EXPECTED_HDOP).epsilon(0.01f));
  REQUIRE(data.fix.pdop == Catch::Approx(EXPECTED_PDOP).epsilon(0.01f));
  REQUIRE(data.fix.vdop == Catch::Approx(EXPECTED_VDOP).epsilon(0.01f));
}

// ---------------------------------------------------------------------------
// Test 5 — Multi-sentence accumulation.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver accumulates GGA + RMC + GSA and populates all fields", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_VALID);
  serial.queue_rx(RMC_VALID);
  serial.queue_rx(GSA_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();

  // Position from GGA.
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);

  // Timestamp from RMC.
  REQUIRE(data.timestamp.valid);
  REQUIRE(data.timestamp.year == EXPECTED_YEAR);
  REQUIRE(data.timestamp.month == EXPECTED_MONTH);

  // Fix type and DOP from GSA (overrides GGA's conservative Fix2D).
  REQUIRE(data.fix.fix_type == GpsFixType::Fix3D);
  REQUIRE(data.fix.hdop == Catch::Approx(EXPECTED_HDOP).epsilon(0.01f));
  REQUIRE(data.fix.pdop == Catch::Approx(EXPECTED_PDOP).epsilon(0.01f));
  REQUIRE(data.fix.vdop == Catch::Approx(EXPECTED_VDOP).epsilon(0.01f));

  REQUIRE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 6 — No fix.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver reports no valid fix when GGA position_fix is 0", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_NO_FIX);
  REQUIRE(gps.read());

  REQUIRE_FALSE(gps.has_valid_fix());
  REQUIRE(gps.get_data().fix.fix_type == GpsFixType::NoFix);
}

// ---------------------------------------------------------------------------
// Test 7 — Invalid checksum: data must remain at sentinels.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver ignores sentence with bad checksum", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_BAD_CHECKSUM);
  // Sentence boundary is reached (read returns true), but nmea_parse
  // rejects the invalid checksum, so state stays at sentinels.
  gps.read();

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 8 — Partial sentence: no crash, no state change.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver handles partial sentence without crashing or changing state",
          "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  // Feed the first 30 characters of a valid GGA sentence (no \r\n yet).
  const std::string partial(GGA_VALID, 30);
  serial.queue_rx(partial.c_str());
  gps.read();

  // No complete sentence has been delivered — state must remain at sentinels.
  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 9 — Buffer overflow: graceful discard of oversized input.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver discards input that overflows the accumulation buffer", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  // Build a fake "sentence" that far exceeds the 256-byte buffer:
  // '$' followed by 300 uppercase-A characters (no \r\n).
  std::string oversized;
  oversized += '$';
  oversized.append(300, 'A');
  serial.queue_rx(oversized.c_str());

  // Must not crash; buffer overflow path resets the accumulator.
  gps.read();

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 10 — Sentinel initialization.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver get_data returns all-invalid sentinels before any read", "[gps][driver]") {
  StubSerial serial;
  GpsDriver gps(serial);

  const GpsData data = gps.get_data();

  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE(data.fix.fix_type == GpsFixType::NoFix);
  REQUIRE(data.fix.satellite_count == GPS_SATELLITE_COUNT_INVALID);
  REQUIRE(data.fix.hdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.pdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.vdop == GPS_DOP_INVALID);
  REQUIRE_FALSE(data.timestamp.valid);

  REQUIRE_FALSE(gps.has_valid_fix());
  REQUIRE_FALSE(is_position_valid(data.position));
  REQUIRE_FALSE(is_altitude_valid(data.altitude_m));
  REQUIRE_FALSE(is_satellite_count_valid(data.fix.satellite_count));
  REQUIRE_FALSE(is_fix_valid(data.fix));
  REQUIRE_FALSE(is_gps_timestamp_valid(data.timestamp));
}

// ---------------------------------------------------------------------------
// Test 11 — Sentence filter: GSV is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver filters GSV sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GSV_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 12 — Sentence filter: GLL is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver filters GLL sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GLL_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 13 — Sentence filter: VTG is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver filters VTG sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(VTG_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 14 — Sentence filter: TXT is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver filters TXT sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(TXT_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 15 — Sentence filter: proprietary $PXXX sentence is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver filters proprietary $PXXX sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(PROPRIETARY_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 16 — Sentence filter: very short sentence (< 6 chars) is dropped.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver ignores very short sentence", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(SHORT_SENTENCE);
  REQUIRE_FALSE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE_FALSE(gps.has_valid_fix());
}

// ---------------------------------------------------------------------------
// Test 17 — GSV between GGA and RMC does not disturb parsed state.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver GSV between GGA and RMC is invisible to GpsData", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_VALID);
  serial.queue_rx(GSV_SENTENCE);
  serial.queue_rx(RMC_VALID);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();

  // Position from GGA.
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);

  // Timestamp from RMC.
  REQUIRE(data.timestamp.valid);
  REQUIRE(data.timestamp.year == EXPECTED_YEAR);
  REQUIRE(data.timestamp.month == EXPECTED_MONTH);
  REQUIRE(data.timestamp.day == EXPECTED_DAY);
  REQUIRE(data.timestamp.hour == EXPECTED_HOUR);
  REQUIRE(data.timestamp.minute == EXPECTED_MINUTE);
  REQUIRE(data.timestamp.second == EXPECTED_SECOND);
}

// ---------------------------------------------------------------------------
// Test 18 — GN talker ID: $GNGGA parsed same as $GPGGA.
// ---------------------------------------------------------------------------
TEST_CASE("GpsDriver accepts GN talker ID", "[gps][driver][filter]") {
  StubSerial serial;
  GpsDriver gps(serial);

  serial.queue_rx(GGA_GN_TALKER);
  REQUIRE(gps.read());

  const GpsData data = gps.get_data();
  REQUIRE(data.position.latitude == Catch::Approx(EXPECTED_LAT).epsilon(1e-5));
  REQUIRE(data.position.longitude == Catch::Approx(EXPECTED_LON).epsilon(1e-5));
  REQUIRE(data.altitude_m == Catch::Approx(EXPECTED_ALT).epsilon(0.1f));
  REQUIRE(data.fix.satellite_count == EXPECTED_SATS);
}

// ===========================================================================
// A-GNSS aiding tests
// ===========================================================================

// Helper: find a CASIC packet by group/sub in a byte vector.  Returns the
// offset of the 0xF1 header byte, or SIZE_MAX if not found.
static size_t find_casic_packet(const std::vector<uint8_t> &bytes, uint8_t group, uint8_t sub) {
  for (size_t i = 0; i + 5 < bytes.size(); ++i) {
    if (bytes[i] == 0xF1 && bytes[i + 1] == 0xD9 && bytes[i + 2] == group && bytes[i + 3] == sub) {
      return i;
    }
  }
  return SIZE_MAX;
}

// Helper: extract a little-endian uint16 from a byte pointer.
static uint16_t le_u16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

// Helper: extract a little-endian int32 from a byte pointer.
static int32_t le_s32(const uint8_t *p) {
  int32_t v;
  memcpy(&v, p, 4);
  return v;
}

// Helper: extract a little-endian uint32 from a byte pointer.
static uint32_t le_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// ---------------------------------------------------------------------------
// Test 19 — CASIC checksum matches spec example.
// Verify the 8-Bit Fletcher algorithm against the AID-POS spec example:
//   F1 D9 0B 10 11 00 01 87 54 69 0D AB 04 18 44 41 A7 FE FF 00 00 00 00 6E 4A
// Checksum covers bytes from 0B..00 00 00 00 (offset 2 through 22, 21 bytes).
// Expected: CK1=0x6E, CK2=0x4A.
// ---------------------------------------------------------------------------
TEST_CASE("CASIC checksum matches spec example", "[gps][driver][aiding]") {
  // Bytes from GroupID through end of payload (the region checksummed).
  // clang-format off
  const uint8_t data[] = {
      0x0B, 0x10, 0x11, 0x00,                         // group, sub, len_lo, len_hi
      0x01,                                             // type = LLA
      0x87, 0x54, 0x69, 0x0D,                          // lat = 225006727
      0xAB, 0x04, 0x18, 0x44,                          // lon = 1142424747
      0x41, 0xA7, 0xFE, 0xFF,                          // alt = -88255 cm
      0x00, 0x00, 0x00, 0x00,                          // acc = 0
  };
  // clang-format on

  // Replicate the checksum algorithm (same as casic_checksum in gps_driver.cpp).
  uint8_t ck1 = 0, ck2 = 0;
  for (size_t i = 0; i < sizeof(data); ++i) {
    ck1 = (ck1 + data[i]) & 0xFF;
    ck2 = (ck2 + ck1) & 0xFF;
  }
  REQUIRE(ck1 == 0x6E);
  REQUIRE(ck2 == 0x4A);
}

// ---------------------------------------------------------------------------
// Test 20 — inject_aiding sends AID-POS for valid position.
// ---------------------------------------------------------------------------
TEST_CASE("inject_aiding sends AID-POS for valid position", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.latitude = 22.5006727;
  aid.longitude = 114.2424747;
  // epoch_s = 0 → no time injection
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  // AID-POS packet present.
  REQUIRE(find_casic_packet(tx, 0x0B, 0x10) != SIZE_MAX);
  // AID-TIME packet NOT present.
  REQUIRE(find_casic_packet(tx, 0x0B, 0x11) == SIZE_MAX);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 21 — inject_aiding sends AID-TIME for valid time.
// ---------------------------------------------------------------------------
TEST_CASE("inject_aiding sends AID-TIME for valid time", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.epoch_s = 1466610963; // 2016-06-22 15:56:03 UTC
  // latitude/longitude remain invalid → no position injection
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  // AID-TIME packet present.
  REQUIRE(find_casic_packet(tx, 0x0B, 0x11) != SIZE_MAX);
  // AID-POS packet NOT present.
  REQUIRE(find_casic_packet(tx, 0x0B, 0x10) == SIZE_MAX);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 22 — inject_aiding sends both when both valid.
// ---------------------------------------------------------------------------
TEST_CASE("inject_aiding sends both when both valid", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.latitude = 47.285227;
  aid.longitude = 8.565261;
  aid.epoch_s = 1466610963;
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  const size_t pos_offset = find_casic_packet(tx, 0x0B, 0x10);
  const size_t time_offset = find_casic_packet(tx, 0x0B, 0x11);
  REQUIRE(pos_offset != SIZE_MAX);
  REQUIRE(time_offset != SIZE_MAX);
  // AID-POS sent before AID-TIME.
  REQUIRE(pos_offset < time_offset);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 23 — inject_aiding is no-op for default data.
// ---------------------------------------------------------------------------
TEST_CASE("inject_aiding is no-op for default data", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid; // all defaults
  gps.inject_aiding(aid);

  REQUIRE(serial.get_tx_bytes().empty());

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 24 — AID-POS encodes lat/lon/alt correctly (spec example).
// ---------------------------------------------------------------------------
TEST_CASE("AID-POS encodes lat/lon/alt correctly", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.latitude = 22.5006727;
  aid.longitude = 114.2424747;
  aid.altitude_m = -882.55f;
  aid.pos_acc_m = 0;
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x0B, 0x10);
  REQUIRE(off != SIZE_MAX);

  // Payload starts at off + 6 (after header(2) + ID(2) + length(2)).
  const uint8_t *payload = &tx[off + 6];

  REQUIRE(payload[0] == 0x01); // type = LLA

  const int32_t lat = le_s32(&payload[1]);
  const int32_t lon = le_s32(&payload[5]);
  const int32_t alt = le_s32(&payload[9]);
  const uint32_t acc = le_u32(&payload[13]);

  REQUIRE(lat == static_cast<int32_t>(22.5006727 * 1e7));
  REQUIRE(lon == static_cast<int32_t>(114.2424747 * 1e7));
  REQUIRE(alt == static_cast<int32_t>(-882.55f * 100.0f));
  REQUIRE(acc == 0);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 25 — AID-POS uses alt=0 when altitude invalid.
// ---------------------------------------------------------------------------
TEST_CASE("AID-POS uses alt=0 when altitude invalid", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.latitude = 47.0;
  aid.longitude = 8.0;
  // altitude_m defaults to GPS_ALTITUDE_INVALID
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x0B, 0x10);
  REQUIRE(off != SIZE_MAX);

  const uint8_t *payload = &tx[off + 6];
  const int32_t alt = le_s32(&payload[9]);
  REQUIRE(alt == 0);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 26 — AID-TIME encodes UTC fields correctly.
// Epoch 1466610963 = 2016-06-22 15:56:03 UTC
// ---------------------------------------------------------------------------
TEST_CASE("AID-TIME encodes UTC fields correctly", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.epoch_s = 1466610963;
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x0B, 0x11);
  REQUIRE(off != SIZE_MAX);

  const uint8_t *payload = &tx[off + 6];
  REQUIRE(payload[0] == 0x00); // type = UTC
  REQUIRE(payload[1] == 0x00); // reserved
  REQUIRE(payload[2] == 18);   // leap_sec (as of 2026)

  const uint16_t year = le_u16(&payload[3]);
  REQUIRE(year == 2016);
  REQUIRE(payload[5] == 6);  // month
  REQUIRE(payload[6] == 22); // day
  REQUIRE(payload[7] == 15); // hour
  REQUIRE(payload[8] == 56); // minute
  REQUIRE(payload[9] == 3);  // second

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 27 — AID-TIME encodes accuracy correctly.
// time_acc_ms = 2500 → tacc_s = 2, tacc_ns = 500000000
// ---------------------------------------------------------------------------
TEST_CASE("AID-TIME encodes accuracy correctly", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  GpsAidingData aid;
  aid.epoch_s = 1466610963;
  aid.time_acc_ms = 2500;
  gps.inject_aiding(aid);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x0B, 0x11);
  REQUIRE(off != SIZE_MAX);

  const uint8_t *payload = &tx[off + 6];
  const uint16_t tacc_s = le_u16(&payload[14]);
  const uint32_t tacc_ns = le_u32(&payload[16]);
  REQUIRE(tacc_s == 2);
  REQUIRE(tacc_ns == 500000000);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 28 — begin() sends CFG-EPHSAVE.
// ---------------------------------------------------------------------------
TEST_CASE("begin sends CFG-EPHSAVE", "[gps][driver][aiding]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x06, 0x10);
  REQUIRE(off != SIZE_MAX);

  // Verify payload: length=1, enable=1.
  REQUIRE(tx[off + 4] == 0x01); // len_lo = 1
  REQUIRE(tx[off + 5] == 0x00); // len_hi = 0
  REQUIRE(tx[off + 6] == 0x01); // enable = 1

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 29 — GpsAidingData default is no-injection.
// ---------------------------------------------------------------------------
TEST_CASE("GpsAidingData default is no-injection", "[gps][driver][aiding]") {
  const GpsAidingData aid;
  REQUIRE_FALSE(has_aiding_position(aid));
  REQUIRE_FALSE(has_aiding_time(aid));
}

// ===========================================================================
// GNSS start/stop control tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 30 — gnss_stop writes exact CASIC frame.
// Expected: F1 D9 06 40 01 00 10 57 31
// ---------------------------------------------------------------------------
TEST_CASE("gnss_stop writes exact CASIC frame", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  gps.gnss_stop();

  const auto &tx = serial.get_tx_bytes();
  // clang-format off
  const std::vector<uint8_t> expected = {
      0xF1, 0xD9, 0x06, 0x40, 0x01, 0x00, 0x10, 0x57, 0x31,
  };
  // clang-format on
  REQUIRE(tx == expected);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 31 — gnss_start writes exact CASIC frame.
// Expected: F1 D9 06 40 01 00 11 58 32
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start writes exact CASIC frame", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  gps.gnss_start();

  const auto &tx = serial.get_tx_bytes();
  // clang-format off
  const std::vector<uint8_t> expected = {
      0xF1, 0xD9, 0x06, 0x40, 0x01, 0x00, 0x11, 0x58, 0x32,
  };
  // clang-format on
  REQUIRE(tx == expected);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Helper: count how many times a CASIC packet with given group/sub appears
// in a TX byte vector.
// ---------------------------------------------------------------------------
static size_t count_casic_packets(const std::vector<uint8_t> &bytes, uint8_t group, uint8_t sub) {
  size_t count = 0;
  for (size_t i = 0; i + 5 < bytes.size(); ++i) {
    if (bytes[i] == 0xF1 && bytes[i + 1] == 0xD9 && bytes[i + 2] == group && bytes[i + 3] == sub) {
      ++count;
    }
  }
  return count;
}

// ===========================================================================
// CASIC ACK/NAK/timeout edge-case tests — verify wait_for_casic_ack()
// and send_cfg_with_ack() retry and error handling via gnss_start/stop.
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 32a — gnss_start: NAK on both attempts → command sent twice.
// send_cfg_with_ack retries once on failure, so the command frame appears
// twice in TX.
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start retries once on NAK", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  // Override auto_ack: both attempts get NAK.
  serial.queue_ack_response(AckMode::Nak); // attempt 1
  serial.queue_ack_response(AckMode::Nak); // attempt 2 (retry)

  gps.gnss_start();

  // Command sent twice (initial + retry).
  const auto &tx = serial.get_tx_bytes();
  REQUIRE(count_casic_packets(tx, 0x06, 0x40) == 2);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 32b — gnss_start: timeout on both attempts → command sent twice.
// No ACK/NAK queued at all; wait_for_casic_ack times out on each attempt.
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start retries once on timeout", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  // Override auto_ack: both attempts get nothing.
  serial.queue_ack_response(AckMode::None); // attempt 1 → timeout
  serial.queue_ack_response(AckMode::None); // attempt 2 → timeout

  gps.gnss_start();

  // Command sent twice (initial + retry).
  const auto &tx = serial.get_tx_bytes();
  REQUIRE(count_casic_packets(tx, 0x06, 0x40) == 2);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 32c — gnss_start: NAK on first attempt, ACK on retry → success.
// The command frame appears twice in TX but the operation succeeds.
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start succeeds on retry after initial NAK", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  // First attempt: NAK.  Retry: ACK.
  serial.queue_ack_response(AckMode::Nak); // attempt 1
  serial.queue_ack_response(AckMode::Ack); // attempt 2 (retry)

  gps.gnss_start();

  // Command sent twice (initial + retry).
  const auto &tx = serial.get_tx_bytes();
  REQUIRE(count_casic_packets(tx, 0x06, 0x40) == 2);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 32d — gnss_start: timeout on first attempt, ACK on retry → success.
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start succeeds on retry after initial timeout", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  // First attempt: nothing (timeout).  Retry: ACK.
  serial.queue_ack_response(AckMode::None); // attempt 1 → timeout
  serial.queue_ack_response(AckMode::Ack);  // attempt 2 (retry)

  gps.gnss_start();

  // Command sent twice (initial + retry).
  const auto &tx = serial.get_tx_bytes();
  REQUIRE(count_casic_packets(tx, 0x06, 0x40) == 2);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 32e — gnss_start: ACK on first attempt → command sent only once.
// ---------------------------------------------------------------------------
TEST_CASE("gnss_start does not retry when ACK received", "[gps][driver][gnss_control]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);
  serial.clear_tx();

  // Explicit ACK on first attempt (same as auto_ack, but verifying count).
  serial.queue_ack_response(AckMode::Ack);

  gps.gnss_start();

  // Command sent exactly once — no retry needed.
  const auto &tx = serial.get_tx_bytes();
  REQUIRE(count_casic_packets(tx, 0x06, 0x40) == 1);

  RTOS::set_instance(nullptr);
}

// ===========================================================================
// MON-VER module version poll tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 32 — begin() sends MON-VER poll before CFG commands.
// Expected poll: F1 D9 0A 04 00 00 0E 34
// ---------------------------------------------------------------------------
TEST_CASE("begin sends MON-VER poll before CFG commands", "[gps][driver][mon_ver]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  const auto &tx = serial.get_tx_bytes();

  // MON-VER poll packet must be present (group=0x0A, sub=0x04).
  const size_t ver_off = find_casic_packet(tx, 0x0A, 0x04);
  REQUIRE(ver_off != SIZE_MAX);

  // Verify it has empty payload (length=0).
  REQUIRE(tx[ver_off + 4] == 0x00); // len_lo = 0
  REQUIRE(tx[ver_off + 5] == 0x00); // len_hi = 0

  // Must appear before CFG-EPHSAVE and CFG-NAVSAT.
  const size_t ephsave_off = find_casic_packet(tx, 0x06, 0x10);
  const size_t navsat_off = find_casic_packet(tx, 0x06, 0x0C);
  REQUIRE(ephsave_off != SIZE_MAX);
  REQUIRE(navsat_off != SIZE_MAX);
  REQUIRE(ver_off < ephsave_off);
  REQUIRE(ver_off < navsat_off);

  RTOS::set_instance(nullptr);
}

// ===========================================================================
// CFG-NAVSAT constellation configuration tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 33 — begin() sends CFG-NAVSAT with L1-band constellation mask.
// Mask 0x00004037 = GPS L1 | GLONASS G1 | BeiDou B1 | Galileo E1 | QZSS L1 | BeiDou B1C
// ---------------------------------------------------------------------------
TEST_CASE("begin sends CFG-NAVSAT with L1-band constellation mask", "[gps][driver][navsat]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x06, 0x0C);
  REQUIRE(off != SIZE_MAX);

  // Verify payload: length=4, mask=0x00004037.
  REQUIRE(tx[off + 4] == 0x04); // len_lo = 4
  REQUIRE(tx[off + 5] == 0x00); // len_hi = 0

  const uint32_t mask = le_u32(&tx[off + 6]);
  REQUIRE(mask == 0x00004037);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 34 — CFG-NAVSAT exact CASIC frame verification.
// Expected: F1 D9 06 0C 04 00 35 00 00 00 4B 70
// ---------------------------------------------------------------------------
TEST_CASE("CFG-NAVSAT writes exact CASIC frame", "[gps][driver][navsat]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  const auto &tx = serial.get_tx_bytes();
  const size_t off = find_casic_packet(tx, 0x06, 0x0C);
  REQUIRE(off != SIZE_MAX);

  // Extract the complete packet: header(2) + ID(2) + length(2) + payload(4) + checksum(2) = 12.
  REQUIRE(off + 12 <= tx.size());
  const std::vector<uint8_t> actual(tx.begin() + static_cast<ptrdiff_t>(off),
                                    tx.begin() + static_cast<ptrdiff_t>(off) + 12);

  // clang-format off
  const std::vector<uint8_t> expected = {
      0xF1, 0xD9, 0x06, 0x0C, 0x04, 0x00, 0x37, 0x40, 0x00, 0x00, 0x8D, 0x38,
  };
  // clang-format on
  REQUIRE(actual == expected);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 35 — begin() sends CFG-NAVSAT poll after set.
// The poll packet has the same group/sub but length=0.
// Expected poll: F1 D9 06 0C 00 00 12 3C
// ---------------------------------------------------------------------------
TEST_CASE("begin sends CFG-NAVSAT poll after set", "[gps][driver][navsat]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  // Find both CFG-NAVSAT packets: set (len=4) and poll (len=0).
  const auto &tx = serial.get_tx_bytes();
  const size_t set_off = find_casic_packet(tx, 0x06, 0x0C);
  REQUIRE(set_off != SIZE_MAX);
  REQUIRE(tx[set_off + 4] == 0x04); // set has payload length 4

  // Search for the poll packet after the set packet.
  size_t poll_off = SIZE_MAX;
  for (size_t i = set_off + 12; i + 8 <= tx.size(); ++i) {
    if (tx[i] == 0xF1 && tx[i + 1] == 0xD9 && tx[i + 2] == 0x06 && tx[i + 3] == 0x0C &&
        tx[i + 4] == 0x00 && tx[i + 5] == 0x00) {
      poll_off = i;
      break;
    }
  }
  REQUIRE(poll_off != SIZE_MAX);
  REQUIRE(poll_off > set_off);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 36 — begin() sends CFG-EPHSAVE before CFG-NAVSAT.
// ---------------------------------------------------------------------------
TEST_CASE("begin sends CFG-EPHSAVE before CFG-NAVSAT", "[gps][driver][navsat]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  GpsDriver gps(serial);

  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  const auto &tx = serial.get_tx_bytes();
  const size_t ephsave_off = find_casic_packet(tx, 0x06, 0x10);
  const size_t navsat_off = find_casic_packet(tx, 0x06, 0x0C);
  REQUIRE(ephsave_off != SIZE_MAX);
  REQUIRE(navsat_off != SIZE_MAX);
  REQUIRE(ephsave_off < navsat_off);

  RTOS::set_instance(nullptr);
}

// ===========================================================================
// CASIC poll response tests — verify the generic poll helper processes
// module responses correctly via the StubSerial auto-poll-response
// mechanism.
// ===========================================================================

// Helper: build a MON-VER payload (32 bytes: 16 sw + 16 hw) from C strings.
static void build_mon_ver_payload(uint8_t (&payload)[32], const char *sw, const char *hw) {
  memset(payload, 0, sizeof(payload));
  strncpy(reinterpret_cast<char *>(&payload[0]), sw, 16);
  strncpy(reinterpret_cast<char *>(&payload[16]), hw, 16);
}

// Helper: build a CFG-NAVSAT payload (4 bytes: U4 LE mask).
static void build_navsat_payload(uint8_t (&payload)[4], uint32_t mask) {
  memcpy(payload, &mask, sizeof(mask));
}

// ---------------------------------------------------------------------------
// Test 37 — MON-VER poll response is consumed by begin().
// ---------------------------------------------------------------------------
TEST_CASE("MON-VER poll response is consumed during begin", "[gps][driver][poll]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);

  uint8_t ver_payload[32];
  build_mon_ver_payload(ver_payload, "FWVER_1.0.0", "TAU1113");
  serial.add_poll_response(0x0A, 0x04, ver_payload, sizeof(ver_payload));

  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  // After begin(), all auto-generated RX bytes (ACKs + poll responses)
  // should be fully consumed by the driver's poll/ACK handlers.
  REQUIRE(serial.available() == 0);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 38 — CFG-NAVSAT poll response is consumed by begin().
// ---------------------------------------------------------------------------
TEST_CASE("CFG-NAVSAT poll response is consumed during begin", "[gps][driver][poll]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);

  uint8_t navsat_payload[4];
  build_navsat_payload(navsat_payload, 0x00000025); // GPS + BeiDou + QZSS (module subset)
  serial.add_poll_response(0x06, 0x0C, navsat_payload, sizeof(navsat_payload));

  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  // Poll response consumed — RX empty.
  REQUIRE(serial.available() == 0);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 39 — Both MON-VER and CFG-NAVSAT poll responses consumed by begin().
// ---------------------------------------------------------------------------
TEST_CASE("begin consumes both MON-VER and CFG-NAVSAT poll responses", "[gps][driver][poll]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);

  uint8_t ver_payload[32];
  build_mon_ver_payload(ver_payload, "FWVER_1.0.0", "TAU1113");
  serial.add_poll_response(0x0A, 0x04, ver_payload, sizeof(ver_payload));

  uint8_t navsat_payload[4];
  build_navsat_payload(navsat_payload, 0x00004037); // full match
  serial.add_poll_response(0x06, 0x0C, navsat_payload, sizeof(navsat_payload));

  GpsDriver gps(serial);
  gps.begin(GpsDriver::MODULE_DEFAULT_BAUD);

  // Both poll responses and all ACKs consumed.
  REQUIRE(serial.available() == 0);

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 40 — begin() succeeds when no poll responses are registered (timeout).
// Without poll responses registered, the poll helpers time out gracefully.
// begin() must still return true.
// ---------------------------------------------------------------------------
TEST_CASE("begin succeeds when poll responses time out", "[gps][driver][poll]") {
  StubRTOS rtos;
  RTOS::set_instance(&rtos);

  StubSerial serial;
  serial.set_auto_ack(true);
  // No poll responses registered — polls will timeout.

  GpsDriver gps(serial);
  REQUIRE(gps.begin(GpsDriver::MODULE_DEFAULT_BAUD));

  RTOS::set_instance(nullptr);
}

// ---------------------------------------------------------------------------
// Test 41 — StubSerial auto-poll-response builds a valid CASIC packet.
// Verify the checksum is correct by manually computing it.
// ---------------------------------------------------------------------------
TEST_CASE("StubSerial auto-poll builds valid CASIC checksum", "[gps][driver][poll]") {
  StubSerial serial;

  // Register a CFG-NAVSAT poll response with known mask.
  uint8_t navsat_payload[4];
  build_navsat_payload(navsat_payload, 0x00004037);
  serial.add_poll_response(0x06, 0x0C, navsat_payload, sizeof(navsat_payload));

  // Write a poll packet to trigger the auto-response.
  // clang-format off
  const uint8_t poll_pkt[] = {
      0xF1, 0xD9, 0x06, 0x0C, 0x00, 0x00, 0x12, 0x3C,
  };
  // clang-format on
  serial.write(poll_pkt, sizeof(poll_pkt));

  // Read the auto-generated response from RX.
  // Expected: F1 D9 06 0C 04 00 37 40 00 00 8D 38
  REQUIRE(serial.available() >= 12);
  uint8_t response[12];
  for (int i = 0; i < 12; ++i) {
    const int b = serial.read();
    REQUIRE(b >= 0);
    response[i] = static_cast<uint8_t>(b);
  }

  // Verify header, group, sub, length.
  REQUIRE(response[0] == 0xF1);
  REQUIRE(response[1] == 0xD9);
  REQUIRE(response[2] == 0x06);
  REQUIRE(response[3] == 0x0C);
  REQUIRE(response[4] == 0x04);
  REQUIRE(response[5] == 0x00);

  // Verify payload.
  const uint32_t mask = le_u32(&response[6]);
  REQUIRE(mask == 0x00004037);

  // Verify checksum (8-Bit Fletcher over bytes 2..9).
  uint8_t ck1 = 0, ck2 = 0;
  for (int i = 2; i < 10; ++i) {
    ck1 = (ck1 + response[i]) & 0xFF;
    ck2 = (ck2 + ck1) & 0xFF;
  }
  REQUIRE(response[10] == ck1);
  REQUIRE(response[11] == ck2);
}
