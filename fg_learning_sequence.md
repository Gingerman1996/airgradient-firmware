# BQ27427 Impedance-Track learning cycle — canonical sequence

**Source of truth:** TI sluucd5 (BQ27427 Technical Reference Manual, Jan 2023). Every claim below carries a page + section citation from that document. This describes what the learning cycle *should* be per the datasheet — independent of the current AirGradient Go code.

**Sign convention:** charge current positive, discharge current negative. "EffectiveCurrent" is the gauge's internal filtered current used for state decisions (Fig 7-1, p42).

---

## Key thresholds (all scale with Design Capacity, DC)

From §7.4.2.2.1 Current Thresholds (p41–42), the threshold = `DC / (RegisterValue × 0.1)`:

| Register | Default | Formula | @ DC=1340 | @ DC=2000 (board config) |
|---|---|---|---|---|
| **Dsg Current Threshold** | 167 | DC/16.7 | 80 mA | **120 mA** |
| **Chg Current Threshold** | 100 | DC/10 | 134 mA | 200 mA |
| **Quit Current** | 250 | DC/25 | 53.6 mA | **80 mA** |
| **Dsg Relax Time** | 60 s | — | 60 s | 60 s |
| **Chg Relax Time** | 60 s | — | 60 s | 60 s |
| **Quit Relax Time** | 1 s | — | 1 s | 1 s |
| **OCV Wait Time** (§7.4.2.1.1, p36) | 60 s | — | 60 s | 60 s |

Other learning-critical registers:
- **Terminate Voltage** (§7.4.2.3.6, p45): default 3200 mV, range 2500–3700 mV. SOC/RemainingCapacity forced to 0 at `Terminate Voltage + Delta Voltage` after the voltage dips below TermV for **TermV Valid t = 2 s** (§7.4.2.1.19, p40).
- **Q Invalid MaxV = 3811 mV, Q Invalid MinV = 3750 mV** (§7.4.4.1.1, p49): OCV taken inside this flat band is rejected for Qmax.
- **Sleep Current** (§7.4.2.3.11, p46): default **10 mA** (NOT 50). SLEEP entered only if `OpConfig[SLEEP]=1` (default 1, §7.4.1.4.1 p34) AND `|AverageCurrent| < Sleep Current` (Fig 2-1, p12).
- **Design Capacity / Design Energy** (§7.4.2.3.5, p45): default 1340 mAh / 4960 mWh. `Qmax(mAh) = Qmax Cell 0 × Design Capacity / 2^14` (§7.4.2.3.1, p43).

---

## The IT state machine (§7.4.2.2.1 + Fig 7-1, p41–42)

Three states: **CHARGE, RELAXATION, DISCHARGE**. Transitions (EffectiveCurrent):

- **→ CHARGE**: current rises above `+Chg Current Threshold` for **Charge Relax Time** (60 s).
- **CHARGE → RELAXATION**: current falls below `+Quit Current` for **Charge Relax Time** (60 s).
- **RELAXATION → DISCHARGE**: current falls below `−Dsg Current Threshold` for **Quit Relax Time** (1 s).
- **DISCHARGE → RELAXATION**: current rises above `−Quit Current` for **Dsg Relax Time** (60 s).

