r"""
LAM Research Challenge 2026 - Stage 3: Industrial Paint Mixing
Physical Rig Characterization & Volumetric Accuracy Analysis
Workspace: d:\LAM Final

Parses analysis/data/calibration_trials.csv:
- Day 1: Individual Flow Transmitter (FT) Water Characterization & Initial Paint Batch
- Day 2: Step 1 Flush Line Calibration (2.0 L) & Step 3 Full Autonomous Demo Batch (3,150 mL)
"""

import os
import csv

def load_data(csv_path=None):
    if csv_path is None:
        base_dir = os.path.dirname(os.path.abspath(__file__))
        csv_path = os.path.join(base_dir, "data", "calibration_trials.csv")
    
    trials = []
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            trials.append({
                "trial_id": int(row["trial_id"]),
                "day": row.get("day", "DAY_1").strip(),
                "fluid": row.get("fluid", "WATER").strip(),
                "line": row["line"].strip(),
                "test_type": row["test_type"].strip(),
                "duration_s": float(row["duration_s"]),
                "pwm_pct": float(row["pwm_pct"]),
                "pulses": int(row["pulses"]),
                "target_ml": float(row["target_ml"]),
                "actual_ml": float(row["actual_ml"]),
                "flow_mlps": float(row["flow_mlps"]),
                "k_factor": float(row["k_factor_calc"]),
                "error_pct": float(row["error_pct"]),
                "notes": row["notes"].strip()
            })
    return trials

def analyze():
    trials = load_data()
    print("=" * 115)
    print("           LAM RESEARCH CHALLENGE 2026 - STAGE 3 COMPREHENSIVE TRIAL DATA")
    print("           Day 1 & Day 2 Physical Characterization, Flush & Recipe Verification")
    print("=" * 115)
    print(f"{'ID':<3} | {'Day':<6} | {'Fluid':<9} | {'Line':<7} | {'Type':<16} | {'Target':<8} | {'Delivered':<9} | {'K(p/mL)':<7} | {'Error%':<7} | Notes")
    print("-" * 115)
    
    for t in trials:
        print(f"#{t['trial_id']:<2} | {t['day']:<6} | {t['fluid']:<9} | {t['line']:<7} | {t['test_type']:<16} | {t['target_ml']:>6.1f} mL | {t['actual_ml']:>7.1f} mL | {t['k_factor']:<7.3f} | {t['error_pct']:>+6.2f}% | {t['notes']}")
    
    print("=" * 115)
    print("\n--- 1. FLOW TRANSMITTER (FT) LOCKED CALIBRATION CONSTANTS ---")
    calibrated = [t for t in trials if t["test_type"] == "calibrated_dose" or t["test_type"] == "step1_verified"]
    for t in calibrated:
        print(f"  Line {t['line']:<6} ({t['fluid']:<5}): K = {t['k_factor']:.3f} p/mL | Target = {t['target_ml']:>6.1f} mL | Delivered = {t['actual_ml']:>6.1f} mL | Accuracy = {100.0 - abs(t['error_pct']):.2f}%")

    print("\n--- 2. DAY 2 STEP 1 FLUSH (LINE 3 INTO 2.0 L JUG) ---")
    flush_runs = [t for t in trials if t["line"] == "FLUSH"]
    for f in flush_runs:
        print(f"  [{f['test_type']}] Target: {f['target_ml']:.1f} mL | Delivered: {f['actual_ml']:.1f} mL | Error: {f['error_pct']:+.2f}% | Pulses: {f['pulses']}")

    print("\n--- 3. MUSTARD RECIPE BATCH PERFORMANCE COMPARISON ---")
    batches = [t for t in trials if "batch" in t["test_type"]]
    for b in batches:
        print(f"  [{b['day']} - {b['test_type']}]")
        print(f"    Target Recipe Volume   : {b['target_ml']:.1f} mL (3.0 Litres)")
        print(f"    Gross Liquid Collected : {b['actual_ml']:.1f} mL ({b['actual_ml']/1000.0:.3f} L) [{b['error_pct']:+.2f}% gross error]")
        print(f"    Liquid Decomposition   : ~100.0 mL line hold-up displacement + {b['actual_ml']-100.0:.1f} mL actual paint mix")
        print(f"    Net Paint Dosing Error : {(b['actual_ml'] - 100.0 - b['target_ml']) / b['target_ml'] * 100.0:+.2f}%")
        print(f"    Batch Colour Outcome   : {b['notes']}")
        print()
    print("=" * 115)

if __name__ == "__main__":
    analyze()
