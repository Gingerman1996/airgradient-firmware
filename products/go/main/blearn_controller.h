/**
 * AirGradient Go — Automated battery-learning (blearn) state machine
 *
 * The pure FSM that drives a per-unit, end-of-line fuel-gauge learning run
 * (charge → rest/OCV1 → unplug → discharge → EDV ship-mode → re-plug →
 * repeat/verify), as specified in `blearn_two_cycle_design.md` Part 2.
 *
 * DESIGN CONSTRAINT — THIS FILE IS PURE.  It must NOT include bq27427.h or any
 * ESP-IDF / driver header.  It operates only on PowerSnapshot flags + plain
 * values + emits a BlearnAction; the orchestrator performs the actual driver
 * reads (for verify) and passes the results back in via on_verify_result().
 * Keeping it pure is what makes it host-testable — unlike the FG read path.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BLEARN_CONTROLLER_H
#define BLEARN_CONTROLLER_H

#include <cstdint>

#include "go_display.h"  // Screen
#include "go_power.h"    // PowerSnapshot
#include "go_settings.h" // BlearnStage

/// Number of Ra (impedance) grid points learned by the BQ27427 (TRM §7.4.3).
/// Duplicated here as a plain constant so this header stays free of the driver
/// header (the FSM must host-compile).  MUST match BQ27427::RA_TABLE_SIZE; a
/// static_assert in blearn_controller.cpp (target build only) guards the pair.
static constexpr int BQ27427_RA_TABLE_SIZE = 15;

// ---------------------------------------------------------------------------
// BlearnAction — what the orchestrator should drive after a tick (design §2).
// ---------------------------------------------------------------------------

/// The controller is driven, not autonomous: it never touches hardware.  Each
/// tick() returns this for the orchestrator to apply.
struct BlearnAction {
  bool set_charge_enabled = false;     ///< Desired BMS charge-enable state.
  uint16_t charge_current_ma = 0;      ///< ICHG to program while charging.
  bool low_power = false;              ///< Quiet load (Rest) vs full load (Discharge).
  bool unplug_cue = false;             ///< LED + screen "unplug charger".
  Screen screen = Screen::Home;        ///< Phase screen to paint.
  bool commit_then_ship = false;       ///< EDV: persist CycleDone, paint, ship-mode.
  bool persist_stage = false;          ///< Write blearn_stage/cycle/itpor now.
  bool run_verify = false;             ///< Orchestrator should read the FG and call
                                       ///<   on_verify_result() this poll.
  bool active = false;                 ///< false when stage ∈ {Idle, Complete, Failed}
                                       ///<   and no action is needed — orchestrator
                                       ///<   leaves normal operation untouched.
};

// ---------------------------------------------------------------------------
// VerifyInputs — gauge read-back for the verify criteria (design §7).
// ---------------------------------------------------------------------------

/// Filled by the orchestrator from the (driver-side) FG read-back when the
/// controller asks for a verify (BlearnAction::run_verify).  verify_pass() is
/// pure so the criteria are host-testable.
struct VerifyInputs {
  bool reads_ok = false;        ///< All driver reads succeeded this attempt.
  bool itpor = false;           ///< Flags ITPOR — a POR wiped learning.
  bool qmax_up = false;         ///< CONTROL_STATUS QMAX_UP — Qmax updated.
  uint16_t qmax_mah = 0;        ///< Learned Qmax converted to mAh.
  uint16_t design_capacity_mah = 0; ///< Configured design capacity.
  int16_t ra[BQ27427_RA_TABLE_SIZE] = {}; ///< Learned Ra grid (15 values).
};

// ---------------------------------------------------------------------------
// BlearnController
// ---------------------------------------------------------------------------

class BlearnController {
public:
  /// Number of Ra grid points (must match BQ27427::RA_TABLE_SIZE — duplicated
  /// here as a plain constant so this header stays free of the driver header).
  static constexpr int RA_TABLE_SIZE = BQ27427_RA_TABLE_SIZE;

  /// Hard cap on cycles (default 2).  Verification-gated: the run stops as
  /// soon as criteria pass, but never exceeds this (design §1/§7).
  static constexpr uint8_t CYCLE_TARGET = 2;

  /// POR-induced restart cap → Failed (design §6).
  static constexpr uint8_t ITPOR_LOSS_CAP = 3;

  /// Minimum rest duration before leaving Rest, even once OCV1 is taken
  /// (design §4: rest ≥ 500 s).  Matches the gauge's relax + OCV-wait budget.
  static constexpr uint32_t REST_TIMEOUT_MS = 500000;

  /// Max time in Charge without reaching Full Charge before giving up
  /// (design §8).  8 h covers a 2000 mAh cell at the learning charge current
  /// with generous slack.
  static constexpr uint32_t CHARGE_TIMEOUT_MS = 8u * 60u * 60u * 1000u;

  /// ICHG to program while charging in a learning run (design §4).
  static constexpr uint16_t CHARGE_CURRENT_MA = 1500;

  /// ROM-default Ra value bands (design §7 criterion 4): a grid still at its
  /// from-scratch defaults has NOT learned.  The BQ27427 ships the Ra table at
  /// a flat default; "learned" means the values have moved off that default
  /// across the whole range (not just the bottom few points).  We treat a grid
  /// where every value sits within RA_DEFAULT_TOL of RA_DEFAULT_VALUE as "still
  /// at defaults" → fail.  Bench-tunable once real learned grids are captured.
  static constexpr int16_t RA_DEFAULT_VALUE = 256;
  static constexpr int16_t RA_DEFAULT_TOL = 4;

  BlearnController() = default;

  /// Seed the controller's working state from persisted GoSettings at boot,
  /// BEFORE resume_on_boot().  Does not mutate; just adopts the loaded values.
  void load(BlearnStage stage, uint8_t cycle, uint8_t itpor_losses);

  /// Begin a fresh run (admin "Start battery learning").  Sets stage=Charge,
  /// cycle=1, itpor_losses=0.  The next tick() returns persist_stage so the
  /// orchestrator commits it.
  void start();

  /// Abort/clear a run (admin "Reset battery learning").  Sets stage=Idle,
  /// cycle=0, itpor_losses=0; next tick() asks the orchestrator to persist.
  void reset();

  /// One-shot boot-resume decision (design §6).  Run once at boot AFTER load()
  /// and one poll_bms() so the snapshot carries fresh FG flags + power source.
  /// Returns true if the stage/cycle/itpor changed and should be persisted.
  /// Only re-enters the FSM when a run is genuinely in progress
  /// (stage ∉ {Idle, Complete, Failed}); a normal field unit is never hijacked.
  bool resume_on_boot(const PowerSnapshot &snap);

  /// Per-poll tick (design §4).  Pure: derives the next stage from the
  /// snapshot flags and returns the action to apply.  @p now_ms is a
  /// monotonic millisecond clock (passed in so the FSM stays host-testable).
  BlearnAction tick(const PowerSnapshot &snap, uint32_t now_ms);

  /// Apply the result of a verify the orchestrator performed (in response to
  /// BlearnAction::run_verify).  Advances Verify → Complete on pass, → Charge
  /// (next cycle) on fail below cap, → Failed at/over cap (design §7).
  /// Returns true if the stage changed and should be persisted.
  bool on_verify_result(const VerifyInputs &in);

  /// Pure verify criteria (design §7).  Exposed for host testing.
  static bool verify_pass(const VerifyInputs &in);

  // --- Accessors (for persistence + display) ---
  BlearnStage stage() const { return _stage; }
  uint8_t cycle() const { return _cycle; }
  uint8_t itpor_losses() const { return _itpor_losses; }

private:
  BlearnStage _stage = BlearnStage::Idle;
  uint8_t _cycle = 0;
  uint8_t _itpor_losses = 0;

  /// Uptime when the current Rest began; 0 = not resting.  Used for the
  /// REST_TIMEOUT_MS gate.  Runtime-only (the EDV power-off ends every cycle).
  uint32_t _rest_started_ms = 0;

  /// Uptime when the current Charge began; 0 = not charging.  Used for the
  /// CHARGE_TIMEOUT_MS escape.
  uint32_t _charge_started_ms = 0;

  /// Latched true once the BMS has actually been seen charging during this
  /// Charge stage, so the "BMS charge terminated" gate can't false-trigger on
  /// the brief NotCharging window before the charger ramps up.  Reset on every
  /// enter_charge().
  bool _charge_observed = false;

  /// True after a stage transition that the orchestrator must persist; cleared
  /// once tick() has reported it via BlearnAction::persist_stage.
  bool _persist_pending = false;

  void enter_charge();
};

#endif // BLEARN_CONTROLLER_H
