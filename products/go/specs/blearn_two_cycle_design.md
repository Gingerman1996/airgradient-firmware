# Workflow: One-button automated battery learning (blearn) — AirGradient Go

**Status:** Workflow spec for design hand-off
**Target:** AirGradient Go (ESP32-C5, ESP-IDF), BQ27427 fuel gauge + BQ25628 BMS
**Companion:** [`fg_learning_sequence.md`](fg_learning_sequence.md) — the TRM-derived Impedance-Track sequence this workflow automates. Read it for the *why* behind each phase; this doc is the *operator + firmware flow*.

## Purpose

Define the **per-unit battery-learning workflow** that runs as the **end-of-line production test on every unit before shipping**, so it can be turned into firmware where the operator **presses one button to start, and the device runs the whole learning procedure automatically** — through one or more full charge/discharge cycles, a ship-mode power-off, reboots, and a final pass/fail verification — **with the only manual actions being unplugging and re-plugging the charger when the device asks.**

Every shipped unit learns and verifies its own fuel gauge; there is **no golden-image copy step**. The test must be trivial for a line operator: one button to start, then plug/unplug on cue, then read a pass/fail result.

This document describes *behaviour and flow only* in Parts 1–2, then a concrete firmware design. It is the input to the coding pass.

### Production reality (read this first)

Running a full learn on **every** unit makes **time-per-unit the dominant cost**: a full charge + full discharge is multiple hours, and the number of cycles multiplies it. Two consequences shape the design:

1. **Cycle count is the primary time multiplier.** The design therefore makes the run **verification-gated**: it stops as soon as the gauge verifies as learned (best case **one** cycle), with a hard cap (default 2). Resolving the discharge-load prerequisite (§10) so a single cycle fills the whole Ra grid is what makes a 1-cycle production test feasible — that is the highest-leverage decision in this whole document.
2. **The test must run many units in parallel.** Each unit is an independent autonomous FSM (state in its own flash, auto-resume on re-plug). A line operator services a **rack** of units, plugging/unplugging each one when *its* LED cues — no per-unit babysitting, no menu navigation. The persist + auto-resume design exists precisely to make this hands-off across the ship-mode power-off.

---

## The only manual actions

Everything else is automatic. The human does exactly three kinds of thing, each in response to an on-device cue:

| When | Cue shown by device | Operator action |
|---|---|---|
| Start | (admin menu) | Press **Start battery learning** once |
| After each full charge + rest | **LED blinks "unplug"** + screen prompt | **Unplug the charger** |
| After each discharge ends (device powers off) | Screen: **"Discharge complete"** then device is **off** | **Plug the charger back in** (device boots and resumes by itself) |

No re-arming, no menu navigation between cycles, no manual reading of registers. Plug/unplug on cue is the entire manual surface.

---

## The automated workflow

### Cycle 1
1. **Start** — operator enables learning (one button). Device records "learning in progress, cycle 1" to non-volatile flash (survives power-off).
2. **Charge to full** — device charges at high current until the fuel gauge reports Full Charge.
3. **Rest / OCV1** — device disables charging and quiets its own load so the cell relaxes; the gauge captures the top open-circuit voltage (OCV1).
4. **Cue: unplug** — device blinks the LED and shows an unplug prompt. → *operator unplugs charger.*
5. **Discharge** — running on battery, the device draws enough load to keep the gauge in its DISCHARGE state and learns the resistance grid + Qmax while the cell drains.
6. **End of discharge (EDV)** — when the cell reaches the safe cutoff voltage, the device **records "cycle 1 done" to flash**, shows **"Discharge complete"**, and **enters ship mode (powers off)** before the cell can over-discharge.

### Re-plug → Cycle 2 (automatic)
7. *Operator plugs the charger back in.* The device boots from cold.
8. **Resume decision** — on boot the device reads its saved learning state from flash **and** the gauge's learned status. If cycle 1 completed and the learning is intact, it **automatically continues into cycle 2** — no operator input.
9. **Cycle 2** repeats steps 2–6 (charge → rest/OCV1 → unplug cue → discharge → EDV), then records **"cycles done"** to flash and powers off again.

