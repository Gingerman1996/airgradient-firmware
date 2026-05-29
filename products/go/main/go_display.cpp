#include "go_display.h"

#ifndef TEST_HOST

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

extern "C" {
#include "u8x8.h"
}

#include "rtos.h"

static constexpr const char *TAG = "go_display";

// ===========================================================================
// RTC display snapshot — persists across deep sleep
// ===========================================================================
//
// Survives deep sleep in RTC slow memory.  Written by prepare_for_sleep()
// and read by run_button_wake_path() on the next button wake.
//
// s_rtc_display_snapshot_valid is false on first power-on (zero-initialized
// RTC memory), so an uninitialized snapshot is never used.

RTC_DATA_ATTR static RtcDisplaySnapshot s_rtc_display_snapshot;
RTC_DATA_ATTR static bool s_rtc_display_snapshot_valid = false;

void save_rtc_display_snapshot(const DisplayValues &values) {
  s_rtc_display_snapshot.co2_ppm = values.co2_ppm;
  s_rtc_display_snapshot.pm25_ugm3 = values.pm25_ugm3;
  s_rtc_display_snapshot.temperature_c = values.temperature_c;
  s_rtc_display_snapshot.humidity_pct = values.humidity_pct;
  s_rtc_display_snapshot.tvoc_index = values.tvoc_index;
  s_rtc_display_snapshot.nox_index = values.nox_index;
  s_rtc_display_snapshot.pressure_hpa = values.pressure_hpa;
  s_rtc_display_snapshot.altitude_m = values.altitude_m;
  s_rtc_display_snapshot.battery_pct = values.battery_pct;
  s_rtc_display_snapshot.is_battery_charging = values.is_battery_charging;
  s_rtc_display_snapshot.gps_enabled = values.gps_enabled;
  s_rtc_display_snapshot.gps_fix = values.gps_fix;
  s_rtc_display_snapshot.tracking_active = values.tracking_active;
  s_rtc_display_snapshot.ble_enabled = values.ble_enabled;
  s_rtc_display_snapshot.use_fahrenheit = values.use_fahrenheit;
  s_rtc_display_snapshot.pm_use_usaqi = values.pm_use_usaqi;
  s_rtc_display_snapshot_valid = true;
}

bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out) {
  if (!s_rtc_display_snapshot_valid) {
    return false;
  }
  *snapshot_out = s_rtc_display_snapshot;
  return true;
}

// ===========================================================================
// Anonymous namespace: display driver + rendering helpers
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// Display driver — SSD1680 SPI protocol (file-local)
// ---------------------------------------------------------------------------

constexpr int WIDTH_PX = 128;
constexpr int HEIGHT_PX = 250;
constexpr int FRAME_BYTES = (WIDTH_PX * HEIGHT_PX) / 8; // 4000
constexpr size_t TX_BOUNCE_BYTES = 4096;

// Error-propagation macro for driver functions returning esp_err_t.
// Intentionally unscoped so it works in non-void-returning functions.
#define DISP_RETURN_ON_ERR(expr)                                                                   \
  do {                                                                                             \
    const esp_err_t _e = (expr);                                                                   \
    if (_e != ESP_OK)                                                                              \
      return _e;                                                                                   \
  } while (0)

struct DriverState {
  bool inited = false;
  bool bus_acquired = false;

  spi_host_device_t spi_host = SPI2_HOST;
  int spi_clock_hz = 4000000;

  gpio_num_t pin_cs = GPIO_NUM_NC;
  gpio_num_t pin_dc = GPIO_NUM_NC;
  gpio_num_t pin_rst = GPIO_NUM_NC;
  gpio_num_t pin_busy = GPIO_NUM_NC;

  spi_device_handle_t spi = nullptr;
  uint8_t *tx_bounce = nullptr;
};

DriverState g_driver;

// --- Low-level GPIO helpers ---

inline void set_cs(int level) { gpio_set_level(g_driver.pin_cs, level); }
inline void set_dc(int level) { gpio_set_level(g_driver.pin_dc, level); }
inline void set_rst(int level) { gpio_set_level(g_driver.pin_rst, level); }

void delay_ms(uint32_t ms) {
  if (ms == 0)
    return;
  RTOS::delay_ms(ms);
}

// BUSY=1 means controller processing; BUSY=0 means ready.
void wait_busy_low() {
  while (gpio_get_level(g_driver.pin_busy) != 0) {
    RTOS::delay_ms(1);
  }
}

// --- SPI transfer primitives ---

esp_err_t spi_write_byte(uint8_t value) {
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_USE_TXDATA;
  t.length = 8;
  t.tx_data[0] = value;
  return spi_device_polling_transmit(g_driver.spi, &t);
}

esp_err_t spi_write_bulk(const uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk = (len - offset > TX_BOUNCE_BYTES) ? TX_BOUNCE_BYTES : (len - offset);
    memcpy(g_driver.tx_bounce, data + offset, chunk);
    spi_transaction_t t = {};
    t.length = chunk * 8;
    t.tx_buffer = g_driver.tx_bounce;
    DISP_RETURN_ON_ERR(spi_device_polling_transmit(g_driver.spi, &t));
    offset += chunk;
  }
  return ESP_OK;
}

// --- SSD1680 command/data protocol ---

esp_err_t write_cmd(uint8_t command) {
  set_cs(0);
  set_dc(0);
  const esp_err_t err = spi_write_byte(command);
  set_cs(1);
  return err;
}

esp_err_t write_data(uint8_t data) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_byte(data);
  set_cs(1);
  return err;
}

esp_err_t write_data_bytes(const uint8_t *data, size_t len) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_bulk(data, len);
  set_cs(1);
  return err;
}

// --- Driver public API ---

esp_err_t driver_init(const DisplayService::Config &cfg) {
  if (g_driver.inited)
    return ESP_OK;

  g_driver.spi_host = cfg.spi_host;
  g_driver.spi_clock_hz = cfg.clock_hz;
  g_driver.pin_cs = static_cast<gpio_num_t>(cfg.pin_cs);
  g_driver.pin_dc = static_cast<gpio_num_t>(cfg.pin_dc);
  g_driver.pin_rst = static_cast<gpio_num_t>(cfg.pin_rst);
  g_driver.pin_busy = static_cast<gpio_num_t>(cfg.pin_busy);

  // Configure output GPIOs (CS, DC, RST)
  gpio_config_t out_cfg = {};
  out_cfg.pin_bit_mask = (1ULL << cfg.pin_cs) | (1ULL << cfg.pin_dc) | (1ULL << cfg.pin_rst);
  out_cfg.mode = GPIO_MODE_OUTPUT;
  out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&out_cfg));

  // Configure BUSY input
  gpio_config_t in_cfg = {};
  in_cfg.pin_bit_mask = 1ULL << cfg.pin_busy;
  in_cfg.mode = GPIO_MODE_INPUT;
  in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&in_cfg));

  set_cs(1);
  set_dc(1);
  set_rst(1);

  // Attach SPI device (CS managed manually, not by ESP-IDF driver)
  spi_device_interface_config_t devcfg = {};
  devcfg.mode = 0;
  devcfg.clock_speed_hz = cfg.clock_hz;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_NO_DUMMY;
  DISP_RETURN_ON_ERR(spi_bus_add_device(cfg.spi_host, &devcfg, &g_driver.spi));

  // DMA-capable bounce buffer for chunked SPI writes
  g_driver.tx_bounce = static_cast<uint8_t *>(heap_caps_malloc(TX_BOUNCE_BYTES, MALLOC_CAP_DMA));
  if (g_driver.tx_bounce == nullptr)
    return ESP_ERR_NO_MEM;

  delay_ms(100);
  g_driver.inited = true;
  ESP_LOGI(TAG, "EPD driver init done");
  return ESP_OK;
}

