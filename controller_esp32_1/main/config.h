// ESP32 #1 (U2) "controller node" - pin map, recipe and tuning constants.
// LAM Research Challenge 2026 - Stage 3 - Day 1 (Industrial Paint Mixing)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"

// ============================================================================
// 1. PIN MAP: POPULATED FROM SCHEMATIC SHEET 1
// ============================================================================
static const int PIN_SV[6]  = {23, 22, 21, 19, 18, 13};  // SV1..SV6: GPIO 23, 22, 21, 19, 18, 13 (HIGH = valve ON)
static const int PIN_PWM[5] = {32, 33, 25, 26, 27};      // PWM_1..PWM_5: GPIO 32, 33, 25, 26, 27 -> Cytron MDD10A
static const int PIN_SW     = -1;                        // start switch: -1 (Disabled - started via Serial Studio 'start'/'batch')
static const int PIN_FSW[4] = {36, 39, 34, 35};          // FSW1..FSW4: GPIO 36, 39, 34, 35, active LOW

// UART link to ESP32 #2 (Serial2)
#define LINK_UART_NUM  UART_NUM_2
#define LINK_RX_PIN    16
#define LINK_TX_PIN    17
#define LINK_BAUD_RATE 115200

// ============================================================================
// 2. P&ID MAPPING (Day 1 & Day 2 Architecture):
//    - Red Line:    Driver 1 Ch1 (M1=PWM_1 / GPIO 32), Solenoid SV1 (GPIO 23), Flow sensor FT1 (FS1)
//    - Yellow Line: Driver 1 Ch2 (M2=PWM_2 / GPIO 33), Solenoid SV2 (GPIO 22), Flow sensor FT2 (FS2)
//    - Blue Line:   Driver 2 Ch1 (M1=PWM_3 / GPIO 25), Solenoid SV3 (GPIO 21), Flow sensor FT3 (FS3)
//    - Flush Line:  Blue Pump / Line 3, Solenoid SV4 (GPIO 19), Flow sensor FT4 (FS4)
//    - Chilling:    Driver 2 Ch2 (M2=PWM_4 / GPIO 26), Solenoid SV5 (GPIO 18) -> Ice Tank Recirculation
// ============================================================================
enum Line { L_RED = 0, L_BLUE = 1, L_YELLOW = 2, L_FLUSH = 3, NUM_LINES = 4 };
static const char* LINE_NAME[NUM_LINES] = {"RED", "BLUE", "YELLOW", "FLUSH"};
static const bool  LINE_ENABLED[NUM_LINES] = {true, true, true, true}; // All lines enabled for Day 2

static const int LINE_PUMP[NUM_LINES] = {0, 2, 1, 2};   // Red->PWM_1, Blue->PWM_3, Yellow->PWM_2, Flush->PWM_3 (Blue pump)
static const int LINE_SV[NUM_LINES]   = {0, 2, 1, 3};   // Red->SV1,   Blue->SV3,   Yellow->SV2,   Flush->SV4
static const int LINE_FT[NUM_LINES]   = {0, 2, 1, 2};   // Red->FS1,   Blue->FS3,   Yellow->FS2,   Flush->FS3 (FT-03)

// Day 2 Chilling Circuit Hardware Mapping (Schematic Sheet 1)
static const int SV_CHILL_IDX   = 4;                    // SV-05: GPIO 18 (Solenoid Valve 5 on PCB)
static const int PUMP_CHILL_CH  = 3;                    // Motor Driver 2 M2: GPIO 26 (PWM_4 on PCB) -> Pump 4
static const int SV_TANK_IDX    = 4;                    // Legacy alias
static const int SV_WASTE_IDX   = 5;                    // SV-06: (spare on PCB)
static const bool HAVE_WASTE_VALVE = false;             // false = direct lines to tank, no waste valve
static const float TARGET_DELTA_T = 5.0f;               // Day 2 Interlock Threshold: 5.0 C for fast bench demo, 10.0 C for formal task sheet