### Re-plug → Verify → Normal (automatic)
10. *Operator plugs the charger back in.* Device boots.
11. **Verify** — device reads the gauge's learned values and checks they pass the criteria below. On pass it records **"learning complete"** to flash and shows a success result.
12. **Done forever** — on this and every future boot, the device sees "learning complete" + a healthy gauge and goes straight to **normal operation**. Learning never runs again unless an operator explicitly re-arms it.

---

## Why it must survive power-off and reboot

The discharge phase deliberately ends by powering the device **fully off** (ship mode) to protect the cell. That means the workflow cannot live in RAM — it must be **resumable from a cold boot**:

- **Progress marker** (which cycle, and "done"/"complete") is written to **flash (NVS)**, which survives the power-off. (RTC memory does **not** survive ship mode and must not be used for this.)
- **The gauge's learned data** (Qmax + resistance grid) lives in the fuel gauge itself and survives the power-off as long as no deep over-discharge reset occurs — which is exactly what the EDV cutoff prevents.
- **On every boot** the device combines *(flash progress marker) × (live gauge status)* to decide: continue learning, verify, restart, or run normally. This is what makes re-plug "just work" with no operator interaction.

**Critical ordering:** the "cycle done" marker must be written and flushed to flash **before** the device triggers ship-mode power-off. If that write cannot be confirmed, the device must not power off (keep discharging and retry) — otherwise a cycle is silently lost.

---

## Boot resume decision (what the device does on each power-up)

| Saved progress (flash) | Gauge learning intact? | Device does |
|---|---|---|
| none / "complete" + gauge healthy | yes | **Normal operation** |
| "complete" but gauge reset detected | no | Flag for re-learn (operator must re-arm) |
| "cycle 1 done" | yes | **Auto-continue to cycle 2** |
| "cycle 1 done" | no (reset wiped learning) | **Restart from cycle 1** |
| "cycles done" | yes | **Verify → complete** |
| "cycles done" | no | **Restart from cycle 1** |
| mid-cycle (charge/rest/discharge) + booted still on battery | yes | **Resume that discharge** (not a fresh charge) |
| mid-cycle + booted on charger | yes | **Restart that cycle's charge** |

The device must tell apart "operator re-plugged the charger" from "the device just reset on its own (e.g. brief glitch) while still on battery," so it resumes the right phase instead of waiting for a charge that isn't coming.

---

## Completion / verification criteria

The device declares learning **complete** only when, read back from the gauge:

1. **No gauge reset** has occurred since learning (learning is intact).
2. The gauge reports **Qmax learned**.
3. **Qmax is sane** — within a sensible band of the configured design capacity (e.g. roughly 0.7×–1.4×).
4. The **resistance grid is healthy** — every grid value positive, transitions smooth across the whole grid (i.e. the grid is learned across the full charge range, not just near empty).

**Verification-gated, not a fixed count.** After each cycle the device re-checks these criteria. It **stops and declares complete as soon as they pass** — best case after **one** cycle (if §10 is resolved so the grid learns clean). It runs another cycle only if criteria still fail, up to a hard cap (default 2). If still failing at the cap, the device stops (does **not** loop forever), shows a **failure** result (the unit is rejected at the line), and waits for an operator to re-arm or investigate. This is the production pass/fail gate — a unit that didn't actually learn (e.g. Ra still at defaults) fails verification rather than shipping silently mis-gauged.

---

## On-device feedback (so the operator knows what to do, and what happened)

- **LED:** a distinct **"unplug now"** blink during the post-charge cue; a **"resting / working"** indication during charge and rest; a clear **success vs failure** indication at the end.
- **Screen:** current phase ("Charging… / Resting… / Discharging… / Unplug charger / Discharge complete / Verifying… / Learning complete / Learning failed"), plus a live readout of the gauge during learning (charge %, voltage, current, the learned values once available).

---

## Out of scope (intentionally not part of this workflow)

- **No golden-image copy.** Every unit learns its own gauge; there is no "learn one, copy to many" step.
- **Sealing the fuel gauge.** The device stays **unsealed** through learning and verification (verification needs block-transfer reads of the learned values). Sealing to lock the learned data, if wanted, is an optional **final** end-of-line step *after* a unit passes verification — never inside the automated learn/verify flow. (Normal-operation reads — SOC, voltage, current — work sealed; only the verify read-back needs unsealed.)
- **Discharge load tuning is NOT out of scope here — it is a prerequisite (§10).** Because this is now a per-unit production gate, a discharge that fails to actually learn the Ra grid would make the test meaningless. See §10.