esp_err_t driver_bus_acquire() {
  if (!g_driver.inited || g_driver.spi == nullptr)
    return ESP_ERR_INVALID_STATE;
  if (g_driver.bus_acquired)
    return ESP_ERR_INVALID_STATE;

  // ESP-IDF spi_device_acquire_bus() only supports portMAX_DELAY (infinite
  // wait).  Finite timeouts return ESP_ERR_INVALID_ARG.
  DISP_RETURN_ON_ERR(spi_device_acquire_bus(g_driver.spi, portMAX_DELAY));
  g_driver.bus_acquired = true;
  return ESP_OK;
}

void driver_bus_release() {
  if (!g_driver.bus_acquired)
    return;
  if (g_driver.spi != nullptr) {
    spi_device_release_bus(g_driver.spi);
  }
  g_driver.bus_acquired = false;
}

// Full SSD1680 initialization sequence
esp_err_t driver_hw_init_full() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Software reset
  DISP_RETURN_ON_ERR(write_cmd(0x12));
  wait_busy_low();

  // Driver output control: gate lines = HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Data entry mode: X increment, Y decrement
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  // RAM-X window: 0 to 15 (0..127 pixels)
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(WIDTH_PX / 8 - 1)));

  // RAM-Y window: 249 to 0
  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (full mode)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x05));

  // Display update control
  DISP_RETURN_ON_ERR(write_cmd(0x21));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Built-in temperature sensor
  DISP_RETURN_ON_ERR(write_cmd(0x18));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // RAM-X counter to 0
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-Y counter to HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  wait_busy_low();

  return ESP_OK;
}

// Write frame to both SSD1680 RAM planes + trigger full update.
esp_err_t driver_set_basemap(const uint8_t *data) {
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));
  DISP_RETURN_ON_ERR(write_cmd(0x26));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));

  // Trigger full update
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xF7));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Prepare controller for partial update mode.
esp_err_t driver_part_begin() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Re-establish Y mapping after reset
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (partial mode — differs from full init)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Data entry mode (same as full init)
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  return ESP_OK;
}

// Write pixel data to a rectangular region for partial update.
// Coordinates are logical screen coordinates (Y=0 at top).
esp_err_t driver_part_write_region(unsigned int x_start_px, unsigned int y_start_px,
                                   const uint8_t *data, unsigned int height_px,
                                   unsigned int width_px) {
  const unsigned int x_start = x_start_px / 8;
  const unsigned int x_end = x_start + width_px / 8 - 1;

  // Map logical top-origin Y to controller Y (inverted).
  // Controller Y=249 corresponds to screen top (Y=0).
  const unsigned int y_start = (HEIGHT_PX - 1) - y_start_px;
  const unsigned int y_end = (HEIGHT_PX - 1) - (y_start_px + height_px - 1);

  // Set RAM window
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_end)));

  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end / 256)));

  // Set RAM cursor
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));

  // Write pixel data (new RAM plane only; partial uses diff against basemap)
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  return write_data_bytes(data, static_cast<size_t>(height_px) * width_px / 8);
}

// Trigger partial display update.
esp_err_t driver_part_commit() {
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xFF));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Fast refresh (0xC7 waveform) — full-screen, no flash
// ---------------------------------------------------------------------------
// Uses a temperature-override trick: force the SSD1680 to load the OTP LUT
// for 100 °C, which selects a shorter, more aggressive waveform. The 0xC7
// trigger then drives all pixels unconditionally (non-differential).

// Initialize controller for fast refresh mode.
// Must be called before driver_fast_write() + driver_fast_commit().
esp_err_t driver_hw_init_fast() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);

  // Software reset (clears all registers to defaults)
  DISP_RETURN_ON_ERR(write_cmd(0x12));
  wait_busy_low();

  // Select built-in temperature sensor
  DISP_RETURN_ON_ERR(write_cmd(0x18));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Load actual temperature value into register
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xB1));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();

  // Override temperature register to 100 °C → selects faster OTP LUT
  DISP_RETURN_ON_ERR(write_cmd(0x1A));
  DISP_RETURN_ON_ERR(write_data(0x64)); // 100 °C
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Reload LUT using the overridden temperature
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0x91));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();

  // Re-establish display geometry (SW reset cleared these).
  // Must match driver_hw_init_full() for our frame buffer layout.

  // Driver output control: gate lines = HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Data entry mode: X increment, Y decrement
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  // RAM-X window: 0 to 15 (0..127 pixels)
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(WIDTH_PX / 8 - 1)));

  // RAM-Y window: 249 to 0
  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-X counter to 0
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-Y counter to HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));

  return ESP_OK;
}

// Write full frame to both RAM planes for fast refresh.
// Writes 0x24 (new) and 0x26 (old/basemap) with identical data so that
// subsequent partial refreshes (which diff 0x24 vs 0x26) remain coherent.
esp_err_t driver_fast_write(const uint8_t *data) {
  // Write image to new RAM plane (0x24)
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));

  // Reset RAM cursor for second plane write
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));

  // Write same image to old RAM plane (0x26) — basemap coherence
  DISP_RETURN_ON_ERR(write_cmd(0x26));
  return write_data_bytes(data, FRAME_BYTES);
}

// Trigger fast display update (0xC7 waveform — non-differential, no flash).
esp_err_t driver_fast_commit() {
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xC7));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Put SSD1680 into deep sleep mode 1.
esp_err_t driver_deep_sleep() {
  DISP_RETURN_ON_ERR(write_cmd(0x10));
  DISP_RETURN_ON_ERR(write_data(0x01));
  delay_ms(100);
  return ESP_OK;
}

#undef DISP_RETURN_ON_ERR

// ---------------------------------------------------------------------------
// Layout constants — see products/go/docs/display_service.md
// ---------------------------------------------------------------------------

constexpr int SCREEN_W = 128;
constexpr int CONTENT_W = 122; // preserved for non-home screens (snackbar, list, etc.)
constexpr int BODY_Y = 18;
constexpr int BODY_H = 232;

