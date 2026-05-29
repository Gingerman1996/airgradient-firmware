/**
 * AirGradient — host tests for the pure battery-learning FSM.
 *
 * The BlearnController is the main correctness artifact of the blearn feature
 * (the orchestrator wiring is thin glue); these tests pin its transitions, the
 * boot-resume matrix (design §6), the verify criteria (design §7), and the
 * POR-loss cap → Failed escape.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "blearn_controller.h"

#include <catch2/catch_test_macros.hpp>

namespace {

// A snapshot with everything cleared; tests set only the fields they exercise.
PowerSnapshot snap_blank() { return PowerSnapshot{}; }

// A "learning intact" snapshot: no POR, Qmax learned.
PowerSnapshot snap_intact(bool on_charger) {
  PowerSnapshot s = snap_blank();
  s.fg_itpor = false;
  s.fg_qmax_up = true;
  s.external_input_present = on_charger;
  return s;
}

// A healthy learned Ra grid (every value > 0, all off the ROM default).
void fill_good_ra(VerifyInputs &in) {
  for (int i = 0; i < BQ27427_RA_TABLE_SIZE; ++i) {
    in.ra[i] = static_cast<int16_t>(400 + i * 10); // well off RA_DEFAULT_VALUE(256)
  }
}

VerifyInputs verify_good() {
  VerifyInputs in{};
  in.reads_ok = true;
  in.itpor = false;
  in.qmax_up = true;
  in.design_capacity_mah = 2000;
  in.qmax_mah = 1950; // within [1400, 2800]
  fill_good_ra(in);
  return in;
}

} // namespace

// ---------------------------------------------------------------------------
// Happy-path stage transitions
// ---------------------------------------------------------------------------

TEST_CASE("start enters Charge cycle 1 and asks to persist", "[blearn]") {
  BlearnController c;
  c.start();
  REQUIRE(c.stage() == BlearnStage::Charge);
  REQUIRE(c.cycle() == 1);

  BlearnAction a = c.tick(snap_blank(), 1000);
  REQUIRE(a.active);
  REQUIRE(a.persist_stage); // the pending write from start()
  REQUIRE(a.set_charge_enabled);
  REQUIRE(a.charge_current_ma == BlearnController::CHARGE_CURRENT_MA);
  REQUIRE(a.screen == Screen::BlearnCharging);
}

TEST_CASE("Charge advances to Rest on Full Charge", "[blearn]") {
  BlearnController c;
  c.start();
  (void)c.tick(snap_blank(), 0); // arm charge timer

  PowerSnapshot s = snap_blank();
  REQUIRE(c.tick(s, 1000).screen == Screen::BlearnCharging); // not full yet

  s.fg_flag_fc = true;
  BlearnAction a = c.tick(s, 2000);
  REQUIRE(c.stage() == BlearnStage::Rest);
  REQUIRE(a.persist_stage);
}

TEST_CASE("Charge advances to Rest on BMS charge-done even if FC never latches", "[blearn]") {
  BlearnController c;
  c.start();
  (void)c.tick(snap_blank(), 0); // arm charge timer

  // Charger actively charging — observed, but the gauge FC flag has not latched.
  PowerSnapshot s = snap_blank();
  s.charging_status = BmsChargingState::FastCharge;
  s.fg_flag_fc = false;
  REQUIRE(c.tick(s, 1000).screen == Screen::BlearnCharging);
  REQUIRE(c.stage() == BlearnStage::Charge);

  // BMS terminates charge while FC is still 0 (chemistry / Taper-Voltage
  // mismatch) → must still advance to Rest, not hang until CHARGE_TIMEOUT_MS.
  s.charging_status = BmsChargingState::ChargeTerminationDone;
  BlearnAction a = c.tick(s, 2000);
  REQUIRE(c.stage() == BlearnStage::Rest);
  REQUIRE(a.persist_stage);
}

TEST_CASE("Charge does not false-trigger Rest on NotCharging before charging seen", "[blearn]") {
  BlearnController c;
  c.start();
  (void)c.tick(snap_blank(), 0); // arm charge timer

  // BMS briefly reports NotCharging at the very start (charger not ramped yet)
  // with FC still 0 — must NOT be treated as charge-done.
  PowerSnapshot s = snap_blank();
  s.charging_status = BmsChargingState::NotCharging;
  s.fg_flag_fc = false;
  c.tick(s, 1000);
  REQUIRE(c.stage() == BlearnStage::Charge);
}

TEST_CASE("Charge times out to Failed", "[blearn]") {
  BlearnController c;
  c.start();
  (void)c.tick(snap_blank(), 1); // charge_started = 1

  BlearnAction a = c.tick(snap_blank(), 1 + BlearnController::CHARGE_TIMEOUT_MS);
  REQUIRE(c.stage() == BlearnStage::Failed);
  REQUIRE(a.persist_stage);
}

TEST_CASE("Rest advances to Discharge only on OCV taken AND rest timeout", "[blearn]") {
  BlearnController c;
  c.load(BlearnStage::Rest, 1, 0);

  // Enter Rest tick (rest_started armed).
  PowerSnapshot s = snap_blank();
  BlearnAction a = c.tick(s, 1000);
  REQUIRE(a.set_charge_enabled == false);
  REQUIRE(a.low_power == true);
  REQUIRE(a.screen == Screen::BlearnResting);
  REQUIRE(c.stage() == BlearnStage::Rest);

  // OCV taken but timeout not elapsed → stay.
  s.fg_ocv_taken = true;
  c.tick(s, 1000 + BlearnController::REST_TIMEOUT_MS - 1);
  REQUIRE(c.stage() == BlearnStage::Rest);

  // Timeout elapsed without OCV → stay.
  PowerSnapshot s2 = snap_blank();
  c.tick(s2, 1000 + BlearnController::REST_TIMEOUT_MS + 5000);
  REQUIRE(c.stage() == BlearnStage::Rest);

  // Both conditions → advance.
  s.fg_ocv_taken = true;
  BlearnAction adv = c.tick(s, 1000 + BlearnController::REST_TIMEOUT_MS + 6000);
  REQUIRE(c.stage() == BlearnStage::Discharge);
  REQUIRE(adv.persist_stage);
}

TEST_CASE("Discharge raises unplug cue and ships on EDV", "[blearn]") {
  BlearnController c;
  c.load(BlearnStage::Discharge, 1, 0);

  BlearnAction a = c.tick(snap_blank(), 1000);
  REQUIRE(a.unplug_cue);
  REQUIRE(a.low_power == false);
  REQUIRE(a.set_charge_enabled == false);
  REQUIRE(a.screen == Screen::BlearnUnplug);
  REQUIRE(c.stage() == BlearnStage::Discharge);

  PowerSnapshot edv = snap_blank();
  edv.edv_cutoff_reached = true;
  BlearnAction ship = c.tick(edv, 2000);
  REQUIRE(c.stage() == BlearnStage::CycleDone);
  REQUIRE(ship.commit_then_ship);
  REQUIRE(ship.screen == Screen::DischargeComplete);
  REQUIRE(ship.unplug_cue == false);
}

// ---------------------------------------------------------------------------
// Boot-resume matrix (design §6)
// ---------------------------------------------------------------------------

TEST_CASE("resume: Idle/Complete healthy is not hijacked", "[blearn][resume]") {
  {
    BlearnController c;
    c.load(BlearnStage::Idle, 0, 0);
    REQUIRE(c.resume_on_boot(snap_intact(true)) == false);
    REQUIRE(c.stage() == BlearnStage::Idle);
  }
  {
    BlearnController c;
    c.load(BlearnStage::Complete, 2, 0);
    REQUIRE(c.resume_on_boot(snap_intact(true)) == false);
    REQUIRE(c.stage() == BlearnStage::Complete);
  }
}

TEST_CASE("resume: Complete with gauge reset → Failed", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::Complete, 2, 0);
  PowerSnapshot lost = snap_blank();
  lost.fg_itpor = true; // POR
  REQUIRE(c.resume_on_boot(lost) == true);
  REQUIRE(c.stage() == BlearnStage::Failed);
}

TEST_CASE("resume: CycleDone intact below target → next cycle Charge", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::CycleDone, 1, 0);
  REQUIRE(c.resume_on_boot(snap_intact(true)) == true);
  REQUIRE(c.stage() == BlearnStage::Charge);
  REQUIRE(c.cycle() == 2);
}

TEST_CASE("resume: CycleDone intact at target → Verify", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::CycleDone, BlearnController::CYCLE_TARGET, 0);
  REQUIRE(c.resume_on_boot(snap_intact(true)) == true);
  REQUIRE(c.stage() == BlearnStage::Verify);
}

TEST_CASE("resume: CycleDone with POR loss → restart cycle 1, loss counted", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::CycleDone, 2, 0);
  PowerSnapshot lost = snap_blank();
  lost.fg_itpor = true;
  REQUIRE(c.resume_on_boot(lost) == true);
  REQUIRE(c.stage() == BlearnStage::Charge);
  REQUIRE(c.cycle() == 1);
  REQUIRE(c.itpor_losses() == 1);
}

TEST_CASE("resume: POR-loss cap reaches Failed", "[blearn][resume]") {
  BlearnController c;
  // Already at cap-1 losses; one more loss should trip Failed.
  c.load(BlearnStage::CycleDone, 1, BlearnController::ITPOR_LOSS_CAP - 1);
  PowerSnapshot lost = snap_blank();
  lost.fg_itpor = true;
  REQUIRE(c.resume_on_boot(lost) == true);
  REQUIRE(c.itpor_losses() == BlearnController::ITPOR_LOSS_CAP);
  REQUIRE(c.stage() == BlearnStage::Failed);
}

TEST_CASE("resume: mid-run intact on battery resumes Discharge", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::Rest, 1, 0);
  REQUIRE(c.resume_on_boot(snap_intact(false)) == true); // on battery
  REQUIRE(c.stage() == BlearnStage::Discharge);
}

TEST_CASE("resume: mid-run intact on charger restarts Charge", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::Discharge, 1, 0);
  REQUIRE(c.resume_on_boot(snap_intact(true)) == true); // on charger
  REQUIRE(c.stage() == BlearnStage::Charge);
}

TEST_CASE("resume: Failed stays Failed", "[blearn][resume]") {
  BlearnController c;
  c.load(BlearnStage::Failed, 1, 2);
  REQUIRE(c.resume_on_boot(snap_intact(true)) == false);
  REQUIRE(c.stage() == BlearnStage::Failed);
}

// ---------------------------------------------------------------------------
// Verify criteria (design §7) — the production pass/fail gate
// ---------------------------------------------------------------------------

TEST_CASE("verify_pass: a fully-learned grid passes", "[blearn][verify]") {
  REQUIRE(BlearnController::verify_pass(verify_good()));
}

TEST_CASE("verify_pass: read failure fails", "[blearn][verify]") {
  VerifyInputs in = verify_good();
  in.reads_ok = false;
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

TEST_CASE("verify_pass: POR fails", "[blearn][verify]") {
  VerifyInputs in = verify_good();
  in.itpor = true;
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

TEST_CASE("verify_pass: qmax not updated fails", "[blearn][verify]") {
  VerifyInputs in = verify_good();
  in.qmax_up = false;
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

TEST_CASE("verify_pass: Qmax out of band fails", "[blearn][verify]") {
  VerifyInputs lo = verify_good();
  lo.qmax_mah = 1000; // < 0.7*2000
  REQUIRE_FALSE(BlearnController::verify_pass(lo));

  VerifyInputs hi = verify_good();
  hi.qmax_mah = 3000; // > 1.4*2000
  REQUIRE_FALSE(BlearnController::verify_pass(hi));
}

TEST_CASE("verify_pass: non-positive Ra value fails", "[blearn][verify]") {
  VerifyInputs in = verify_good();
  in.ra[7] = 0;
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

TEST_CASE("verify_pass: Ra still at ROM defaults fails (the real grid check)",
          "[blearn][verify]") {
  // Whole grid pinned at the ROM default → "not learned across the range".
  VerifyInputs in = verify_good();
  for (int i = 0; i < BQ27427_RA_TABLE_SIZE; ++i) {
    in.ra[i] = BlearnController::RA_DEFAULT_VALUE;
  }
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

TEST_CASE("verify_pass: even one grid point still at default fails", "[blearn][verify]") {
  VerifyInputs in = verify_good();
  in.ra[BQ27427_RA_TABLE_SIZE - 1] = BlearnController::RA_DEFAULT_VALUE; // top point unlearned
  REQUIRE_FALSE(BlearnController::verify_pass(in));
}

// ---------------------------------------------------------------------------
// on_verify_result transitions
// ---------------------------------------------------------------------------

TEST_CASE("on_verify_result: pass → Complete", "[blearn][verify]") {
  BlearnController c;
  c.load(BlearnStage::Verify, BlearnController::CYCLE_TARGET, 0);
  REQUIRE(c.on_verify_result(verify_good()) == true);
  REQUIRE(c.stage() == BlearnStage::Complete);
}

TEST_CASE("on_verify_result: fail below cap → another cycle Charge", "[blearn][verify]") {
  BlearnController c;
  c.load(BlearnStage::Verify, 1, 0); // cycle 1, cap 2
  VerifyInputs bad = verify_good();
  bad.qmax_up = false;
  REQUIRE(c.on_verify_result(bad) == true);
  REQUIRE(c.stage() == BlearnStage::Charge);
  REQUIRE(c.cycle() == 2);
}

TEST_CASE("on_verify_result: fail at cap → Failed", "[blearn][verify]") {
  BlearnController c;
  c.load(BlearnStage::Verify, BlearnController::CYCLE_TARGET, 0);
  VerifyInputs bad = verify_good();
  bad.itpor = true;
  REQUIRE(c.on_verify_result(bad) == true);
  REQUIRE(c.stage() == BlearnStage::Failed);
}

TEST_CASE("on_verify_result: ignored when not in Verify", "[blearn][verify]") {
  BlearnController c;
  c.load(BlearnStage::Charge, 1, 0);
  REQUIRE(c.on_verify_result(verify_good()) == false);
  REQUIRE(c.stage() == BlearnStage::Charge);
}

TEST_CASE("Verify tick requests a verify", "[blearn][verify]") {
  BlearnController c;
  c.load(BlearnStage::Verify, 2, 0);
  BlearnAction a = c.tick(snap_blank(), 1000);
  REQUIRE(a.run_verify);
  REQUIRE(a.screen == Screen::BlearnVerifying);
}

TEST_CASE("reset clears to Idle", "[blearn]") {
  BlearnController c;
  c.load(BlearnStage::Failed, 2, 3);
  c.reset();
  REQUIRE(c.stage() == BlearnStage::Idle);
  REQUIRE(c.cycle() == 0);
  REQUIRE(c.itpor_losses() == 0);
  BlearnAction a = c.tick(snap_blank(), 1000);
  REQUIRE(a.persist_stage);
  REQUIRE(a.active == false);
}
