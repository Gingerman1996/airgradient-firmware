/**
 * AirGradient Go — Application Entry Point
 *
 * Thin shell: constructs the real board and runs the app.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_app.h"
#include "go_hardware_board.h"

#ifdef AG_DEBUG_GPS
#include "esp_log.h"
#endif

extern "C" void app_main() {
#ifdef AG_DEBUG_GPS
  // Verbose GPS debug — enable for a build with `AG_DEBUG_GPS=1 idf.py build`.
  // VERBOSE on GpsDriver streams every NMEA sentence and per-read byte count;
  // DEBUG on GpsService surfaces the 10 s sat/HDOP summary line. The TTFF
  // and fix-state-transition markers in GpsService are AG_LOGI and always on.
  esp_log_level_set("GpsDriver", ESP_LOG_VERBOSE);
  esp_log_level_set("GpsService", ESP_LOG_DEBUG);
#endif

  GoHardwareBoard board;
  GoApp app(board);
  app.run();
}