---

## Notes for the design stage (not operator-facing)

- The discharge phase only learns the resistance grid while the gauge is genuinely in **DISCHARGE** (cell current above its discharge-current threshold) and while the cell is near room temperature. Designers must confirm the discharge load meets this — otherwise more cycles won't help. (Background and the relevant thresholds are in [`fg_learning_sequence.md`](fg_learning_sequence.md).)
- Existing building blocks to reuse already exist in the firmware: an admin "battery learning" arming toggle, charge-enable/disable and charge-current control, a low-power gating path used during the rest/relax window, the EDV→ship-mode path with the "Discharge complete" screen, an NVS settings store, an LED service with a charge-done blink, and fuel-gauge read-back of the learned values. The design should build the workflow's state machine on top of these rather than introduce parallel mechanisms.

---

# Firmware design (state machine)

This part turns the workflow above into a concrete firmware design: the persisted state, the state machine, the per-poll tick, the boot resume algorithm, and the exact existing symbols to reuse / extend. It is implementation-ready for a coding pass.

## 1. State model

Two persisted fields encode the whole run (no flat 12-state enum — a stage + a cycle counter is enough and keeps cycle 1 / cycle 2 from duplicating logic):

```cpp
enum class BlearnStage : uint8_t {
  Idle = 0,     // not learning (normal operation)
  Charge,       // charging to Full Charge
  Rest,         // charge off, quiet load, capturing OCV1
  Discharge,    // unplug cue raised; draining on battery toward EDV
  CycleDone,    // EDV reached for this cycle — written+committed BEFORE ship mode
  Verify,       // re-plugged after final cycle; checking pass criteria
  Complete,     // learned + verified → normal operation forever
  Failed,       // gave up (POR loss loop, or verify failed after cap)
};
```

Persisted (NVS) alongside the existing `GoSettings` keys (`go_settings.{h,cpp}`):

| Field | NVS key | Meaning |
|---|---|---|
| `blearn_stage : BlearnStage` | `"bls"` | current stage (above) |
| `blearn_cycle : uint8_t` | `"bln"` | 1-based cycle number in progress |
| `blearn_itpor_losses : uint8_t` | `"bli"` | count of POR-induced restarts (diagnostic; survives reboots) |

`blearn_cycle_target` is a compile-time constant (default **2**), not persisted.

**Relationship to the existing toggle:** `GoSettings::battery_learning_enabled` (`go_settings.h:52`, non-persistent, admin opt-in) stays as the **per-session arming** for the *power dashboard / LOW_POWER UX*. The new **persisted** `blearn_stage` is what actually drives the multi-cycle run and auto-resume. Starting a run sets `blearn_stage = Charge, blearn_cycle = 1`; from then on the persisted stage is authoritative across reboots even though the session toggle is false after a cold boot.

**Write discipline:** stage transitions write the single key via `ConfigStore::set_int` + `commit()` directly (not a full `save_go_settings()`), so the commit is prompt and the pre-ship-mode write is atomic. `GoSettings` caches the loaded values at boot for read.

## 2. Where it lives