// Status bar
constexpr int STATUS_DIVIDER_Y = 17;
constexpr int ICON_GAP = 2;
constexpr int LOCK_X = 6;
constexpr int BATTERY_X = 116;
constexpr int TRACKING_X = 109;
constexpr int STATUS_BASELINE_Y = 12;
constexpr int ELLIPSE_CY = 8;
constexpr int LINK_CY = 5;

// Home screen: hero blocks
constexpr int PM_BLOCK_Y = 18;
constexpr int PM_BLOCK_H = 72;
constexpr int PM_LABEL_BASELINE_Y = 44;
constexpr int PM_VALUE_BASELINE_Y = 84;
constexpr int CO2_BLOCK_Y = 90;
constexpr int CO2_BLOCK_H = 72;
constexpr int CO2_LABEL_NAME_BASELINE_Y = 112;
constexpr int CO2_LABEL_UNIT_BASELINE_Y = 113;
constexpr int CO2_VALUE_BASELINE_Y = 153;

// Home screen: grid
constexpr int GRID_DIVIDER_0_Y = 162; // between CO2 and row 1
constexpr int GRID_DIVIDER_1_Y = 192; // between row 1 and row 2
constexpr int GRID_DIVIDER_2_Y = 222; // between row 2 and row 3
constexpr int GRID_DIVIDER_X = 64;

// Display-off
constexpr int DISPLAY_OFF_LOGO_Y = 113;
constexpr int DISPLAY_OFF_LOGO_H = 24;

// Chart — shifted 1px down for 2px-thick 3rd grid divider when active
constexpr int PLOT_X = 4;
constexpr int PLOT_Y = 225;
constexpr int PLOT_W = 121; // x=4..124 → 121 pixels
constexpr int PLOT_H = 25;  // y=225..248 → 25 pixels

// Snackbar (unchanged)
constexpr int SNACKBAR_Y = 232;
constexpr int SNACKBAR_H = 18;
constexpr int SNACKBAR_TEXT_BASELINE_Y = 244;

// Menu overlays
constexpr int MAIN_MENU_BG_Y = 162;
constexpr int MAIN_MENU_BG_H = 88;
constexpr int FULL_SCREEN_BG_Y = 18;
constexpr int FULL_SCREEN_BG_H = 232;

// Grid cell label/value positions
constexpr int GRID_LABEL_X[6] = {7, 68, 7, 68, 7, 68};
constexpr int GRID_LABEL_Y[6] = {175, 175, 205, 205, 235, 235};
constexpr int GRID_VALUE_X[6] = {7, 68, 7, 68, 7, 68};
constexpr int GRID_VALUE_Y[6] = {187, 187, 217, 217, 247, 247};

// Selection rectangles — shifted 1px down for 2px-thick 1st grid divider
constexpr int SEL_TEMP_X = 0;
constexpr int SEL_TEMP_Y = 164;
constexpr int SEL_TEMP_W = 64;
constexpr int SEL_TEMP_H = 28;
constexpr int SEL_HUM_X = 64;
constexpr int SEL_HUM_Y = 164;
constexpr int SEL_HUM_W = 64;
constexpr int SEL_HUM_H = 28;

// ---------------------------------------------------------------------------
// u8g2 virtual display callback — provides geometry info only
// ---------------------------------------------------------------------------

const u8x8_display_info_t U8X8_DISPLAY_INFO_EPD_128X250 = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 0,
    /* post_reset_wait_ms = */ 0,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ 10000000UL,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 0,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ 16,
    /* tile_hight = */ 32,
    /* default_x_offset = */ 0,
    /* flipmode_x_offset = */ 0,
    /* pixel_width = */ 128,
    /* pixel_height = */ 250,
};

uint8_t u8x8_d_epd_128x250_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  (void)arg_int;
  (void)arg_ptr;
  switch (msg) {
  case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    u8x8_d_helper_display_setup_memory(u8x8, &U8X8_DISPLAY_INFO_EPD_128X250);
    return 1;
  default:
    return 1;
  }
}

// ---------------------------------------------------------------------------
// Helper predicates
// ---------------------------------------------------------------------------

bool is_home_like(Screen screen) { return screen == Screen::Home || screen == Screen::MainMenu; }

bool is_list_screen(Screen screen) {
  return screen == Screen::Settings || screen == Screen::SettingsChoice ||
         screen == Screen::TagList || screen == Screen::Confirm || screen == Screen::About;
}

// A "navigable" screen is one the user reaches through normal menu interaction.
// Transitions between navigable screens use body-only partial for snappy UX.
// Screens NOT listed here (PairingPasskey, Shutdown) trigger Fast on transition.
bool is_navigable(Screen screen) { return is_home_like(screen) || is_list_screen(screen); }

// A "menu-navigation" screen is any navigable screen except Home.
// Transitions where either side is menu-navigation always use Partial.
bool is_menu_navigation_screen(Screen screen) {
  return is_navigable(screen) && screen != Screen::Home;
}

bool metric_has_chart(Metric metric) { return metric != Metric::None; }

bool is_float_non_negative(float value) { return value >= 0.0f; }

bool is_temperature_valid(float celsius) { return celsius > MeasuresInvalid::TEMPERATURE; }

bool is_altitude_valid(float altitude_m) { return altitude_m > -10000.0f; }

float temp_for_display(float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius))
    return celsius;
  if (!use_fahrenheit)
    return celsius;
  return celsius * 9.0f / 5.0f + 32.0f;
}

// ---------------------------------------------------------------------------
// Value formatting (inline during rendering)
// ---------------------------------------------------------------------------

int pm25_to_usaqi(float pm25) {
  struct Breakpoint {
    float c_low;
    float c_high;
    int i_low;
    int i_high;
  };
  static const Breakpoint BP[] = {
      {0.0f, 12.0f, 0, 50},       {12.1f, 35.4f, 51, 100},    {35.5f, 55.4f, 101, 150},
      {55.5f, 150.4f, 151, 200},  {150.5f, 250.4f, 201, 300}, {250.5f, 350.4f, 301, 400},
      {350.5f, 500.4f, 401, 500},
  };
  if (!(pm25 >= 0.0f))
    return -1;
  for (const auto &bp : BP) {
    if (pm25 <= bp.c_high) {
      const float ratio = (pm25 - bp.c_low) / (bp.c_high - bp.c_low);
      return bp.i_low + static_cast<int>(lroundf(ratio * static_cast<float>(bp.i_high - bp.i_low)));
    }
  }
  return 500;
}

void format_one_decimal(char *out, size_t out_size, float value) {
  const int scaled = static_cast<int>(lroundf(value * 10.0f));
  const bool neg = scaled < 0;
  const unsigned int abs_scaled = static_cast<unsigned int>(neg ? -scaled : scaled);
  snprintf(out, out_size, "%s%u.%01u", neg ? "-" : "", abs_scaled / 10U, abs_scaled % 10U);
}

