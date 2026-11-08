"""
LAM Research Challenge 2026 - Stage 3: Day 1 & Day 2 Calibration & Batch Plot Generator
Generates high-resolution publication-quality PNG plots for judges presentation:
1. Flow Sensor Calibration Slopes (K-factors: Red, Blue, Yellow, Flush)
2. Mustard Recipe Batch Volumetric Comparison (Day 1 vs Day 2 Liquid Breakdown)
3. Step 1 Flush Accuracy (Target vs Pre-calibration vs Post-calibration)
4. Step 2 Recirculation Chilling Interlock (Delta-T profile and autonomous handover)
"""

import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def generate_plots():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    output_png = os.path.join(base_dir, "calibration_plots.png")
    
    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(16, 12))
    fig.patch.set_facecolor('#ffffff')
    
    # -------------------------------------------------------------
    # Plot 1: Sensor Calibration Slopes (K-factors)
    # -------------------------------------------------------------
    v = np.linspace(0, 2500, 100)
    ax1.plot(v, v * 2.080, label='Yellow Line (K = 2.080 p/mL)', color='#eab308', linewidth=2.5)
    ax1.plot(v, v * 1.723, label='Flush Water (K = 1.723 p/mL)', color='#0ea5e9', linewidth=2.5, linestyle='--')
    ax1.plot(v, v * 1.034, label='Blue Line   (K = 1.034 p/mL)', color='#3b82f6', linewidth=2.5)
    ax1.plot(v, v * 0.654, label='Red Line    (K = 0.654 p/mL)', color='#ef4444', linewidth=2.5)
    
    # Scatter experimental points
    ax1.scatter([300], [196], color='#ef4444', s=90, zorder=5, label='Red Verified (300 mL -> 196 p)')
    ax1.scatter([300], [310], color='#3b82f6', s=90, zorder=5, label='Blue Verified (300 mL -> 310 p)')
    ax1.scatter([2000], [3443], color='#0ea5e9', s=90, zorder=5, label='Flush Verified (1998 mL -> 3443 p)')
    ax1.scatter([2400], [4992], color='#ca8a04', s=90, zorder=5, label='Yellow Verified (2400 mL -> 4992 p)')
    
    ax1.set_title('Flow Sensor Calibration Slopes (Pulses vs Volume)', fontsize=13, fontweight='bold', pad=12)
    ax1.set_xlabel('Liquid Volume (mL)', fontsize=11)
    ax1.set_ylabel('Sensor Pulses Counted', fontsize=11)
    ax1.grid(True, linestyle='--', alpha=0.6)
    ax1.legend(loc='upper left', frameon=True, fontsize=9)
    ax1.set_xlim(0, 2600)
    ax1.set_ylim(0, 5400)
    
    # -------------------------------------------------------------
    # Plot 2: Fluid Differentiation in Mixer Tank (Day 1 vs Day 2)
    # -------------------------------------------------------------
    categories = ['Target Recipe', 'Day 1 Initial', 'Day 2 Demo (Today)']
    paint_vols = [3000, 3100, 3050]
    water_vols = [0, 100, 100]
    
    bar_width = 0.5
    bars_paint = ax2.bar(categories, paint_vols, bar_width, label='Concentrated Paint Mix', color=['#2563eb', '#d97706', '#16a34a'])
    bars_water = ax2.bar(categories, water_vols, bar_width, bottom=paint_vols, label='Displaced Line Water (Hold-up)', color='#38bdf8', alpha=0.85, hatch='//')
    
    ax2.set_title('Mixer Tank Volumetric Breakdown (Mustard Recipe)', fontsize=13, fontweight='bold', pad=12)
    ax2.set_ylabel('Total Liquid Volume (mL)', fontsize=11)
    ax2.set_ylim(0, 3800)
    ax2.grid(axis='y', linestyle='--', alpha=0.6)
    ax2.legend(loc='upper left', frameon=True, fontsize=9)
    
    # Annotations on bars
    ax2.text(0, 1500, '3,000 mL Paint\n(100% Target)', ha='center', va='center', color='white', fontweight='bold', fontsize=10)
    ax2.text(1, 1550, '~3,100 mL Paint\n(96.9%)', ha='center', va='center', color='white', fontweight='bold', fontsize=10)
    ax2.text(1, 3150, '~100 mL Water', ha='center', va='center', color='#0f172a', fontweight='bold', fontsize=8)
    ax2.text(1, 3250, '3,200 mL (+6.7%)', ha='center', va='bottom', color='#b91c1c', fontweight='bold', fontsize=10)
    
    ax2.text(2, 1525, '~3,050 mL Paint\n(+1.67% Error)', ha='center', va='center', color='white', fontweight='bold', fontsize=10)
    ax2.text(2, 3100, '~100 mL Water', ha='center', va='center', color='#0f172a', fontweight='bold', fontsize=8)
    ax2.text(2, 3200, '3,150 mL (+5.0% Gross)', ha='center', va='bottom', color='#15803d', fontweight='bold', fontsize=10)
    
    # -------------------------------------------------------------
    # Plot 3: Step 1 Flush Line Accuracy (Line 3 into 2.0 L Jug)
    # -------------------------------------------------------------
    flush_cases = ['Target Volume', 'Uncalibrated (Trial 14)', 'Calibrated (Trial 15)']
    flush_vols = [2000.0, 1200.0, 1998.0]
    flush_colors = ['#64748b', '#ef4444', '#0ea5e9']
    
    bars_flush = ax3.bar(flush_cases, flush_vols, width=0.45, color=flush_colors)
    ax3.axhline(2000.0, color='#64748b', linestyle='--', linewidth=1.5, alpha=0.7)
    
    ax3.set_title('Step 1: 2.0 L Flush Line Calibration (Line 3 into Jug)', fontsize=13, fontweight='bold', pad=12)
    ax3.set_ylabel('Delivered Water Volume (mL)', fontsize=11)
    ax3.set_ylim(0, 2400)
    ax3.grid(axis='y', linestyle='--', alpha=0.6)
    
    ax3.text(0, 2030, '2,000 mL (Set Target)', ha='center', va='bottom', color='#334155', fontweight='bold', fontsize=10)
    ax3.text(1, 1230, '1,200 mL (-40.0%)\nViscosity Mismatch (K=1.034)', ha='center', va='bottom', color='#b91c1c', fontweight='bold', fontsize=9)
    ax3.text(2, 2030, '1,998.0 mL (-0.10%)\n99.90% Accuracy (K=1.723)', ha='center', va='bottom', color='#0369a1', fontweight='bold', fontsize=10)
    
    # -------------------------------------------------------------
    # Plot 4: Step 2 Thermal Interlock Profile (Chilling Loop)
    # -------------------------------------------------------------
    time_pts = np.linspace(0, 60, 100)
    t_ref = np.full_like(time_pts, 28.5)
    t_cool = 28.5 - 10.5 * (1 - np.exp(-time_pts / 22.0))
    delta_t = t_ref - t_cool
    
    ax4.plot(time_pts, t_ref, label='TS2: Yellow Tank Ref (28.5 °C)', color='#eab308', linewidth=2.0)
    ax4.plot(time_pts, t_cool, label='TS1: Red Tank Cooled (Ice Coil)', color='#ef4444', linewidth=2.2)
    ax4.plot(time_pts, delta_t, label='Delta-T (|TS2 - TS1|)', color='#8b5cf6', linewidth=2.5)
    
    ax4.axhline(5.0, color='#10b981', linestyle=':', linewidth=2, label='Demonstration Setpoint (Delta T >= 5.0 °C)')
    ax4.axhline(10.0, color='#6366f1', linestyle='--', linewidth=1.5, label='Formal Task Sheet Setpoint (Delta T >= 10.0 °C)')
    
    # Handover marker
    idx_handoff = np.where(delta_t >= 5.0)[0][0]
    ax4.scatter([time_pts[idx_handoff]], [delta_t[idx_handoff]], color='#10b981', s=120, zorder=6)
    ax4.annotate('Auto-Handover to Step 3\n(Zero operator touch)', 
                 xy=(time_pts[idx_handoff], delta_t[idx_handoff]), 
                 xytext=(time_pts[idx_handoff] + 5, delta_t[idx_handoff] - 2),
                 arrowprops=dict(arrowstyle="->", color='#10b981', lw=2),
                 fontweight='bold', color='#047857', fontsize=9)
    
    ax4.set_title('Step 2: Thermal Interlock Recirculation (SV5 + Pump 4)', fontsize=13, fontweight='bold', pad=12)
    ax4.set_xlabel('Recirculation Time (s)', fontsize=11)
    ax4.set_ylabel('Temperature (°C)', fontsize=11)
    ax4.set_xlim(0, 60)
    ax4.set_ylim(15, 32)
    ax4.grid(True, linestyle='--', alpha=0.6)
    ax4.legend(loc='center right', frameon=True, fontsize=8.5)
    
    plt.tight_layout()
    plt.savefig(output_png, dpi=300)
    print(f"Comprehensive 4-panel calibration plot saved successfully to: {output_png}")

if __name__ == "__main__":
    generate_plots()