**Why this matters — there are TWO Qmax paths and they gate differently:**
- **OCV-pair Qmax** (the primary path, §2.1, p9: "comparing states of charge before and after applying the load with the amount of charge passed"). Needs two valid relaxed OCV endpoints — each taken in **RELAXATION** (current below **Quit Current**, 80 mA @ DC2000) with `OCVTAKEN` set (§5.4, p23) and `VOK` true (§5.1.1, p21) — bracketing a known coulomb count. **This path does not itself require the gauge to enter DISCHARGE**; the charge passed is integrated regardless of IT state. `QMAX_UP` (§5.1.1) is the generic "Qmax updated" bit and can set from a clean OCV1/OCV2 bracket. *(Caveat: the TRM documents the OCV-pair principle in §2.1 but only spells out the **Fast Qmax** mechanism explicitly in the IT Cfg registers; it does not state in so many words that `QMAX_UP` sets with zero qualified discharge. So treat "OCV-pair Qmax without any discharge" as datasheet-plausible but not datasheet-guaranteed — which is one more reason the canonical cycle below mandates a real discharge anyway.)*
- **Ra (resistance grid)** and **end-of-discharge Fast-Qmax refinement** (§7.4.2.1.6/7, p37) need the gauge in **DISCHARGE** → current below `−Dsg Current Threshold` (120 mA @ DC2000).
- **SLEEP** (power state) is gated by **Sleep Current** (10 mA default), a *third, independent* register. **SLEEP is neither necessary nor sufficient for learning** — it only changes the update cadence (1 s NORMAL → 20 s SLEEP, with a brief auto-wake ~every 48 s, §2.4.4/2.4.5, p10–11) and exits when `|AverageCurrent| > Sleep Current` or instantaneous current exceeds ±30 mA. Learning is gated by the IT state machine above, not by SLEEP.

Consequence for a discharge held in the **80–120 mA gap** (or below): the gauge stays in **RELAXATION** the whole "discharge" and **never enters DISCHARGE**. Result is **OCV-pair Qmax may still land** (if OCV1/OCV2 are both valid) but you lose the **Fast-Qmax** end-of-discharge refinement **and all Ra grid learning**. It is *not* "Qmax fine, Ra dead" — it is "OCV-pair Qmax only, Fast-Qmax lost, Ra dead."

---

## Canonical learning sequence

### Phase 0 — Configure for learning (CONFIG UPDATE)
1. UNSEAL, then `SET_CFGUPDATE` (0x0013); wait ≥1100 ms (§5.1.9, p21). **Complete all config writes in this phase well within ~240 s** — the gauge auto-exits CONFIG UPDATE after ~240 s without a `SOFT_RESET` and any later writes are lost (§2.4.3, p10).
2. Set **Design Capacity / Design Energy** to the real cell (§7.4.2.3.5). Set **Chem ID** to the matching chemistry (`CHEM_A/B/C` or ChemID bits, §5.1.15 p22 / OpConfigD §7.4.1.4.4 p35). *(For packs > 6 Ah set Design Energy Scale = 10, §7.4.2.1.23 — not needed at DC=2000, scale 1.)*
3. Set **Terminate Voltage** to the lowest safe end-of-discharge voltage for the system (§7.4.2.3.6).
4. **Set Update Status bit0 (0x01) AND bit1 (0x02)** (subclass 82, offset 2, §7.4.2.3.2, p43). Quote: *"Only if a learning cycle is to be completed during initial configuration of the gauge's golden file should bit 0 and bit 1 be set."* Setting them removes the per-update limits (Max Qmax Change 20% §7.4.2.1.8, Qmax Max Delta% 10% §7.4.2.1.9; Ra limits Ra Filter §7.4.2.1.2 / Ra Max Delta §7.4.2.1.16) so the first cycle can move Qmax/Ra freely.
5. `SOFT_RESET` (0x0042) to exit CONFIG UPDATE → clears `ITPOR` and `CFGUPMODE`, runs an OCV measurement (§5.1.17, p22).
6. **Wait for `CONTROL_STATUS [INITCOMP] = 1`** before trusting any prediction (§5.1.1, p21 / §2.4.2, p10) — initialization must complete first.
7. **Ensure gauging has actually started:** `Flags[BAT_DET]` must be set, or "gauge predictions are not valid" (§5.4, p23). With `OpConfig[BIE]=1` this comes from the BIN-pin insertion edge; with `BIE=0` the host must issue `BAT_INSERT` (0x000C), which *"also starts Impedance Track gauging"* (§5.1.7, p21).
8. Confirm fast learning armed: `CONTROL_STATUS [QMAX_UP]=0` and `[RES_UP]=0` (cleared on POR/`BAT_DET`; "when cleared, enables fast learning," §5.1.1, p21).
9. **Coulomb-counter autocal (CCA)** runs ~3 min 45 s after init and periodically thereafter (§5.1.1, p20). Don't begin the precision discharge integration inside that first ~4 min window, and expect periodic CCA to briefly perturb `AverageCurrent()`.