void format_pm_value(char *out, size_t out_size, float value, bool use_usaqi) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (use_usaqi) {
    const int aqi = pm25_to_usaqi(value);
    if (aqi > 500) {
      snprintf(out, out_size, "500+");
      return;
    }
    snprintf(out, out_size, "%d", aqi);
    return;
  }
  if (value > 999.0f) {
    snprintf(out, out_size, "999+");
    return;
  }
  if (value >= 100.0f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_co2_value(char *out, size_t out_size, int value) {
  if (value == MeasuresInvalid::CO2) {
    snprintf(out, out_size, "-");
    return;
  }
  if (value > 9999) {
    snprintf(out, out_size, "9999+");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

void format_temperature_value(char *out, size_t out_size, float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius)) {
    snprintf(out, out_size, "-");
    return;
  }
  const float shown = temp_for_display(celsius, use_fahrenheit);
  snprintf(out, out_size, "%.1f %c", shown, use_fahrenheit ? 'F' : 'C');
}

void format_humidity_value(char *out, size_t out_size, float humidity) {
  if (!is_float_non_negative(humidity)) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%.0f %%", humidity);
}

// Integer index display for TVOC/NOx grid cells.
void format_int_index_value(char *out, size_t out_size, int value) {
  if (value < 0) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

// Float index display for chart min/max stats.
void format_float_index_value(char *out, size_t out_size, float value) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (fabsf(value - roundf(value)) < 0.05f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_pressure_value(char *out, size_t out_size, float pressure_hpa) {
  if (!is_float_non_negative(pressure_hpa)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (pressure_hpa > 9999.0f) {
    snprintf(out, out_size, "9999+ hPa");
    return;
  }
  snprintf(out, out_size, "%.0f hPa", pressure_hpa);
}

void format_altitude_value(char *out, size_t out_size, float altitude_m) {
  if (!is_altitude_valid(altitude_m)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (altitude_m > 9999.0f) {
    snprintf(out, out_size, "9999+ m");
    return;
  }
  snprintf(out, out_size, "%.0f m", altitude_m);
}

void format_chart_stat(char *out, size_t out_size, Metric metric, float value, bool use_fahrenheit,
                       bool pm_use_usaqi) {
  switch (metric) {
  case Metric::Pm25:
    format_pm_value(out, out_size, value, pm_use_usaqi);
    break;
  case Metric::Co2:
    format_co2_value(out, out_size, static_cast<int>(lroundf(value)));
    break;
  case Metric::Temp:
    format_temperature_value(out, out_size, value, use_fahrenheit);
    break;
  case Metric::Humidity:
    format_humidity_value(out, out_size, value);
    break;
  case Metric::Tvoc:
  case Metric::Nox:
    format_float_index_value(out, out_size, value);
    break;
  case Metric::None:
  default:
    snprintf(out, out_size, "-");
    break;
  }
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void draw_text(u8g2_t *u, int x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  u8g2_DrawStr(u, static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(baseline_y), text);
}

void draw_centered_text(u8g2_t *u, int center_x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  const int w = static_cast<int>(u8g2_GetStrWidth(u, text));
  draw_text(u, center_x - w / 2, baseline_y, text);
}

void draw_logo(u8g2_t *u, int y, int h) {
  u8g2_SetFont(u, u8g2_font_6x10_tr);
  draw_centered_text(u, CONTENT_W / 2, y + h / 2 + 4, "AirGradient");
}

// Battery glyphs from u8g2_font_siji_t_6x10
constexpr uint16_t BATTERY_GLYPH_CHARGING = 57914;
constexpr uint16_t BATTERY_GLYPH_LVL_0_10 = 57932;
constexpr uint16_t BATTERY_GLYPH_LVL_10_30 = 57933;
constexpr uint16_t BATTERY_GLYPH_LVL_30_50 = 57935;
constexpr uint16_t BATTERY_GLYPH_LVL_50_70 = 57937;
constexpr uint16_t BATTERY_GLYPH_LVL_70_95 = 57939;
constexpr uint16_t BATTERY_GLYPH_LVL_95_100 = 57940;

uint16_t battery_glyph(bool is_charging, uint8_t pct) {
  if (is_charging)
    return BATTERY_GLYPH_CHARGING;
  const uint8_t v = (pct > 100U) ? 100U : pct;
  if (v <= 10U)
    return BATTERY_GLYPH_LVL_0_10;
  if (v <= 30U)
    return BATTERY_GLYPH_LVL_10_30;
  if (v <= 50U)
    return BATTERY_GLYPH_LVL_30_50;
  if (v <= 70U)
    return BATTERY_GLYPH_LVL_50_70;
  if (v <= 95U)
    return BATTERY_GLYPH_LVL_70_95;
  return BATTERY_GLYPH_LVL_95_100;
}

// Draw a single grid cell (label in helvR08, value in helvB08).
// For temperature, caller should use DrawUTF8 directly for the degree symbol.
void draw_cell(u8g2_t *u, int index, const char *label, const char *value, bool use_utf8) {
  u8g2_SetFont(u, u8g2_font_helvR08_tr);
  draw_text(u, GRID_LABEL_X[index], GRID_LABEL_Y[index], label);
  u8g2_SetFont(u, u8g2_font_helvB08_tf);
  if (use_utf8) {
    u8g2_DrawUTF8(u, static_cast<u8g2_uint_t>(GRID_VALUE_X[index]),
                  static_cast<u8g2_uint_t>(GRID_VALUE_Y[index]), value);
  } else {
    draw_text(u, GRID_VALUE_X[index], GRID_VALUE_Y[index], value);
  }
}

// Draw list/menu rows with selection highlight.
void draw_list_rows(u8g2_t *u, const DisplayValues &v, bool full_screen) {
  const int row_base_y = full_screen ? 20 : 166;
  constexpr int ROW_RECT_H = 20;
  constexpr int ROW_STEP = 22;
  const int text_baseline = full_screen ? 33 : 179;
  // Extra offset for content rows (index >= 2) to clear the separator line.
  const int content_pad = (full_screen && v.show_separator_after_back) ? 2 : 0;

  for (uint8_t i = 0; i < v.row_count && i < MAX_LIST_ROWS; ++i) {
    const int pad = (i >= 2) ? content_pad : 0;
    const int row_y = row_base_y + ROW_STEP * static_cast<int>(i) + pad;
    const bool selected = (i == v.selected_row) && !v.rows[i].disabled;
    if (selected) {
      u8g2_DrawBox(u, 0, row_y, SCREEN_W, ROW_RECT_H);
      u8g2_SetDrawColor(u, 1);
    }
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    draw_text(u, 10, text_baseline + ROW_STEP * static_cast<int>(i) + pad, v.rows[i].text);
    if (selected) {
      u8g2_SetDrawColor(u, 0);
    }
  }

  if (v.show_separator_after_back) {
    u8g2_DrawHLine(u, 0, 63, SCREEN_W);
  }
}

} // anonymous namespace

// ===========================================================================
// DisplayService — public methods
// ===========================================================================

DisplayService::DisplayService(const Config &config)
    : _config(config), _u8g2{}, _render_buf{}, _spi_buf{}, _region_buf{}, _prev_values{},
      _diff_count(0), _pending_mode(RefreshMode::Full), _task_handle(nullptr), _running(false),
      _worker_busy(false) {}

bool DisplayService::init(const DisplayValues &initial, bool defer_refresh) {
  esp_err_t err = driver_init(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "driver init failed: %s", esp_err_to_name(err));
    return false;
  }

  // u8g2: software renderer into RAM only — no hardware callbacks
  u8g2_SetupDisplay(&_u8g2, u8x8_d_epd_128x250_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
  u8g2_SetupBuffer(&_u8g2, _render_buf, BUF_TILE_HEIGHT, u8g2_ll_hvline_horizontal_right_lsb,
                   U8G2_MIRROR);
  u8g2_SetFontMode(&_u8g2, 1); // Transparent font mode

  _prev_values = initial;
  _render_frame(initial);

  if (!defer_refresh) {
    // Synchronous initial full refresh (worker not yet started).
    // Default behavior: backward compatible with run_full_boot() and run_fast_path().
    err = driver_bus_acquire();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "bus acquire failed for init: %s", esp_err_to_name(err));
      return false;
    }
    err = driver_hw_init_full();
    if (err == ESP_OK) {
      err = driver_set_basemap(_render_buf);
    }
    driver_bus_release();

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "initial refresh failed: %s", esp_err_to_name(err));
      return false;
    }
    _diff_count = 0;
  } else {
    // Deferred refresh: render is already in _render_buf.  Copy to _spi_buf,
    // mark a full refresh pending, then start the worker and signal it to
    // perform the initial refresh in the background.  Returns in ~10 ms.
    // The worker acquires the SPI bus for ~3 s; any other SPI device (NAND)
    // that tries to transmit will block until the worker releases the bus.
    memcpy(_spi_buf, _render_buf, sizeof(_render_buf));
    _pending_mode = RefreshMode::Full;
    _worker_busy = true; // will be cleared by worker when refresh completes
    // The _prev_values header now reflects the snapshot-based initial frame,
    // which may differ from the live runtime state the orchestrator produces.
    // Skip the header-change penalty on the first update()
    _defer_header_check = true;
  }

  // Start async worker task.
  _running = true;
  const bool created = RTOS::task_create(
      _worker_entry, "disp_worker", static_cast<uint32_t>(_config.task_stack_size), this,
      static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!created) {
    ESP_LOGE(TAG, "failed to create worker task");
    _running = false;
    _task_handle = nullptr;
    return false;
  }

  if (defer_refresh) {
    // Kick the worker to process the initial full refresh immediately.
    RTOS::task_notify_give(_task_handle);
  }

  return true;
}

bool DisplayService::update(const DisplayValues &values, bool wait) {
  if (_task_handle == nullptr)
    return false;

  if (_worker_busy) {
    if (!wait)
      return false;
    while (_worker_busy) {
      RTOS::delay_ms(1);
    }
  }

  const bool same_list_screen =
      is_list_screen(_prev_values.screen) && _prev_values.screen == values.screen;
  const bool header_changed = _is_header_changed(values, _prev_values) && !_defer_header_check;
  _defer_header_check = false;
  const bool both_navigable = is_navigable(_prev_values.screen) && is_navigable(values.screen);
  const bool can_partial = (both_navigable && !header_changed) || same_list_screen;

  _render_frame(values);

  const bool entering_system_screen =
      values.screen == Screen::Shutdown || values.screen == Screen::PairingPasskey ||
      values.screen == Screen::DischargeComplete;
  const bool menu_navigation =
      !entering_system_screen &&
      (is_menu_navigation_screen(_prev_values.screen) || is_menu_navigation_screen(values.screen));

  if (menu_navigation) {
    _pending_mode = RefreshMode::Partial;
    _menu_exited = true;
  } else if (_diff_count >= _config.max_partial_ops) {
    _pending_mode = RefreshMode::Full;
  } else if (_menu_exited) {
    _pending_mode = RefreshMode::Fast;
  } else if (can_partial) {
    _pending_mode = RefreshMode::Partial;
  } else {
    _pending_mode = RefreshMode::Fast;
  }

  // Clear flag after any full-screen refresh (Full or Fast rewrites everything).
  if (_pending_mode != RefreshMode::Partial) {
    _menu_exited = false;
  }

  memcpy(_spi_buf, _render_buf, sizeof(_render_buf));
  _worker_busy = true;
  _prev_values = values;

  RTOS::task_notify_give(_task_handle);
  return true;
}

void DisplayService::update_sync(const DisplayValues &values) {
  _render_frame(values);

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for sync update: %s", esp_err_to_name(err));
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_render_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sync update failed: %s", esp_err_to_name(err));
  }
  _diff_count = 0;
  _prev_values = values;
}

