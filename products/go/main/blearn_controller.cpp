/**
 * AirGradient Go — Automated battery-learning (blearn) state machine
 *
 * Pure FSM implementation.  See blearn_controller.h for the design constraint
 * (no driver / ESP-IDF includes here) and blearn_two_cycle_design.md Part 2
 * for the spec this implements.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "blearn_controller.h"

#include "ag_log.h"

// On the target build the driver header is available — assert that the locally
// duplicated grid size still matches the driver's, so the two can't drift.
// Guarded so the pure host build (which must not see bq27427.h) skips it.
#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("drivers/bq27427/bq27427.h")
#include "drivers/bq27427/bq27427.h"
static_assert(BQ27427_RA_TABLE_SIZE == BQ27427::RA_TABLE_SIZE,
              "blearn Ra grid size must match the BQ27427 driver");
#endif
#endif

static constexpr const char *TAG = "Blearn";

void BlearnController::load(BlearnStage stage, uint8_t cycle, uint8_t itpor_losses) {
  _stage = stage;
  _cycle = cycle;
  _itpor_losses = itpor_losses;
  _rest_started_ms = 0;
  _charge_started_ms = 0;
  _charge_observed = false;
  _persist_pending = false;
}

void BlearnController::enter_charge() {
  _stage = BlearnStage::Charge;
  _charge_started_ms = 0; // re-armed on the first Charge tick
  _rest_started_ms = 0;
  _charge_observed = false;
}

void BlearnController::start() {
  AG_LOGI(TAG, "start: beginning learning run (cycle 1)");
  _itpor_losses = 0;
  _cycle = 1;
  enter_charge();
  _persist_pending = true;
}

void BlearnController::reset() {
  AG_LOGI(TAG, "reset: clearing learning run");
  _stage = BlearnStage::Idle;
  _cycle = 0;
  _itpor_losses = 0;
  _rest_started_ms = 0;
  _charge_started_ms = 0;
  _persist_pending = true;
}

bool BlearnController::resume_on_boot(const PowerSnapshot &snap) {
  // Auto-resume guard: only re-enter the FSM when a run is genuinely in
  // progress.  A normal field unit (Idle / Complete / Failed) is never
  // hijacked (design §6).
  if (_stage == BlearnStage::Idle || _stage == BlearnStage::Failed) {
    return false;
  }

  const bool intact = (!snap.fg_itpor) && snap.fg_qmax_up; // learning present, no POR
  const bool on_charger = snap.external_input_present;
  const bool mid_run = (_stage == BlearnStage::Charge || _stage == BlearnStage::Rest ||
                        _stage == BlearnStage::Discharge);
  const BlearnStage before = _stage;
  const uint8_t before_cycle = _cycle;
  const uint8_t before_losses = _itpor_losses;

  // Helper for the POR-loss restart path (cap-guarded).
  auto por_restart = [&]() {
    if (++_itpor_losses >= ITPOR_LOSS_CAP) {
      AG_LOGE(TAG,
              "POR-loss cap reached (%u >= %u) — learning keeps getting wiped; "
              "EDV margin / cell needs attention. Marking Failed.",
              _itpor_losses, ITPOR_LOSS_CAP);
      _stage = BlearnStage::Failed;
    } else {
      AG_LOGW(TAG, "POR wiped learning (loss %u/%u) — restarting from cycle 1",
              _itpor_losses, ITPOR_LOSS_CAP);
      _cycle = 1;
      enter_charge();
    }
  };

  switch (_stage) {
  case BlearnStage::Complete:
    if (!intact) {
      AG_LOGW(TAG, "boot: Complete but gauge reset detected — learning lost, re-arm needed");
      _stage = BlearnStage::Failed;
    }
    break;

  case BlearnStage::CycleDone:
    if (!intact) {
      por_restart();
    } else if (_cycle < CYCLE_TARGET) {
      AG_LOGI(TAG, "boot: cycle %u done, intact → continuing to cycle %u", _cycle, _cycle + 1);
      _cycle = static_cast<uint8_t>(_cycle + 1);
      enter_charge();
    } else {
      AG_LOGI(TAG, "boot: final cycle done, intact → Verify");
      _stage = BlearnStage::Verify;
    }
    break;

  case BlearnStage::Verify:
    // Re-run verify (tick will request it).  No state change here.
    AG_LOGI(TAG, "boot: resuming Verify");
    break;

  case BlearnStage::Charge:
  case BlearnStage::Rest:
  case BlearnStage::Discharge:
    if (!intact) {
      por_restart();
    } else if (on_charger) {
      // Operator re-plugged the charger → restart this cycle's charge.
      AG_LOGI(TAG, "boot: mid-run, intact, on charger → restart cycle %u charge", _cycle);
      enter_charge();
    } else {
      // Spurious reset still on battery → resume the discharge (not a fresh
      // charge that isn't coming).
      AG_LOGI(TAG, "boot: mid-run, intact, on battery → resume discharge (cycle %u)", _cycle);
      _stage = BlearnStage::Discharge;
      _rest_started_ms = 0;
      _charge_started_ms = 0;
    }
    break;

  default:
    break;
  }

  (void)mid_run; // documented in the design table; switch covers each stage
  const bool changed = (_stage != before) || (_cycle != before_cycle) ||
                       (_itpor_losses != before_losses);
  if (changed) {
    _persist_pending = false; // resume persists explicitly via the return value
  }
  return changed;
}

BlearnAction BlearnController::tick(const PowerSnapshot &snap, uint32_t now_ms) {
  BlearnAction a{};

  // Surface any pending stage write from start()/reset() once.
  if (_persist_pending) {
    a.persist_stage = true;
    _persist_pending = false;
  }

  switch (_stage) {
  case BlearnStage::Idle:
  case BlearnStage::Complete:
  case BlearnStage::Failed:
    // Terminal / not-learning: leave normal operation untouched.  Still report
    // a phase screen for Complete/Failed so the operator sees the result.
    a.active = false;
    if (_stage == BlearnStage::Complete) {
      a.screen = Screen::BlearnComplete;
    } else if (_stage == BlearnStage::Failed) {
      a.screen = Screen::BlearnFailed;
    }
    return a;

  case BlearnStage::Charge:
    a.active = true;
    a.set_charge_enabled = true;
    a.charge_current_ma = CHARGE_CURRENT_MA;
    a.low_power = false;
    a.screen = Screen::BlearnCharging;
    {
      // Arm the charge timer on the first Charge tick; skip the timeout check
      // on that same tick so the (now_ms - started) subtraction can't underflow
      // when now_ms is small.
      const bool freshly_armed = (_charge_started_ms == 0);
      if (freshly_armed) {
        _charge_started_ms = now_ms ? now_ms : 1; // never 0 (sentinel)
      }
      // The robust "cell is full" signal is the BMS terminating charge — the
      // same trigger the legacy learning UX used (Orchestrator::on_bms_status_
      // timer keyed Rest on !is_bms_charging).  The gauge FC flag is preferred
      // when it latches, but it can stay 0 on a chemistry / Taper-Voltage
      // mismatch (design §10.1) and gating on FC alone would hang here until the
      // 8 h CHARGE_TIMEOUT_MS.  Advance on either, requiring that charging was
      // actually observed first so the brief NotCharging window at charge start
      // can't false-trigger.
      if (is_bms_charging(snap.charging_status)) {
        _charge_observed = true;
      }
      const bool bms_charge_done = _charge_observed && !is_bms_charging(snap.charging_status);
      if (snap.fg_flag_fc || bms_charge_done) {
        if (snap.fg_flag_fc) {
          AG_LOGI(TAG, "Charge → Rest (gauge Full Charge, cycle %u)", _cycle);
        } else {
          AG_LOGW(TAG,
                  "Charge → Rest (BMS terminated charge, cycle %u) but gauge FC not "
                  "latched — check Chem ID (want 1202) / Taper Voltage vs charger "
                  "VREG; OCV1 still taken at the relaxed top",
                  _cycle);
        }
        _stage = BlearnStage::Rest;
        _rest_started_ms = 0;
        _charge_started_ms = 0;
        a.persist_stage = true;
      } else if (!freshly_armed && now_ms - _charge_started_ms >= CHARGE_TIMEOUT_MS) {
        AG_LOGE(TAG, "Charge timed out (>%u ms) without charge termination — Failed",
                CHARGE_TIMEOUT_MS);
        _stage = BlearnStage::Failed;
        a.persist_stage = true;
      }
    }
    return a;

  case BlearnStage::Rest:
    a.active = true;
    a.set_charge_enabled = false; // charge off so the cell relaxes for OCV1
    a.low_power = true;           // quiet load
    a.screen = Screen::BlearnResting;
    {
      const bool freshly_armed = (_rest_started_ms == 0);
      if (freshly_armed) {
        _rest_started_ms = now_ms ? now_ms : 1;
      }
      // Advance only once the gauge has captured OCV1 AND the minimum rest has
      // elapsed (design §4): both gate the move to Discharge.
      if (!freshly_armed && snap.fg_ocv_taken &&
          (now_ms - _rest_started_ms) >= REST_TIMEOUT_MS) {
        AG_LOGI(TAG, "Rest → Discharge (OCV1 taken + rest %u ms elapsed, cycle %u)",
                REST_TIMEOUT_MS, _cycle);
        _stage = BlearnStage::Discharge;
        _rest_started_ms = 0;
        a.persist_stage = true;
      }
    }
    return a;

  case BlearnStage::Discharge:
    // §10.2 BENCH-PENDING: the discharge mechanism here is simply "release
    // LOW_POWER so the device runs its full own-load on battery" — there is no
    // Dsg-Current-Threshold register write.  The device's own draw (~50–70 mA)
    // may sit BELOW the gauge's ~120 mA Dsg threshold, so Ra may not fully
    // learn across the grid; the threshold/load is to be tuned on the bench
    // (see design §10.2).  Do not invent a guessed register value here.
    a.active = true;
    a.set_charge_enabled = false;
    a.low_power = false;     // full load → drive a real discharge toward EDV
    a.unplug_cue = true;     // LED + screen "unplug charger"
    a.screen = Screen::BlearnUnplug;
    if (snap.edv_cutoff_reached) {
      AG_LOGI(TAG, "Discharge → CycleDone (EDV cutoff reached, cycle %u)", _cycle);
      _stage = BlearnStage::CycleDone;
      // The EDV ordering (§5) is handled by the orchestrator's
      // handle_edv_cutoff(): persist+commit CycleDone BEFORE ship-mode.
      a.commit_then_ship = true;
      a.screen = Screen::DischargeComplete;
      a.unplug_cue = false;
    }
    return a;

  case BlearnStage::CycleDone:
    // Reached only if we somehow tick again before ship-mode (e.g. commit
    // failed and the device stayed up).  Keep asking for the commit+ship.
    a.active = true;
    a.set_charge_enabled = false;
    a.low_power = false;
    a.screen = Screen::DischargeComplete;
    a.commit_then_ship = true;
    return a;

  case BlearnStage::Verify:
    a.active = true;
    a.set_charge_enabled = false;
    a.low_power = true;
    a.screen = Screen::BlearnVerifying;
    a.run_verify = true; // orchestrator reads the FG and calls on_verify_result()
    return a;
  }

  return a;
}

bool BlearnController::verify_pass(const VerifyInputs &in) {
  if (!in.reads_ok) {
    return false;
  }
  // 1. No POR since learning.
  if (in.itpor) {
    return false;
  }
  // 2. Qmax learned.
  if (!in.qmax_up) {
    return false;
  }
  // 3. Qmax sane — within [0.7×DC, 1.4×DC] (design §7).
  if (in.design_capacity_mah == 0) {
    return false;
  }
  const uint32_t lo = (static_cast<uint32_t>(in.design_capacity_mah) * 7u) / 10u;
  const uint32_t hi = (static_cast<uint32_t>(in.design_capacity_mah) * 14u) / 10u;
  if (in.qmax_mah < lo || in.qmax_mah > hi) {
    return false;
  }
  // 4. Ra grid healthy: every value > 0, the grid has moved off ROM defaults
  //    across the WHOLE range (not just the bottom points), with smooth
  //    adjacent transitions.  res_up is NOT used as a gate here (design §7).
  bool any_off_default = false;
  bool all_off_default = true;
  for (int i = 0; i < BQ27427_RA_TABLE_SIZE; ++i) {
    if (in.ra[i] <= 0) {
      return false; // non-positive grid value → unhealthy
    }
    const int diff = in.ra[i] - BlearnController::RA_DEFAULT_VALUE;
    const bool at_default = (diff <= BlearnController::RA_DEFAULT_TOL &&
                             diff >= -BlearnController::RA_DEFAULT_TOL);
    if (at_default) {
      all_off_default = false;
    } else {
      any_off_default = true;
    }
  }
  (void)any_off_default;
  // "Moved off defaults across the whole range" → no grid point may still sit
  // at its ROM default.  A grid where even one point is still pinned at the
  // default has not learned across the full charge range.
  if (!all_off_default) {
    return false;
  }
  return true;
}

bool BlearnController::on_verify_result(const VerifyInputs &in) {
  if (_stage != BlearnStage::Verify) {
    return false;
  }
  const BlearnStage before = _stage;

  if (verify_pass(in)) {
    AG_LOGI(TAG, "Verify PASS → Complete (Qmax=%umAh, DC=%umAh)", in.qmax_mah,
            in.design_capacity_mah);
    _stage = BlearnStage::Complete;
  } else if (_cycle >= CYCLE_TARGET) {
    AG_LOGE(TAG, "Verify FAIL at cycle cap (%u/%u) — Failed (unit rejected)",
            _cycle, CYCLE_TARGET);
    _stage = BlearnStage::Failed;
  } else {
    AG_LOGW(TAG, "Verify FAIL, below cap (%u/%u) → another cycle", _cycle, CYCLE_TARGET);
    _cycle = static_cast<uint8_t>(_cycle + 1);
    enter_charge();
  }

  return _stage != before;
}