### Phase 1 — Full charge
10. Charge so EffectiveCurrent > Chg Current Threshold → gauge enters **CHARGE** (DSG flag cleared, §7.4.2.2.1).
11. **Primary Charge Termination** (§7.4.2.3.10, p46) qualifies when, over two consecutive 40 s windows: `IRateAvg1 < Taper Rate` AND `IRateAvg2 < Taper Rate` (Taper Rate 100 = 0.1 h-rate) AND `Voltage() > Taper Voltage`. On qualify → `Flags[FC]` set, `[CHG]` cleared. (`FC Set% = −1` default ⇒ FC = charge-termination detection, §5.4 p23 / §7.4.1.2.2 p33.) **Note a datasheet inconsistency:** Taper Voltage is listed as **4100 mV** in §7.4.2.3.10 (p46) but **4250 mV** in §7.4.4.1.3 (p49) — same parameter, two stated defaults; the active value is whatever the chosen Chem profile writes.

### Phase 2 — Top relaxation → OCV1
12. Stop/disable charge so current falls below **Quit Current**. After **Charge Relax Time (60 s)** the gauge enters **RELAXATION** (`OCVTAKEN` cleared). *(This is the **Charge** Relax Time, not Dsg Relax Time — Fig 7-1, p42. Both default to 60 s, so the number is unaffected, but the post-charge transition is timed by Charge Relax Time.)*
13. After **OCV Wait Time (60 s)** the gauge measures OCV → `OCVTAKEN` set. This is **OCV1** (defines DOD at the top). Total ≈ **120 s** after current drops sub-Quit-Current.
14. **OCV1 must be > Q Invalid MaxV (3811 mV)** to be valid. A freshly-terminated Li-ion relaxes to ~4.1–4.2 V → satisfied. *Current in this window should be as close to 0 as possible* — any residual load below Quit Current still depresses the reading slightly, but must at minimum be < Quit Current.

### Phase 3 — Discharge (the actual learning workload)
15. Discharge at a current **above Dsg Current Threshold** (≈120 mA @ DC2000, i.e. ≥ ~C/16.7) so the gauge enters **DISCHARGE** (after Quit Relax Time 1 s), **and keep it discharging for > 500 s** (hard requirement — see below). DISCHARGE mode is required for:
    - **Ra grid updates** (§7.4.3, p48) — resistances learned per SOC grid point during discharge. `Res V Drop` (§7.4.2.1.3, p37) qualifies the measurement.
    - **Avg I/P Last Run** and **Delta Voltage** — only updated **after a discharge of at least 500 s has occurred and stopped** (§7.4.2.3.13 p47, §7.4.2.1.18 p40). Treat the **>500 s** discharge duration as a hard prerequisite, not just crossing the Dsg threshold momentarily.
    - **Fast-Qmax measurements** to accumulate (§7.4.2.1.6, p37).
    (Recall from above: **OCV-pair Qmax** does *not* need this DISCHARGE segment — but Ra and the Fast-Qmax refinement do, and a real discharge is the only way to *guarantee* Qmax lands.)
16. TI's IT simulator default load band is **C/1 to C/20** (Max Sim Rate default 1 → C/1, Min Sim Rate default 20 → C/20; §7.4.2.1.15, p39 — the prose's "2 implies C/2" is just an example of the inversion, not the default). A discharge in this band is ideal; C/16.7 is merely the floor to register a discharge at all.
17. A **rising-current profile near empty** (e.g. constant-power load: I rises as V sags) is explicitly accommodated by **Fast Scale Load Select** (§7.4.2.1.24, p40, default 3 = 14 s average) — designed for "discharge at a relatively light load during most of the discharge, but the load increases dramatically near the end."
18. **Fast Qmax start** triggers when `DOD > Fast Qmax Start DOD% (92%)` OR `Voltage < Terminate Voltage + Fast Qmax Start Volt Delta (125 mV)`, AND `current < C/Fast Qmax Current Threshold (C/4)` (§7.4.2.1.6, p37).