void DisplayService::clear() {
  memset(_render_buf, 0xFF, sizeof(_render_buf));

  // Wait for worker to finish if active
  while (_worker_busy) {
    RTOS::delay_ms(1);
  }

  memcpy(_spi_buf, _render_buf, sizeof(_render_buf));

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for clear: %s", esp_err_to_name(err));
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_spi_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "clear failed: %s", esp_err_to_name(err));
  }
  _diff_count = 0;
}

void DisplayService::deep_sleep() {
  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for deep sleep: %s", esp_err_to_name(err));
    return;
  }
  err = driver_deep_sleep();
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "deep sleep failed: %s", esp_err_to_name(err));
  }
}

void DisplayService::stop() {
  if (_task_handle == nullptr)
    return;
  _running = false;
  RTOS::task_notify_give(_task_handle); // Wake worker so it can exit

  // Wait for worker to finish current operation
  while (_worker_busy) {
    RTOS::delay_ms(1);
  }
  RTOS::delay_ms(50); // Let worker task fully exit
  _task_handle = nullptr;
}

// ===========================================================================
// DisplayService — rendering pipeline
// ===========================================================================

void DisplayService::_render_frame(const DisplayValues &v) {
  memset(_render_buf, 0xFF, sizeof(_render_buf));
  u8g2_SetDrawColor(&_u8g2, 0); // Black on white

  if (v.screen == Screen::Shutdown) {
    _draw_shutdown();
    _draw_snackbar(v);
    return;
  }

  if (v.screen == Screen::DischargeComplete) {
    _draw_discharge_complete();
    return;
  }

  // Automated battery-learning terminal result screens — full-screen pass/fail
  // (design §3.5).  The active phases (Charging/Resting/Unplug/Verifying) fall
  // through to the live power dashboard below so the operator sees the FG
  // readout (SOC, V, I, FCC drift) with the stage shown in the dashboard banner.
  if (v.screen == Screen::BlearnComplete) {
    _draw_blearn_phase("Battery learning", "Complete");
    return;
  }
  if (v.screen == Screen::BlearnFailed) {
    _draw_blearn_phase("Battery learning", "Failed");
    return;
  }

  _draw_status_bar(v);

  switch (v.screen) {
  case Screen::Home:
    _draw_home(v);
    break;
  case Screen::MainMenu:
    _draw_home(v);
    _draw_menu_overlay(v);
    break;
  case Screen::Settings:
  case Screen::SettingsChoice:
  case Screen::TagList:
  case Screen::Confirm:
  case Screen::About:
    _draw_full_screen_list(v);
    break;
  case Screen::BlearnCharging:
  case Screen::BlearnResting:
  case Screen::BlearnUnplug:
  case Screen::BlearnVerifying:
    _draw_power_dashboard(v);
    break;
  case Screen::Shutdown:
  case Screen::DischargeComplete:
  case Screen::BlearnComplete:
  case Screen::BlearnFailed:
    break; // Already handled above
  case Screen::PairingPasskey:
    _draw_pairing_passkey(v);
    break;
  }

  _draw_snackbar(v);
}