A small **`BlearnController`** owned by the orchestrator (peer to the existing learning handling in `go_orchestrator.cpp`). It is **driven, not autonomous**: the orchestrator already polls the BMS+FG on a timer (`on_bms_status_timer()` `go_orchestrator.cpp:380`, `on_bms_timer()` `:350`). Each poll passes the fresh `PowerSnapshot` to `BlearnController::tick(const PowerSnapshot&)`, which returns an action for the orchestrator to apply (it does not call hardware directly — keeps it testable host-side, unlike the current FG path which can't host-compile).

```cpp
struct BlearnAction {
  bool set_charge_enabled;   uint16_t charge_current_ma;  // charge control
  bool low_power;            // quiet load (rest) vs full load (discharge)
  bool unplug_cue;           // LED + screen "unplug"
  Screen screen;             // phase screen
  bool commit_then_ship;     // EDV: persist CycleDone, paint, ship-mode
  bool persist_stage;        // write blearn_stage/cycle now
};
BlearnAction BlearnController::tick(const PowerSnapshot&);
```

## 3. Required new surface

1. **Expose FG learning flags on `PowerSnapshot`.** Today only `fg_flag_fc/chg/dsg` are exposed (`go_power.h:60-62`); `qmax_up / res_up / itpor / ocv_taken` are computed in the local `FgSnapshot` (`go_power.cpp:109-126`) and only logged. Add them to `PowerSnapshot` so `BlearnController` and the resume path can read them. *(Low-risk, mechanical.)*
2. **Expose power source + learned-values read on the snapshot/verify path.** `PowerSnapshot` already carries charging state; ensure "external input present?" (plugged vs battery) is readable at boot. Verify uses `read_qmax_cell0()` / `read_ra_table()` / `read_design_capacity_mah()` (already added to `bq27427.h:115/125/102`).
3. **`BlearnController`** (new) + its NVS load/save in `go_settings.{h,cpp}`.
4. **Admin action** "Start battery learning" → `BlearnController::start()` (sets stage=Charge, cycle=1, commits).
5. **Phase screens** — reuse `Screen::DischargeComplete` (`go_display.h:23`); add `Screen` values (or a single status screen with a phase string) for *Charging / Resting / Unplug charger / Verifying / Learning complete / Learning failed*.
6. **Driver: `set_update_status_learning(bool enable)`** on `BQ27427` (new) — sets/clears Update Status bit0+bit1 (subclass 0x52, offset 2) via the existing UNSEAL→CFGUPDATE→block-write→checksum→SOFT_RESET path (mirror `configure_cell`). Set on cycle-1 `Charge` entry, clear on `Complete` (see §10.3). *(Driver `read_qmax_cell0`/`read_ra_table`/`read_design_capacity_mah` already exist: `bq27427.h:115/125/102`.)*
7. **Driver: `read_chem_id()` + `select_chemistry_4v2()`** on `BQ27427` (new) — read `Control(0x0008)`; if ≠ 1202, run `SET_CFGUPDATE`→`Control(0x0031)` (CHEM_B)→`SOFT_RESET`. Idempotent; called from first-boot config **before** any learn (see §10.1). Resets IT learning when it switches.

## 4. Per-poll tick (stage behaviour + exit)

| Stage | Charge | Load (`low_power`) | Cue / screen | Advance when |
|---|---|---|---|---|
| `Charge` | ON, ICHG = `LEARNING_CHARGE_CURRENT_MA` (`go_orchestrator.cpp:449`) | n/a | "Charging…" | `fg.fc()` (Full Charge) → `Rest` |
| `Rest` | OFF (`set_manual_charge_disabled(true)` `go_power.h:141`) | **low** (existing LOW_POWER gates, `go_orchestrator.cpp:466`) | "Resting…" | `fg.ocv_taken()` **and** rest ≥ `CHARGE_REST_TIMEOUT_MS` (500 s) → `Discharge` |
| `Discharge` | OFF | **full** (release LOW_POWER) | **unplug cue** (`set_charge_done_alert(true)` `go_led.h:144`) + "Unplug charger" | `snap.edv_cutoff_reached` → `CycleDone` |
| `CycleDone` | — | — | "Discharge complete" | persist+commit → `trigger_edv_ship_mode()` (device off) |
| `Verify` | OFF | low | "Verifying…" | criteria pass → `Complete`; fail & cycle ≥ cap → `Failed`; fail & < cap → `Charge`(next) |
| `Complete` | normal | normal | success | terminal (normal operation) |
| `Failed` | normal | normal | failure | terminal (await re-arm) |

Note the load polarity is the **only** behavioural subtlety: `Rest` wants quiet load (LOW_POWER on), `Discharge` wants full load (LOW_POWER off). The existing gate already keys on `plugged_in` (`go_orchestrator.cpp:465`), which matches: plugged during Rest, unplugged during Discharge. `BlearnController` makes that explicit per-stage rather than implicit in plug state.

## 5. EDV ordering (the one hard-ordering rule)

Extend `handle_edv_cutoff()` (`go_orchestrator.cpp:1199`) for the blearn case:

```
on edv_cutoff_reached while blearn_stage == Discharge:
    blearn_stage = CycleDone ; ConfigStore.commit()      // MUST confirm
    if (!commit_ok) return;                               // don't ship-mode; retry next poll
    ui.set_screen(DischargeComplete); update_display()
    power_service.trigger_edv_ship_mode()                 // does not return
```

A lost commit must **not** lead to ship-mode (keep discharging, retry) — otherwise the cycle marker is lost across the power-off.

## 6. Boot resume algorithm

Run once at boot, after `load_go_settings()` (`go_settings.cpp:66`) and one `poll_bms()` for fresh FG flags + power source.

```
intact      = (fg.itpor == 0) && fg.qmax_up        // learning present, no POR
on_charger  = snapshot external-input present
mid_run     = stage in {Charge, Rest, Discharge}

switch (stage):
  Idle, Complete (intact)        -> normal operation
  Complete (!intact)             -> stage = Failed; warn "learning lost, re-arm"
  CycleDone (intact, cycle<target)-> stage = Charge; cycle += 1
  CycleDone (intact, cycle>=target)-> stage = Verify
  CycleDone (!intact)            -> itpor_losses++; stage = Charge; cycle = 1   (cap-guarded)
  Verify                         -> re-run verify
  mid_run (intact, on battery)   -> stage = Discharge        // spurious reset mid-discharge; resume
  mid_run (intact, on charger)   -> stage = Charge           // operator re-plugged; restart cycle's charge
  mid_run (!intact)              -> itpor_losses++; stage = Charge; cycle = 1
  Failed                         -> stay Failed; await re-arm
```

`itpor_losses >= ITPOR_LOSS_CAP` (e.g. 3) → `Failed` with a loud log (the EDV margin / cell needs attention). This is the only thing the persisted `blearn_itpor_losses` exists for.

**Auto-resume guard:** the device re-enters the FSM at boot **only when `stage ∉ {Idle, Complete, Failed}`** — i.e. a run is genuinely in progress. A normal field unit (`Idle`/`Complete`) is never hijacked.

## 7. Verify criteria → `Complete`

Read via the existing FG-DUMP path (`read_qmax_cell0`, `read_ra_table`, `read_design_capacity_mah`, `read_flags`, `control_subcommand`). All must hold:

1. `itpor == 0` (no POR since learning).
2. `qmax_up == 1`.
3. Qmax(mAh) within `[0.7×DC, 1.4×DC]` of design capacity.
4. Ra grid healthy: **every value > 0**, and the grid has **moved off ROM defaults across the whole range** (not just the bottom points) with smooth adjacent transitions.

> `res_up` is **not** a sufficient gate — it sets after the *first* Ra update, not a full grid. Criterion 4 (Ra off-defaults across the range) is the real "grid learned" check. Treat `res_up` as a diagnostic only.

On `Complete`, log the verified Qmax raw + the 15 Ra values (these are the golden values for any later baking — out of scope here).

## 8. Failure / escape

- `Charge` never reaches `fc()` within a max-charge window → `Failed`.
- `Discharge` with `plugged_in` still true (operator never unplugged) → unplug cue persists; never force ship-mode while on charger.
- Admin menu item **"Reset battery learning"** → `stage = Idle, cycle = 0, itpor_losses = 0`, commit (operator escape from `Failed`/stuck).

## 9. Files to touch

| File | Change |
|---|---|
| `bq27427.h` / `.cpp` | add `read_chem_id()` + `select_chemistry_4v2()` (CHEM_B→1202, §10.1) and `set_update_status_learning(bool)` (Update Status bit0+bit1, §10.3) |
| `go_hardware_board.cpp` | in `init_bms()` after `configure_cell()`: read Chem ID, select CHEM_B (1202) if ≠ 1202 — the actual charge-not-full fix (§10.1) |
| `go_power.h` / `.cpp` | add `qmax_up/res_up/itpor/ocv_taken` (and external-input-present) to `PowerSnapshot`; populate in `poll_bms()` |
| `go_settings.h` / `.cpp` | add persisted `blearn_stage/blearn_cycle/blearn_itpor_losses` + keys `"bls"/"bln"/"bli"`; load/save |
| `blearn_controller.h` / `.cpp` (new) | the `tick()` FSM + `start()` / `reset()` / `resume_on_boot()` |
| `go_orchestrator.h` / `.cpp` | own a `BlearnController`; call `tick()` from `on_bms_status_timer()`; apply `BlearnAction`; extend `handle_edv_cutoff()` (§5); run resume (§6) at boot |
| `go_display.h` / `.cpp` | phase screens (§3.5) |
| `go_led.*` | reuse `set_charge_done_alert()` for the unplug cue (no change likely) |
| admin menu | "Start battery learning" + "Reset battery learning" actions |

## 10. Prerequisite — resolve before this is a valid production test

Because every unit must verify as learned to ship, three gauge-side settings must be sorted **before** the FSM is a meaningful test. None changes the state machine; all are config writes done at first boot / start-of-run.

1. **Fuel-gauge chemistry must match the 4.2 V cell — Chem ID 1202 (CHEM_B). DO THIS FIRST.** Verified against the BQ27427 TRM (SLUUCD5): the gauge powers up on its **default Chem ID 3230 = a 4.35 V profile** (p7; p18 §4.2), but the charger tops the cell at 4.20 V (`charge_voltage_mv = 4200`). On the 4.35 V profile the chemistry-resident **Taper Voltage ≈ 4250 mV** sits *above* 4.20 V, so the Full-Charge condition `IRateAvg < Taper Rate AND Voltage() > Taper Voltage` (p46 §7.4.2.3.10; default `FC Set% = -1` ⇒ FC = charge-termination detection, p23 §5.4) **can never qualify → `FC` stays 0**, and RemainingCapacity never syncs to FCC (p46) → **SOC saturates ~93 %**. Two observed symptoms (FC=0, SOC=93 % at a fully-tapered 4.20 V), one cause. **Fix:** select **CHEM_B = `Control(0x0031)` → Chem ID 1202 (4.2 V)** via UNSEAL → `SET_CFGUPDATE (0x0013)` → wait CFGUPMODE set (≥1100 ms) → `0x0031` → `SOFT_RESET (0x0042)` → wait CFGUPMODE clear (`CHEMCHANGE` bit confirms). The profile swap carries the whole 4.2 V block — OCV→SOC curve *and* Taper Voltage — so 4.20 V then reads ~100 % and FC can set. **Do NOT hand-patch Taper Voltage; do NOT raise the charger VREG** (the cell is a 4.2 V cell). Make it **idempotent**: read `Control(CHEM_ID = 0x0008)` and switch only if ≠ 1202 — **because changing Chem ID RESETS IT learning** (Qmax/Ra are chemistry-specific; `QMAX_UP`/`RES_UP` clear on the reset). ⇒ chem must be correct **before** any blearn run; switching chem on an already-learned unit forces a full re-learn.

2. **Discharge must reach DISCHARGE state at a safe temperature.** Ra only learns while cell current is above the gauge's Dsg Current Threshold (~120 mA @ DC2000) *and* the grid is normalized at 25 °C. Field data from this board:
   - Device's own full load ≈ **50–70 mA** → below threshold → gauge stays in RELAXATION → Ra does not learn (matches the observed "only bottom-3 grid points" result).
   - A heavy bench load (~2 A) does cross the threshold but self-heats the cell to **~56 °C** → corrupts the 25 °C-normalized grid *and* risks the 60 °C thermal ship-mode trip.
   - **Target a moderate discharge (~C/10–C/5, roughly 200–400 mA) that clears the threshold while staying near room temperature.** Candidate knobs: keep more rails on during discharge to raise the device's own draw; OR **lower the gauge's Dsg Current Threshold register** (`DC/(reg×0.1)`) so the device's own ~60 mA qualifies as DISCHARGE (cheapest — config write, no heat, no bench load). Decide and bench-confirm before relying on the test.

3. **Set Update Status bit0+bit1 at the start of each unit's learn; clear at completion.** For a from-scratch learn the per-update change limits (Max Qmax Change, Ra Max Delta, etc.) must be lifted so Qmax and the whole Ra grid can move freely in as few cycles as possible (`fg_learning_sequence.md:55`). This likely matters as much as the load for getting the mid/high-SOC grid to move in one cycle. In the FSM: set the bits on `Charge`-entry of cycle 1 (subclass 0x52, offset 2, via the existing UNSEAL→CFGUPDATE→commit path used by `configure_cell`); clear them on transition to `Complete` (`fg_learning_sequence.md:93`) so the shipped unit uses normal bounded field-refinement limits.

Order: **chem first** (it resets learning), then load + change-limits. Resolving these three is what determines whether the production test is **1 cycle (feasible) or ≥2 cycles (twice the line time)** — and whether "pass" actually means learned. Background + thresholds: [`fg_learning_sequence.md`](fg_learning_sequence.md); chem mechanism cited against TRM SLUUCD5 pp. 7, 18, 23, 46, 49.