// ============================================================================
// 3. RECIPE: Mustard = 80 % yellow, 10 % blue, 10 % red of 3000 mL
// ============================================================================
static const float BATCH_ML = 3000.0f;
static const float PCT[NUM_LINES] = {10.0f, 10.0f, 80.0f, 0.0f};   // by line: R, B, Y, (flush)
static const int   DOSE_ORDER[3]  = {L_YELLOW, L_BLUE, L_RED};     // big dose first (2400 mL), small ones last (300 mL each)

// ============================================================================
// 4. PER-LINE CALIBRATION (Empirically Characterized on Physical Rig)
//    - Line 0 (Red / T1 / FS1):     Target 300 mL (10%)  -> K = 0.654 p/mL, Lead = 2.5 mL
//    - Line 1 (Blue / T2 / FS2):    Target 300 mL (10%)  -> K = 1.034 p/mL, Lead = 2.5 mL
//    - Line 2 (Yellow / T3 / FS3):  Target 2400 mL (80%) -> K = 2.080 p/mL, Lead = 3.5 mL
//    - Line 3 (Flush / FT4 / FS4):  Target 2000 mL       -> K = 1.723 p/mL (exact 2.0 L water calibration)
// ============================================================================
static const float DEFAULT_LINE_K[NUM_LINES]        = {0.654f, 1.034f, 2.080f, 1.723f}; // empirical p/mL
static const float DEFAULT_LINE_FAST[NUM_LINES]     = {100.0f, 100.0f, 100.0f, 100.0f}; // Fast PWM duty % (Max flow: 10 mL/s)
static const float DEFAULT_LINE_SLOW[NUM_LINES]     = {100.0f, 100.0f, 80.0f,  100.0f}; // Red & Blue run 100% PWM throughout
static const float DEFAULT_LINE_LEAD[NUM_LINES]     = {2.5f,   2.5f,   3.5f,   2.5f};   // Early cutoff lead in mL
static const float DEFAULT_LINE_SLOWFRAC[NUM_LINES] = {0.0f,   0.0f,   0.05f,  0.0f};   // Red & Blue run constant duty; Yellow has 5% tail
static const float SLOW_MIN_ML                      = 10.0f;  // Minimum volume in slow zone (mL)

// ============================================================================
// 5. TIMING, SAFETY, TOP-UP
// ============================================================================
static const uint32_t PWM_HZ            = 5000;
static const uint32_t SV_OPEN_LEAD_MS   = 200;
static const uint32_t START_RAMP_MS     = 400;
static const uint32_t SETTLE_MS         = 1500;   // valve stays open this long so coast pulses are counted
static const uint32_t NOFLOW_FAULT_MS   = 10000;  // < 0.3 mL progress in 10 s = fault (prevents false aborts on air lock)
static const uint32_t DOSE_TIMEOUT_MS   = 360000;  // 360 s (6 min) timeout for 2,400 mL Yellow at 10 mL/s
static const uint32_t LINK_TIMEOUT_MS   = 400;    // no sensor frame for this long while running = fault
static const bool     USE_MIX_HIGH_FLOAT = false; // FSW4 as mixing-tank overfill guard (active LOW)

// Top-up (approaches target from below only; never overshoots)
static const int      TOPUP_MAX            = 3;
static const float    TOPUP_TRIGGER_ML     = 1.5f;   // only top up if short by more than 1.5 mL
static const float    TOPUP_AIM            = 0.7f;   // burst aims at 70 % of remaining volume
static const uint32_t TOPUP_MIN_BURST_MS   = 60;
static const uint32_t TOPUP_MAX_BURST_MS   = 1500;
static const uint32_t TOPUP_SETTLE_MS      = 900;

// ============================================================================
// 6. TYPES
// ============================================================================
struct LineCfg {
    float kPpm;       // pulses per mL
    float dutyFast;   // %
    float dutySlow;   // %
    float leadMl;     // mL to stop early
    float slowFrac;   // fraction of dose run slow
};

struct DoseResult {
    float    targetMl;
    float    deliveredMl;
    float    stopMl;
    float    coastMl;
    float    secs;
    uint32_t pulses;
};