bool DisplayService::_is_header_changed(const DisplayValues &a, const DisplayValues &b) const {
  return a.battery_pct != b.battery_pct || a.is_battery_charging != b.is_battery_charging ||
         a.locked != b.locked || a.ble_enabled != b.ble_enabled ||
         a.ble_connected != b.ble_connected || a.wifi_enabled != b.wifi_enabled ||
         a.gps_enabled != b.gps_enabled || a.gps_fix != b.gps_fix ||
         a.tracking_active != b.tracking_active;
}

// ===========================================================================
// DisplayService — screen draw methods
// ===========================================================================

void DisplayService::_draw_status_bar(const DisplayValues &v) {
  int cursor = LOCK_X;

  // 1. Lock or Unlock icon (left-pinned, always drawn)
  if (v.locked) {
    u8g2_SetFont(&_u8g2, u8g2_font_open_iconic_all_1x_t);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0xCA);
  } else {
    u8g2_SetFont(&_u8g2, u8g2_font_open_iconic_thing_1x_t);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0x44);
  }
  cursor += 10 + ICON_GAP;

  // 2. Dynamic flow icons: WiFi, Link/Unlink, GPS
  if (v.wifi_enabled) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0xE21A);
    cursor += 10 + ICON_GAP;
  }

  if (v.ble_enabled) {
    const int x = cursor;
    if (v.ble_connected) {
      // Link: two frames + connecting bar
      u8g2_DrawFrame(&_u8g2, x, LINK_CY, 5, 5);
      u8g2_DrawFrame(&_u8g2, x + 6, LINK_CY, 5, 5);
      u8g2_DrawLine(&_u8g2, x + 3, LINK_CY + 2, x + 7, LINK_CY + 2);
    } else {
      // Unlink: two frames + broken fragments
      u8g2_DrawFrame(&_u8g2, x, LINK_CY, 5, 5);
      u8g2_DrawFrame(&_u8g2, x + 6, LINK_CY, 5, 5);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY + 4, x + 3, LINK_CY + 4);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY + 4, x + 7, LINK_CY + 4);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY - 2, x + 3, LINK_CY - 3);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY - 2, x + 7, LINK_CY - 3);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY + 6, x + 3, LINK_CY + 7);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY + 6, x + 7, LINK_CY + 7);
    }
    cursor += 12 + ICON_GAP;
  }

  if (v.gps_fix) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0xE0A1);
    cursor += 10 + ICON_GAP;
  }

  // 3. Tracking dot (right-pinned, fixed position)
  if (v.tracking_active) {
    u8g2_DrawFilledEllipse(&_u8g2, TRACKING_X, ELLIPSE_CY, 2, 2, U8G2_DRAW_ALL);
  }

  // 4. Battery (right-pinned, fixed position)
  if (v.battery_pct != 0xFFu || v.is_battery_charging) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, BATTERY_X, STATUS_BASELINE_Y,
                   battery_glyph(v.is_battery_charging, v.battery_pct));
  }

  // Header divider
  u8g2_DrawHLine(&_u8g2, 0, STATUS_DIVIDER_Y, SCREEN_W);
}