### Phase 4 — Bottom: terminate → relaxation → OCV2
19. Discharge until `Voltage()` dips below **Terminate Voltage** for **TermV Valid t (2 s)** → SOC/RemainingCapacity forced to 0 (§7.4.2.1.19). SOC reaches 0 % at `Terminate Voltage + Delta Voltage` (§7.4.2.3.6) — note **Delta Voltage default is 1 mV** (§7.4.2.3.15, p47), a learned value bounded by Min/Max Delta Voltage; do **not** confuse it with **Max Delta Voltage = 200 mV** (§7.4.2.1.17, p39).
20. **Stop the discharge** (remove load). Current rises above `−Quit Current`; after **Dsg Relax Time (60 s)** → RELAXATION; after **OCV Wait Time (60 s)** → **OCV2**. ≈120 s after the load is removed.
21. **OCV2 must be < Q Invalid MinV (3750 mV)** to be valid. A cell at ~Terminate Voltage (3.0–3.2 V) relaxed is well below → valid, deep DOD.
22. **Fast Qmax computed at end of discharge** when `#measurements > Fast Qmax Min Points (3)` AND `DOD > Fast Qmax End DOD% (96%)` (§7.4.2.1.7, p37).

### Phase 5 — Verify the update landed
23. `CONTROL_STATUS [QMAX_UP]` should now be **1** (Qmax updated, §5.1.1). `[VOK]` should have been 1 across the OCV windows.
24. **`RES_UP` can only set *after* Qmax is updated** (§5.1.1, p21): *"this bit can only be set after Qmax is updated ([QMAX_UP] bit is set)."* The Ra grid *is* measured during cycle-1 discharge (the Ra table can move), but the formal `[RES_UP]` "impedance learning complete" flag is sequenced behind `[QMAX_UP]` — so it typically lands on the **discharge after** Qmax updated. Plan for ≥1 full cycle for Qmax and a further qualified discharge to confirm `RES_UP`.
25. Confirm `Flags[ITPOR] = 0` — if a POR/reset occurred (e.g. pack protector cut PACK+), RAM is wiped and learning is lost (§2.4.1 p10, §2.4.2 p10). This is the failure mode the EDV/ship-mode cutoff exists to prevent.

### Phase 6 — Finalize golden image
26. Re-enter CONFIG UPDATE; **clear Update Status bit0+bit1** back to 0 so production units apply the normal change limits (§7.4.2.3.2).
27. Read learned **Qmax Cell 0** (§7.4.2.3.1) and the **Ra table** (15 values, §7.4.3) — bake into the golden image flashed to other units. Sanity-check the Ra table: no negative values, smooth grid-to-grid transitions (§7.4.3 p48).
28. SEAL (`SEALED` 0x0020 sets Update Status bit7, §5.1.13 p22).

---

## Temperature & environment
- Ra is normalized to **25 °C** (§7.4.3, p48). Run the learning cycle near 25 °C; extremes corrupt the grid. `T Rise` (§7.4.2.3.7) / `T Time Constant` (§7.4.2.3.8) model self-heating but don't substitute for a controlled ambient.

## The single biggest correction vs the intuitive model
SLEEP is *not* the learning enabler. The three current regimes are governed by three separate registers:
- `|I| < Sleep Current (10 mA)` → **SLEEP** (power only, no learning implication)
- `|I| < Quit Current (80 mA)` → **RELAXATION** → OCV / Qmax eligible
- `|I| > Dsg Current Threshold (120 mA)` → **DISCHARGE** → Ra grid + Fast-Qmax measurements

A complete cycle therefore wants current **below Quit Current at the two OCV windows** and **above Dsg Current Threshold for > 500 s during the discharge** — i.e. a profile that is quiet at top, loaded through the middle, and (optionally) rising near empty. Holding current minimal across the whole discharge yields at best an **OCV-pair-Qmax-only** result (Fast-Qmax refinement + Ra grid both lost).