void DisplayService::_draw_home(const DisplayValues &v) {
  if (v.display_off) {
    draw_logo(&_u8g2, DISPLAY_OFF_LOGO_Y, DISPLAY_OFF_LOGO_H);
    return;
  }

  // Admin Battery Learning mode replaces the sensor grid with a power
  // dashboard.  Only the home screen is affected — menus, settings, etc.
  // render normally on top of whatever the underlying screen is.
  if (v.show_power_dashboard) {
    _draw_power_dashboard(v);
    return;
  }

  const bool chart_visible = metric_has_chart(v.active_metric);
  const bool pm_selected = (v.active_metric == Metric::Pm25);
  const bool co2_selected = (v.active_metric == Metric::Co2);
  const bool temp_selected = (v.active_metric == Metric::Temp);
  const bool hum_selected = (v.active_metric == Metric::Humidity);

  // --- Grid dividers (full 128px width) ---
  // 1st divider: always 2px thick
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_0_Y, SCREEN_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_0_Y + 1, SCREEN_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_1_Y, SCREEN_W);
  // 3rd divider: 2px thick when chart visible, 1px otherwise
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_2_Y, SCREEN_W);
  if (chart_visible) {
    u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_2_Y + 1, SCREEN_W);
  }

  // Vertical divider
  if (chart_visible) {
    // Only rows 1–2 when chart active
    u8g2_DrawVLine(&_u8g2, GRID_DIVIDER_X, GRID_DIVIDER_0_Y, GRID_DIVIDER_2_Y - GRID_DIVIDER_0_Y);
  } else {
    // All 3 rows
    u8g2_DrawVLine(&_u8g2, GRID_DIVIDER_X, GRID_DIVIDER_0_Y, 249 - GRID_DIVIDER_0_Y);
  }

  // --- Selection rectangles (filled black with white text) ---
  if (pm_selected) {
    u8g2_DrawBox(&_u8g2, 0, PM_BLOCK_Y, SCREEN_W, PM_BLOCK_H);
  }
  if (co2_selected) {
    u8g2_DrawBox(&_u8g2, 0, CO2_BLOCK_Y, SCREEN_W, CO2_BLOCK_H);
  }
  if (temp_selected) {
    u8g2_DrawBox(&_u8g2, SEL_TEMP_X, SEL_TEMP_Y, SEL_TEMP_W, SEL_TEMP_H);
  }
  if (hum_selected) {
    u8g2_DrawBox(&_u8g2, SEL_HUM_X, SEL_HUM_Y, SEL_HUM_W, SEL_HUM_H);
  }

  char buf[24];

  // --- PM2.5 hero section ---
  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 1);

  // Dual-font label: "PM2.5" in Logisoso16 + "(ug/m3)" or "(USAQI)" in HelvR12
  {
    const char *pm_name = "PM2.5";
    const char *pm_unit = v.pm_use_usaqi ? "(USAQI)" : "(ug/m3)";

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    const int name_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, pm_name));
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    const int unit_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, pm_unit));
    const int total_w = name_w + unit_w;
    const int label_x = (SCREEN_W - total_w) / 2;

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    draw_text(&_u8g2, label_x, PM_LABEL_BASELINE_Y, pm_name);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    draw_text(&_u8g2, label_x + name_w, PM_LABEL_BASELINE_Y, pm_unit);
  }

  // PM2.5 value — centered at x=64
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  format_pm_value(buf, sizeof(buf), v.pm25_ugm3, v.pm_use_usaqi);
  draw_centered_text(&_u8g2, SCREEN_W / 2, PM_VALUE_BASELINE_Y, buf);

  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // --- CO2 hero section ---
  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 1);

  // Dual-font label: "CO2" in Logisoso16 + "(ppm)" in HelvR12
  {
    const char *co2_name = "CO2";
    const char *co2_unit = "(ppm)";

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    const int name_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, co2_name));
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    const int unit_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, co2_unit));
    const int total_w = name_w + unit_w;
    const int label_x = (SCREEN_W - total_w) / 2;

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    draw_text(&_u8g2, label_x, CO2_LABEL_NAME_BASELINE_Y, co2_name);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    draw_text(&_u8g2, label_x + name_w, CO2_LABEL_UNIT_BASELINE_Y, co2_unit);
  }

  // CO2 value — centered at x=64
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  format_co2_value(buf, sizeof(buf), v.co2_ppm);
  draw_centered_text(&_u8g2, SCREEN_W / 2, CO2_VALUE_BASELINE_Y, buf);

  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // --- Grid cells ---
  char temp_buf[24];
  char hum_buf[24];

  // Temperature grid cell uses DrawUTF8 for the degree symbol.
  // format_temperature_value is preserved for chart stats; here we format
  // with the UTF-8 degree sign for display.
  if (!is_temperature_valid(v.temperature_c)) {
    snprintf(temp_buf, sizeof(temp_buf), "-");
  } else {
    const float shown = temp_for_display(v.temperature_c, v.use_fahrenheit);
    format_one_decimal(temp_buf, sizeof(temp_buf), shown);
    const size_t len = strlen(temp_buf);
    snprintf(temp_buf + len, sizeof(temp_buf) - len, " \xC2\xB0%c", v.use_fahrenheit ? 'F' : 'C');
  }
  format_humidity_value(hum_buf, sizeof(hum_buf), v.humidity_pct);

  // Row 1: Temp / Humidity (always visible)
  if (temp_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_cell(&_u8g2, 0, "Temperature", temp_buf, true);
  if (temp_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  if (hum_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_cell(&_u8g2, 1, "Humidity", hum_buf, false);
  if (hum_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // Row 2: TVOC/NOx or Min/Max (conditional)
  if (chart_visible) {
    char min_buf[24];
    char max_buf[24];
    format_chart_stat(min_buf, sizeof(min_buf), v.active_metric, v.chart_min, v.use_fahrenheit,
                      v.pm_use_usaqi);
    format_chart_stat(max_buf, sizeof(max_buf), v.active_metric, v.chart_max, v.use_fahrenheit,
                      v.pm_use_usaqi);
    draw_cell(&_u8g2, 2, "Min", min_buf, false);
    draw_cell(&_u8g2, 3, "Max", max_buf, false);
  } else {
    char tvoc_buf[24];
    char nox_buf[24];
    format_int_index_value(tvoc_buf, sizeof(tvoc_buf), v.tvoc_index);
    format_int_index_value(nox_buf, sizeof(nox_buf), v.nox_index);
    draw_cell(&_u8g2, 2, "TVOC", tvoc_buf, false);
    draw_cell(&_u8g2, 3, "NOx", nox_buf, false);
  }

  // Row 3: Pressure/Altitude or Chart (conditional)
  if (chart_visible) {
    _draw_chart(v);
  } else {
    char pressure_buf[24];
    char altitude_buf[24];
    format_pressure_value(pressure_buf, sizeof(pressure_buf), v.pressure_hpa);
    format_altitude_value(altitude_buf, sizeof(altitude_buf), v.altitude_m);
    draw_cell(&_u8g2, 4, "Pressure", pressure_buf, false);
    draw_cell(&_u8g2, 5, "Altitude", altitude_buf, false);
  }
}

void DisplayService::_draw_power_dashboard(const DisplayValues &v) {
  const PowerDashboardData &p = v.power_dashboard;
  char buf[40];

  // Phase banner — short label so it fits even with LOW-PWR suffix.
  // Plugged-in + low-power means CHARGE phase running the low-power gates
  // (SPS30 + GPS off so charge current isn't burnt by the load).
  const char *phase;
  if (p.blearn_phase != nullptr) {
    phase = p.blearn_phase;
  } else if (!p.plugged_in && p.low_power_active) {
    phase = "RELAX LOW-PWR";
  } else if (p.plugged_in && p.low_power_active) {
    phase = "CHRG LOW-PWR";
  } else if (p.plugged_in) {
    phase = "CHARGE";
  } else {
    phase = "RELAX";
  }
  u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 36, phase);
  u8g2_DrawHLine(&_u8g2, 0, 42, SCREEN_W);
  u8g2_DrawHLine(&_u8g2, 0, 43, SCREEN_W); // 2px bold to stand in for missing bold font

  if (!p.valid) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    draw_centered_text(&_u8g2, SCREEN_W / 2, 130, "FG: NO DATA");
    return;
  }

  // SOC headline.
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(p.soc_pct));
  draw_centered_text(&_u8g2, SCREEN_W / 2, 90, buf);

  // Cell voltage.
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
  snprintf(buf, sizeof(buf), "%.3fV", static_cast<double>(p.voltage_mv) / 1000.0);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 114, buf);

  // Cell current — signed, big enough to read at a glance during charge/relax.
  u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
  snprintf(buf, sizeof(buf), "I %+d mA", static_cast<int>(p.current_ma));
  draw_centered_text(&_u8g2, SCREEN_W / 2, 134, buf);
  u8g2_DrawHLine(&_u8g2, 0, 142, SCREEN_W);

  // Detail rows — small font.  FCC drift is the headline for learning
  // progress (1917 → 2128 on a fresh learn), so it gets the first detail row.
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);

  snprintf(buf, sizeof(buf), "%u/%u mAh", static_cast<unsigned>(p.remaining_mah),
           static_cast<unsigned>(p.full_charge_mah));
  draw_centered_text(&_u8g2, SCREEN_W / 2, 156, buf);

  snprintf(buf, sizeof(buf), "T%.1fC  FC%d CHG%d DSG%d", static_cast<double>(p.temperature_c),
           p.flag_fc ? 1 : 0, p.flag_chg ? 1 : 0, p.flag_dsg ? 1 : 0);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 170, buf);

  snprintf(buf, sizeof(buf), "Vs %.2f  Vp %.2f", static_cast<double>(p.vsys_mv) / 1000.0,
           static_cast<double>(p.vpmid_mv) / 1000.0);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 184, buf);

  snprintf(buf, sizeof(buf), "ICHG %u mA", static_cast<unsigned>(p.charge_current_ma));
  draw_centered_text(&_u8g2, SCREEN_W / 2, 198, buf);

  // BMS state — mirror of BmsChargingState (defined in
  // components/airgradient-bms/types/bms_types.h).  Local mapping keeps the
  // display layer free of the bms_types.h include.  Order must match the
  // enum declaration order in that header.
  static const char *const kBmsStates[] = {
      "Unknown",   "NotCharging",  "TrickleCharge", "PreCharge",
      "FastCharge", "TaperCharge", "TopOffActive",  "ChrgDone",
  };
  constexpr uint8_t kBmsStateCount = sizeof(kBmsStates) / sizeof(kBmsStates[0]);
  const char *state_str =
      (p.bms_charging_state < kBmsStateCount) ? kBmsStates[p.bms_charging_state] : "?";
  snprintf(buf, sizeof(buf), "BMS %s", state_str);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 212, buf);
}

void DisplayService::_draw_menu_overlay(const DisplayValues &v) {
  // Erase below the 2px-thick grid divider at MAIN_MENU_BG_Y, preserving
  // the divider pair (y, y+1) as the top border of the menu overlay.
  constexpr int erase_y = MAIN_MENU_BG_Y + 2;
  constexpr int erase_h = MAIN_MENU_BG_H - 2;
  u8g2_DrawBox(&_u8g2, 0, erase_y, SCREEN_W, erase_h);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, erase_y, SCREEN_W, erase_h);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, false);
}

void DisplayService::_draw_full_screen_list(const DisplayValues &v) {
  // White overlay area (full-screen list background)
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, true);

  // About screen has additional text fields below the list rows
  if (v.screen == Screen::About) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
    draw_text(&_u8g2, 10, 88, v.about_title);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    draw_text(&_u8g2, 10, 103, v.about_firmware);
    draw_text(&_u8g2, 10, 117, v.about_serial);
    draw_text(&_u8g2, 10, 130, v.about_hardware);
  }
}

void DisplayService::_draw_snackbar(const DisplayValues &v) {
  if (v.snackbar_text == nullptr || v.snackbar_text[0] == '\0')
    return;

  u8g2_DrawBox(&_u8g2, 0, SNACKBAR_Y, SCREEN_W, SNACKBAR_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, SNACKBAR_TEXT_BASELINE_Y, v.snackbar_text);
  u8g2_SetDrawColor(&_u8g2, 0);
}

void DisplayService::_draw_shutdown() {
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 115, "Powering off...");
  draw_centered_text(&_u8g2, CONTENT_W / 2, 135, "See you soon");
  draw_logo(&_u8g2, 218, 24);
}

void DisplayService::_draw_discharge_complete() {
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 115, "Discharge complete");
  draw_centered_text(&_u8g2, CONTENT_W / 2, 135, "Ship mode");
  draw_logo(&_u8g2, 218, 24);
}

void DisplayService::_draw_blearn_phase(const char *line1, const char *line2) {
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 115, line1);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 135, line2);
  draw_logo(&_u8g2, 218, 24);
}

void DisplayService::_draw_pairing_passkey(const DisplayValues &v) {
  // Label — small font, centered
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 100, "Bluetooth Pairing");

  // Passkey — large numeric font, centered, zero-padded to 6 digits
  char passkey_str[8];
  snprintf(passkey_str, sizeof(passkey_str), "%06" PRIu32, v.ble_passkey);
  u8g2_SetFont(&_u8g2, u8g2_font_10x20_tn);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 135, passkey_str);

  // Instruction — small font
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 165, "Enter code on phone");
}

void DisplayService::_draw_chart(const DisplayValues &v) {
  // Polyline only — no axes, no border lines.
  if (v.chart_samples == nullptr || v.chart_count == 0)
    return;

  const float range = v.chart_max - v.chart_min;
  const int plot_h = PLOT_H - 1; // drawable height range
  int prev_x = PLOT_X;
  int prev_y = PLOT_Y + plot_h / 2;

  for (int x = 0; x < PLOT_W; ++x) {
    const int sample_index =
        (x * static_cast<int>(v.chart_count - 1)) / ((PLOT_W > 1) ? (PLOT_W - 1) : 1);
    const float sample = v.chart_samples[sample_index];

    int y = PLOT_Y + plot_h / 2;
    if (range > 0.001f) {
      const float norm = (sample - v.chart_min) / range;
      y = PLOT_Y + plot_h - static_cast<int>(norm * static_cast<float>(plot_h));
    }
    if (y < PLOT_Y)
      y = PLOT_Y;
    if (y > PLOT_Y + plot_h)
      y = PLOT_Y + plot_h;

    const int draw_x = PLOT_X + x;
    if (x > 0) {
      u8g2_DrawLine(&_u8g2, prev_x, prev_y, draw_x, y);
    }
    prev_x = draw_x;
    prev_y = y;
  }
}

// ===========================================================================
// DisplayService — async worker task
// ===========================================================================

void DisplayService::_worker_entry(void *arg) {
  auto *self = static_cast<DisplayService *>(arg);
  self->_worker_loop();
  RTOS::task_delete(nullptr);
}

void DisplayService::_worker_loop() {
  while (_running) {
    // Block until the orchestrator signals frame-ready.
    RTOS::task_notify_take(UINT32_MAX);
    if (!_running)
      break;

    esp_err_t err = driver_bus_acquire();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "worker: bus acquire failed: %s", esp_err_to_name(err));
      _worker_busy = false;
      continue;
    }

    switch (_pending_mode) {
    case RefreshMode::Full:
      err = driver_hw_init_full();
      if (err == ESP_OK)
        err = driver_set_basemap(_spi_buf);
      _diff_count = 0;
      break;

    case RefreshMode::Fast:
      err = driver_hw_init_fast();
      if (err == ESP_OK)
        err = driver_fast_write(_spi_buf);
      if (err == ESP_OK)
        err = driver_fast_commit();
      if (_diff_count < UINT8_MAX) {
        _diff_count++;
      }
      break;

    case RefreshMode::Partial:
      err = driver_part_begin();
      if (err == ESP_OK) {
        memcpy(_region_buf, _spi_buf + BODY_Y * BUF_ROW_BYTES, BODY_H * BUF_ROW_BYTES);
        err = driver_part_write_region(0, BODY_Y, _region_buf, BODY_H, SCREEN_W);
      }
      if (err == ESP_OK)
        err = driver_part_commit();
      if (_diff_count < UINT8_MAX) {
        _diff_count++;
      }
      break;
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "worker: refresh failed: %s", esp_err_to_name(err));
    }

    driver_bus_release();
    _worker_busy = false;
  }
}

#endif // !TEST_HOST
