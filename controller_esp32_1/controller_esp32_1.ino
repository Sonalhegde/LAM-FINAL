// LAM Research Challenge 2026 - Stage 3 - Day 1 (Industrial Paint Mixing)
// ESP32 #1 (U2) = CONTROLLER NODE
//   Drives 4 pumps (MDD10A PWM), 6 solenoid valves, reads the start switch and float switches,
//   receives flow-pulse totals from ESP32 #2 and runs the dosing sequence.
//
// Dose = open valves -> ramp pump to FAST -> at (100 % - slow zone) drop to SLOW ->
//        stop at (target - lead) -> keep valve open while coast pulses are counted ->
//        optional top-up bursts (from below only) -> close valves.
//
// USB serial 115200: type 'help'.  Telemetry frame during a dose (Serial Studio compatible):
//   /*time_s,line,phase,target_mL,delivered_mL,duty_pct,flow_Lmin*/   phase 1 = fast, 2 = slow
//
// Board: ESP32 Dev Module, Arduino-ESP32 core 3.x.

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

#define LINK Serial2

static LineCfg     cfg[NUM_LINES];
static Preferences prefs;
static float       gTrunkMl = V_TRUNK_ML_DEFAULT;

static volatile bool gBusy = false, gAbort = false, gStartReq = false;
static const char*   gFault = nullptr;

static uint32_t cnt[5] = {0, 0, 0, 0, 0};     // pulse totals from ESP32 #2
static uint32_t gFsw = 0, lastFrameMs = 0, goodFrames = 0, badFrames = 0;
static bool     linkSeen = false;

static float    gTemp1 = -999.0f;              // TS1: Red Tank (°C)
static float    gTemp2 = -999.0f;              // TS2: Yellow Tank (°C)

static float pumpPct[5]  = {0, 0, 0, 0, 0};
static bool  valveOn[6]  = {false, false, false, false, false, false};

static double calN[NUM_LINES] = {0, 0, 0, 0};
static double calV[NUM_LINES] = {0, 0, 0, 0};
static int    calCount[NUM_LINES] = {0, 0, 0, 0};
static int      lastRunLine = -1;
static uint32_t lastRunPulses = 0;

// ---------------------------------------------------------------------------
// Hardware outputs
// ---------------------------------------------------------------------------
static void setValve(int idx, bool on) {
  if (idx < 0 || idx > 5) return;
  if (idx == SV_WASTE_IDX && !HAVE_WASTE_VALVE) return;
  valveOn[idx] = on;
  if (PIN_SV[idx] >= 0) digitalWrite(PIN_SV[idx], on ? HIGH : LOW);
}

static void allValvesOff() {
  for (int i = 0; i < 6; i++) setValve(i, false);
}

static void setPumpPct(int ch, float pct) {
  if (ch < 0 || ch > 4) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  pumpPct[ch] = pct;
  if (PIN_PWM[ch] >= 0) ledcWrite(PIN_PWM[ch], (uint32_t)(pct * 255.0f / 100.0f + 0.5f));
}

static void setChilling(bool on, float pct = 100.0f) {
  if (on) {
    setValve(SV_CHILL_IDX, true);
    setPumpPct(PUMP_CHILL_CH, pct);
  } else {
    setPumpPct(PUMP_CHILL_CH, 0.0f);
    setValve(SV_CHILL_IDX, false);
  }
}

static void allOff() {
  for (int i = 0; i < 5; i++) setPumpPct(i, 0);
  allValvesOff();
}

static void fault(const char* msg) {
  if (gFault) return;
  gFault = msg;
  gAbort = true;
  allOff();
  Serial.printf("[FAULT] %s\n", msg);
}

// ---------------------------------------------------------------------------
// Serial line reader (USB)
// ---------------------------------------------------------------------------
static bool getLine(String &out) {
  static String rx;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'x' || c == 'X' || c == 27 || c == 3) {
      gAbort = true;
      allOff();
      rx = "";
      Serial.println("\n[EMERGENCY STOP] 'x' received -> All pumps & valves OFF!");
      return false;
    }
    if (c == '\n' || c == '\r') {
      if (rx.length()) { out = rx; rx = ""; return true; }
    } else {
      rx += c;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Link to ESP32 #2
// ---------------------------------------------------------------------------
static void parseFrame(const char* s) {
  if (s[0] != '$') return;
  const char* star = strchr(s, '*');
  if (!star) return;
  uint8_t x = 0;
  for (const char* p = s + 1; p < star; p++) x ^= (uint8_t)*p;
  unsigned int rx = 0;
  if (sscanf(star + 1, "%2x", &rx) != 1 || rx != x) { badFrames++; return; }
  unsigned long seq, ms, c1, c2, c3, c4, c5, fsw;
  float t1 = -999.0f, t2 = -999.0f;
  int fields = sscanf(s + 1, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%f,%f",
                      &seq, &ms, &c1, &c2, &c3, &c4, &c5, &fsw, &t1, &t2);
  if (fields < 8) { badFrames++; return; }
  cnt[0] = (uint32_t)c1; cnt[1] = (uint32_t)c2; cnt[2] = (uint32_t)c3; cnt[3] = (uint32_t)c4; cnt[4] = (uint32_t)c5;
  gFsw = (uint32_t)fsw;
  if (fields >= 10) {
    gTemp1 = t1;
    gTemp2 = t2;
  }
  lastFrameMs = millis();
  linkSeen = true;
  goodFrames++;
}

static void readLink() {
  static char buf[192];
  static uint8_t n = 0;
  while (LINK.available()) {
    char c = (char)LINK.read();
    if (c == '\n') { buf[n] = 0; parseFrame(buf); n = 0; }
    else if (c == '\r') { }
    else if (n < sizeof(buf) - 1) buf[n++] = c;
    else n = 0;
  }
}

// ---------------------------------------------------------------------------
// Start switch, service loop, waiting
// ---------------------------------------------------------------------------
static void handleSwitch() {
  if (PIN_SW < 0) return;
  static bool stable = false, lastRaw = false;
  static uint32_t chg = 0;
  bool raw = (digitalRead(PIN_SW) == LOW);
  uint32_t now = millis();
  if (raw != lastRaw) { lastRaw = raw; chg = now; }
  if (now - chg >= 40 && raw != stable) {
    stable = raw;
    if (stable) {
      if (gBusy) gAbort = true;      // press during a run = stop
      else gStartReq = true;         // press when idle = start batch
    }
  }
}

static void service() {
  readLink();
  handleSwitch();
  if (gBusy) {
    String l;
    if (getLine(l)) {
      l.trim(); l.toLowerCase();
      if (l == "x" || l == "stop" || l == "abort") gAbort = true;
      else Serial.println("[BUSY] send 'x' to abort");
    }
    if (!gAbort) {
      if (!linkSeen || millis() - lastFrameMs > LINK_TIMEOUT_MS) fault("sensor link lost (ESP32 #2)");
      if (USE_MIX_HIGH_FLOAT && PIN_FSW[3] >= 0 && digitalRead(PIN_FSW[3]) == LOW) fault("mixing tank high level (FSW4)");
    }
  }
}

// Waits while servicing the link and abort inputs. Returns false if aborted or faulted.
static bool waitMs(uint32_t ms) {
  uint32_t t = millis();
  while (millis() - t < ms) {
    if (Serial.available()) {
      char c = (char)Serial.read();
      if (c == 'x' || c == 'X' || c == 27 || c == 3) {
        gAbort = true;
        allOff();
        Serial.println("\n[EMERGENCY STOP] 'x' received during dose -> Aborted!");
        return false;
      }
    }
    service();
    if (gAbort) return false;
    delay(1);
  }
  return !gAbort;
}

static bool rampPump(int ch, float from, float to, uint32_t ms) {
  uint32_t steps = ms / 20;
  if (steps < 1) steps = 1;
  for (uint32_t i = 1; i <= steps; i++) {
    setPumpPct(ch, from + (to - from) * (float)i / (float)steps);
    if (!waitMs(20)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Dose engine
// ---------------------------------------------------------------------------
static bool hwReady() {
  bool ok = true;
  for (int i = 0; i < NUM_LINES; i++) {
    if (!LINE_ENABLED[i]) continue; // Skip disabled lines (Flush)
    if (PIN_PWM[LINE_PUMP[i]] < 0) { Serial.printf("[PINS] PWM_%d not set in config.h\n", LINE_PUMP[i] + 1); ok = false; }
    if (PIN_SV[LINE_SV[i]] < 0)    { Serial.printf("[PINS] SV%d not set in config.h\n", LINE_SV[i] + 1); ok = false; }
  }
  if (!linkSeen || millis() - lastFrameMs > LINK_TIMEOUT_MS) { Serial.println("[LINK] no data from ESP32 #2"); ok = false; }
  return ok;
}

static bool beginBusy() {
  if (!hwReady()) return false;
  gBusy = true;
  gAbort = false;
  gFault = nullptr;
  return true;
}

static void endBusy(bool ok) {
  allOff();
  gBusy = false;
  if (!ok) Serial.println(gFault ? "[STOPPED] fault, all outputs off" : "[STOPPED] aborted by operator, all outputs off");
}

static bool doseLine(int line, float targetMl, bool toWaste, bool precise, DoseResult &r) {
  if (line == L_FLUSH || !LINE_ENABLED[line]) {
    Serial.printf("[ERROR] Line %s is disabled (flush tank is not used)\n", LINE_NAME[line]);
    return false;
  }
  const LineCfg &c = cfg[line];
  const int ft = LINE_FT[line], pump = LINE_PUMP[line], sv = LINE_SV[line];
  const int pathSv = toWaste ? SV_WASTE_IDX : SV_TANK_IDX;
  r = DoseResult();
  r.targetMl = targetMl;
  Serial.printf("[DOSE] %s -> %s: target %.1f mL (K=%.3f p/mL, fast %.0f%%, slow %.0f%%, lead %.1f mL)\n",
                LINE_NAME[line], toWaste ? "WASTE" : "TANK", targetMl, c.kPpm, c.dutyFast, c.dutySlow, precise ? c.leadMl : 0.0f);

  allValvesOff();
  setValve(sv, true);
  if (HAVE_WASTE_VALVE && toWaste) setValve(pathSv, true);
  if (!waitMs(SV_OPEN_LEAD_MS)) return false;

  const uint32_t base = cnt[ft];
  const uint32_t t0 = millis();
  auto delivered = [&]() -> float { return (float)(uint32_t)(cnt[ft] - base) / c.kPpm; };

  float slowZone = fmaxf(targetMl * c.slowFrac, SLOW_MIN_ML);
  if (slowZone > targetMl * 0.5f) slowZone = targetMl * 0.5f;
  const float switchMl = targetMl - slowZone;
  float stopMl = targetMl - (precise ? c.leadMl : 0.0f);
  if (stopMl < switchMl) stopMl = switchMl;

  uint32_t lastPrint = millis(), lastProgMs = millis();
  float lastProgMl = 0.0f, lastPrintMl = 0.0f;

  auto tick = [&](int phase) -> bool {
    if (!waitMs(5)) return false;
    const uint32_t now = millis();
    const float d = delivered();
    if (d >= lastProgMl + 0.3f) { lastProgMl = d; lastProgMs = now; }
    else if (now - lastProgMs > NOFLOW_FAULT_MS) { fault("no flow: tank empty, air lock, or pump/sensor not working"); return false; }
    if (now - t0 > DOSE_TIMEOUT_MS) { fault("dose timeout"); return false; }
    if (now - lastPrint >= 200) {
      const float flow = (d - lastPrintMl) / ((now - lastPrint) / 1000.0f) * 0.06f;   // mL/s -> L/min
      lastPrint = now;
      lastPrintMl = d;
      float dt_temp = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
      Serial.printf("/*%.2f,%d,%d,%.1f,%.1f,%.0f,%.2f,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                    now / 1000.0f, line, phase, targetMl, d, pumpPct[pump], flow,
                    (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                    gTemp1, gTemp2, dt_temp);
    }
    return true;
  };

  // Fast phase
  if (!rampPump(pump, 0.0f, c.dutyFast, START_RAMP_MS)) return false;
  while (delivered() < switchMl) { if (!tick(1)) return false; }

  // Slow phase
  setPumpPct(pump, c.dutySlow);
  uint32_t rT = millis();
  float rMl = delivered(), rate = 0.0f;       // rate in mL/s, used by top-up
  while (delivered() < stopMl) {
    if (!tick(2)) return false;
    const uint32_t now = millis();
    if (now - rT >= 250) {
      const float d = delivered();
      rate = (d - rMl) / ((now - rT) / 1000.0f);
      rT = now;
      rMl = d;
    }
  }

  // Stop and let the coast pulses arrive with the valve still open
  setPumpPct(pump, 0.0f);
  r.stopMl = delivered();
  if (!waitMs(SETTLE_MS)) return false;
  float d = delivered();

  // Top-up from below only
  if (precise) {
    for (int k = 0; k < TOPUP_MAX && rate > 0.5f; k++) {
      const float rem = targetMl - d;
      if (rem < TOPUP_TRIGGER_ML) break;
      uint32_t burst = (uint32_t)(TOPUP_AIM * rem / rate * 1000.0f);
      if (burst < TOPUP_MIN_BURST_MS) break;
      if (burst > TOPUP_MAX_BURST_MS) burst = TOPUP_MAX_BURST_MS;
      setPumpPct(pump, c.dutySlow);
      if (!waitMs(burst)) return false;
      setPumpPct(pump, 0.0f);
      if (!waitMs(TOPUP_SETTLE_MS)) return false;
      d = delivered();
      Serial.printf("[TOPUP] %d: burst %lu ms -> %.1f mL\n", k + 1, (unsigned long)burst, d);
    }
  }

  setValve(sv, false);
  if (HAVE_WASTE_VALVE && toWaste) setValve(pathSv, false);
  r.deliveredMl = d;
  r.coastMl = d - r.stopMl;
  r.pulses = (uint32_t)(cnt[ft] - base);
  r.secs = (millis() - t0) / 1000.0f;
  lastRunLine = line;
  lastRunPulses = r.pulses;
  Serial.printf("[DONE] %s: target %.1f mL, counted %.1f mL (%+.2f%%), pulses %lu, coast %.1f mL, %.1f s\n",
                LINE_NAME[line], targetMl, d, targetMl > 0 ? (d - targetMl) / targetMl * 100.0f : 0.0f,
                (unsigned long)r.pulses, r.coastMl, r.secs);
  return true;
}

// ---------------------------------------------------------------------------
// Parallel Multi-Line Closed-Loop Dosing Engine
// ---------------------------------------------------------------------------
struct ParallelState {
  int line;
  int pump;
  int sv;
  int ft;
  float targetMl;
  float switchMl;
  float stopMl;
  float deliveredMl;
  uint32_t base;
  uint32_t stopMs;
  uint32_t lastProgMs;
  float lastProgMl;
  int phase; // 1 = Fast, 2 = Slow, 3 = Coast/Settle, 4 = Done
  bool done;
  DoseResult res;
};

static bool doseParallel(const int lines[], const float targets[], int nLines, DoseResult results[]) {
  ParallelState st[NUM_LINES];
  int activeCount = 0;

  for (int i = 0; i < nLines; i++) {
    int l = lines[i];
    if (l == L_FLUSH || !LINE_ENABLED[l]) {
      Serial.printf("[PARALLEL] Skipping disabled line %s\n", LINE_NAME[l]);
      continue;
    }
    st[activeCount].line = l;
    st[activeCount].pump = LINE_PUMP[l];
    st[activeCount].sv   = LINE_SV[l];
    st[activeCount].ft   = LINE_FT[l];
    st[activeCount].targetMl = targets[i];
    const LineCfg &c = cfg[l];

    float slowZone = fmaxf(targets[i] * c.slowFrac, SLOW_MIN_ML);
    if (slowZone > targets[i] * 0.5f) slowZone = targets[i] * 0.5f;
    st[activeCount].switchMl = targets[i] - slowZone;
    st[activeCount].stopMl   = targets[i] - (c.leadMl > 0 ? c.leadMl : 0.0f);
    if (st[activeCount].stopMl < st[activeCount].switchMl) st[activeCount].stopMl = st[activeCount].switchMl;

    st[activeCount].deliveredMl = 0.0f;
    st[activeCount].phase = 1;
    st[activeCount].done = false;
    st[activeCount].res = DoseResult();
    st[activeCount].res.targetMl = targets[i];

    Serial.printf("[PARALLEL] %s -> Target: %.1f mL (fast=%.0f%%, slow=%.0f%%, lead=%.1f mL)\n",
                  LINE_NAME[l], targets[i], c.dutyFast, c.dutySlow, c.leadMl);
    activeCount++;
  }

  if (activeCount == 0) return true;

  // 1. Open all active solenoid valves together
  for (int i = 0; i < activeCount; i++) {
    setValve(st[i].sv, true);
  }
  if (!waitMs(SV_OPEN_LEAD_MS)) return false;

  // 2. Capture baseline pulse counters
  uint32_t t0 = millis();
  for (int i = 0; i < activeCount; i++) {
    st[i].base = cnt[st[i].ft];
    st[i].lastProgMs = t0;
    st[i].lastProgMl = 0.0f;
  }

  // 3. Ramp all active pumps up together
  uint32_t rampSteps = START_RAMP_MS / 20;
  if (rampSteps < 1) rampSteps = 1;
  for (uint32_t step = 1; step <= rampSteps; step++) {
    for (int i = 0; i < activeCount; i++) {
      float fastDuty = cfg[st[i].line].dutyFast;
      setPumpPct(st[i].pump, fastDuty * (float)step / (float)rampSteps);
    }
    if (!waitMs(20)) return false;
  }

  // 4. Parallel closed-loop dosing execution
  uint32_t lastPrint = millis(), lastStatus = millis();
  int running = activeCount;

  while (running > 0) {
    if (!waitMs(5)) return false;
    uint32_t now = millis();

    for (int i = 0; i < activeCount; i++) {
      if (st[i].done) continue;
      int l = st[i].line;
      const LineCfg &c = cfg[l];

      float d = (float)(uint32_t)(cnt[st[i].ft] - st[i].base) / c.kPpm;
      st[i].deliveredMl = d;

      // No-flow timeout check (while pump running)
      if (st[i].phase == 1 || st[i].phase == 2) {
        if (d >= st[i].lastProgMl + 0.3f) {
          st[i].lastProgMl = d;
          st[i].lastProgMs = now;
        } else if (now - st[i].lastProgMs > NOFLOW_FAULT_MS) {
          char msg[64];
          snprintf(msg, sizeof(msg), "no flow on %s: check tank, pump, or sensor", LINE_NAME[l]);
          fault(msg);
          return false;
        }
      }

      // Phase 1 -> Phase 2 (Fast -> Slow)
      if (st[i].phase == 1 && d >= st[i].switchMl) {
        st[i].phase = 2;
        setPumpPct(st[i].pump, c.dutySlow);
      }

      // Phase 2 -> Phase 3 (Pump OFF immediately when threshold is reached)
      if ((st[i].phase == 1 || st[i].phase == 2) && d >= st[i].stopMl) {
        setPumpPct(st[i].pump, 0.0f);
        st[i].res.stopMl = d;
        st[i].stopMs = now;
        st[i].phase = 3;
      }

      // Phase 3 -> Phase 4 (Settle elapsed -> Valve CLOSED, Line Done!)
      if (st[i].phase == 3 && (now - st[i].stopMs >= SETTLE_MS)) {
        setValve(st[i].sv, false);
        st[i].phase = 4;
        st[i].done = true;
        running--;
        st[i].res.deliveredMl = d;
        st[i].res.coastMl = d - st[i].res.stopMl;
        st[i].res.pulses = (uint32_t)(cnt[st[i].ft] - st[i].base);
        st[i].res.secs = (now - t0) / 1000.0f;
        Serial.printf("[THRESHOLD REACHED] %s: target %.1f mL -> delivered %.1f mL (Pump OFF, SV%d CLOSED)\n",
                      LINE_NAME[l], st[i].targetMl, d, st[i].sv + 1);
      }
    }

    // Live telemetry stream to Serial Studio
    if (now - lastPrint >= 200) {
      float dt = (now - lastPrint) / 1000.0f;
      lastPrint = now;
      float totDelivered = 0.0f;
      float maxDuty = 0.0f;
      for (int i = 0; i < activeCount; i++) {
        totDelivered += st[i].deliveredMl;
        if (!st[i].done && pumpPct[st[i].pump] > maxDuty) {
          maxDuty = pumpPct[st[i].pump];
        }
      }
      static float lastParallelDelivered = 0.0f;
      float flowLpm = (dt > 0.0f) ? (totDelivered - lastParallelDelivered) / dt * 0.06f : 0.0f;
      lastParallelDelivered = totDelivered;

      // Target and delivered volume representation:
      // When dosing 2 lines (e.g. Red & Blue 300 mL): gauge shows 300 mL target and per-line delivered volume reaching 300 mL
      // When dosing full 3-line batch: gauge shows 3000 mL batch total
      float targetDisp = (activeCount == 2) ? st[0].targetMl : BATCH_ML;
      float deliveredDisp = (activeCount == 2) ? fmaxf(st[0].deliveredMl, st[1].deliveredMl) : totDelivered;

      float dt_temp = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
      Serial.printf("/*%.2f,-2,1,%.1f,%.1f,%.0f,%.2f,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                    now / 1000.0f, targetDisp, deliveredDisp, maxDuty, flowLpm,
                    (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                    gTemp1, gTemp2, dt_temp);
    }

    // Live console status line once per second
    if (now - lastStatus >= 1000) {
      lastStatus = now;
      Serial.print("[PARALLEL]");
      for (int i = 0; i < activeCount; i++) {
        Serial.printf("  %s: %.1f/%.1f mL (%s)",
                      LINE_NAME[st[i].line], st[i].deliveredMl, st[i].targetMl,
                      st[i].done ? "OFF" : (st[i].phase == 1 ? "FAST" : (st[i].phase == 2 ? "SLOW" : "SETTLE")));
      }
      Serial.println();
    }
  }

  for (int i = 0; i < activeCount; i++) {
    results[i] = st[i].res;
  }
  return true;
}

static bool runBatch(bool twoStage = false) {
  if (!beginBusy()) return false;
  Serial.println("\n==============================================================");
  Serial.println("  [BATCH] Starting automated 3,000 mL Mustard Batch");
  Serial.println("  Targets: Yellow = 2,400 mL (80%), Blue = 300 mL (10%), Red = 300 mL (10%)");
  Serial.printf("  Mode: %s\n", twoStage ? "Two-Stage (Yellow first, then Red & Blue together)" : "All 3 Lines Parallel");
  Serial.println("  Flush Pump: DISABLED (Motor Driver 2 M2 offline)");
  Serial.println("==============================================================");

  bool ok = true;
  if (twoStage) {
    Serial.println("\n--- STAGE 1: Dosing Yellow (2,400.0 mL) ---");
    DoseResult resY;
    ok = doseLine(L_YELLOW, BATCH_ML * PCT[L_YELLOW] / 100.0f, false, true, resY);

    if (ok) {
      Serial.println("\n--- STAGE 2: Dosing Red & Blue Simultaneously at Equal PWM (300.0 mL each) ---");
      int lines[2] = {L_RED, L_BLUE};
      float targets[2] = {BATCH_ML * PCT[L_RED] / 100.0f, BATCH_ML * PCT[L_BLUE] / 100.0f};
      DoseResult resRB[2];
      ok = doseParallel(lines, targets, 2, resRB);

      if (ok) {
        Serial.println("\n================ [BATCH COMPLETE SUMMARY] ================");
        Serial.printf("  %-7s target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n", "YELLOW", resY.targetMl, resY.deliveredMl,
                      (resY.deliveredMl - resY.targetMl) / resY.targetMl * 100.0f);
        Serial.printf("  %-7s target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n", "RED", resRB[0].targetMl, resRB[0].deliveredMl,
                      (resRB[0].deliveredMl - resRB[0].targetMl) / resRB[0].targetMl * 100.0f);
        Serial.printf("  %-7s target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n", "BLUE", resRB[1].targetMl, resRB[1].deliveredMl,
                      (resRB[1].deliveredMl - resRB[1].targetMl) / resRB[1].targetMl * 100.0f);
        float tot = resY.deliveredMl + resRB[0].deliveredMl + resRB[1].deliveredMl;
        Serial.printf("  TOTAL   target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n", BATCH_ML, tot, (tot - BATCH_ML) / BATCH_ML * 100.0f);
        Serial.println("==========================================================");
        Serial.println("[SYSTEM] All pumps off, all solenoids closed. Process stopped automatically.\n");
      }
    }
  } else {
    int lines[3] = {L_RED, L_BLUE, L_YELLOW};
    float targets[3] = {
      BATCH_ML * PCT[L_RED] / 100.0f,
      BATCH_ML * PCT[L_BLUE] / 100.0f,
      BATCH_ML * PCT[L_YELLOW] / 100.0f
    };
    DoseResult res[3];
    ok = doseParallel(lines, targets, 3, res);

    if (ok) {
      Serial.println("\n================ [BATCH COMPLETE SUMMARY] ================");
      float tot = 0.0f;
      for (int i = 0; i < 3; i++) {
        int l = lines[i];
        Serial.printf("  %-7s target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n",
                      LINE_NAME[l], res[i].targetMl, res[i].deliveredMl,
                      (res[i].deliveredMl - res[i].targetMl) / res[i].targetMl * 100.0f);
        tot += res[i].deliveredMl;
      }
      Serial.printf("  TOTAL   target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n", BATCH_ML, tot, (tot - BATCH_ML) / BATCH_ML * 100.0f);
      Serial.println("==========================================================");
      Serial.println("[SYSTEM] All pumps off, all solenoids closed. Process stopped automatically.\n");
    }
  }
  endBusy(ok);
  return ok;
}

// ---------------------------------------------------------------------------
// Day 2 Sequence Steps (Step 1 Flush, Step 2 Thermal Interlock, Step 3 Recipe)
// ---------------------------------------------------------------------------

static bool runStep1FlushInternal(float targetMl) {
  Serial.println("\n==========================================================");
  Serial.printf(">>> DAY 2 - STEP 1: FLUSH BASE-COLOUR LINE (Target: %.1f mL) <<<\n", targetMl);
  Serial.println("  Line: Blue Line (Pump 03 via PWM_3 / GPIO 25)");
  Serial.println("  Valve: Flush Solenoid SV4 (GPIO 19)");
  Serial.println("  Flow Sensor: FT-03 (FS3)");
  Serial.println("  Discharge: 2.0 L Collection Jug");
  Serial.println("==========================================================");

  const int pump = LINE_PUMP[L_BLUE]; // Blue pump (Driver 2 Ch1 / PWM_3 / GPIO 25)
  const int sv   = LINE_SV[L_FLUSH];  // SV4 (GPIO 19)
  const int ft   = LINE_FT[L_BLUE];  // FT-03 / FS3
  const float k  = cfg[L_FLUSH].kPpm;

  allValvesOff();
  setValve(sv, true); // Open SV4
  bool ok = waitMs(SV_OPEN_LEAD_MS);
  if (!ok) { allOff(); return false; }

  const uint32_t basePulses = cnt[ft];
  const uint32_t targetPulses = (uint32_t)(targetMl * k + 0.5f);
  const float duty = 100.0f;

  ok = rampPump(pump, 0.0f, duty, START_RAMP_MS);

  uint32_t t0 = millis();
  uint32_t lastPrint = millis();
  uint32_t lastFlowCheck = millis();
  uint32_t lastFlowPulses = cnt[ft];

  while (ok && !gAbort) {
    if (!waitMs(5)) { ok = false; break; }
    uint32_t now = millis();
    uint32_t curP = cnt[ft] - basePulses;
    float curMl = (float)curP / k;

    // No-flow interlock (> 10 seconds of pump ON with zero pulses)
    if (now - lastFlowCheck >= 2000) {
      uint32_t dp = cnt[ft] - lastFlowPulses;
      lastFlowPulses = cnt[ft];
      lastFlowCheck = now;
      if (dp == 0 && (now - t0 > 10000)) {
        fault("Step 1 Flush Interlock: No flow detected for > 10 s (tank empty or line blocked)!");
        ok = false;
        break;
      }
    }

    // Telemetry stream
    if (now - lastPrint >= 200) {
      lastPrint = now;
      Serial.printf("/*%.2f,3,1,%.1f,%.1f,100,0.00,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                    now / 1000.0f, targetMl, curMl,
                    (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                    gTemp1, gTemp2, fabsf(gTemp2 - gTemp1));
    }

    // Check if target volume reached
    if (curP >= targetPulses) {
      Serial.printf("\n[STEP 1 COMPLETE] Target flush volume reached: %.1f mL (%lu pulses)!\n",
                    curMl, (unsigned long)curP);
      break;
    }
  }

  setPumpPct(pump, 0.0f);
  waitMs(SETTLE_MS);
  setValve(sv, false);

  if (ok && !gAbort) {
    float finalMl = (float)(cnt[ft] - basePulses) / k;
    Serial.printf("[STEP 1 SAFE STATE] Pump 03 OFF, SV4 CLOSED. Delivered: %.1f mL in %.1f s\n",
                  finalMl, (millis() - t0) / 1000.0f);
    return true;
  }
  allOff();
  return false;
}

static bool runStep2ThermalInterlockInternal(float targetDeltaT, uint32_t timeoutSec) {
  Serial.println("\n==========================================================");
  Serial.printf(">>> DAY 2 - STEP 2: THERMAL INTERLOCK (Target: Delta-T >= %.1f C) <<<\n", targetDeltaT);
  Serial.println("  Cooling Pump: Pump 4 (Driver 2 Ch2 / PWM_4 / GPIO 26)");
  Serial.println("  Cooling Valve: Solenoid SV5 (GPIO 18)");
  Serial.println("  Temp Sensor 1 (TS1 / GPIO 32): Red Tank (Cooled)");
  Serial.println("  Temp Sensor 2 (TS2 / GPIO 13): Yellow Tank (Reference)");
  Serial.printf("  Timeout: %lu seconds\n", (unsigned long)timeoutSec);
  Serial.println("==========================================================");

  allOff();
  setChilling(true, 100.0f); // Open SV5 and start Pump 4 at 100%

  uint32_t t0 = millis();
  uint32_t lastPrint = millis();
  uint32_t lastStatus = millis();
  bool ok = true;
  float currentDt = 0.0f;

  while (ok && !gAbort) {
    if (!waitMs(10)) { ok = false; break; }
    uint32_t now = millis();

    if (gTemp1 > -50.0f && gTemp2 > -50.0f) {
      currentDt = fabsf(gTemp2 - gTemp1);
    } else {
      currentDt = 0.0f;
    }

    // Live telemetry stream to Serial Studio
    if (now - lastPrint >= 200) {
      lastPrint = now;
      Serial.printf("/*%.2f,-4,2,%.1f,%.1f,100,0.00,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                    now / 1000.0f, targetDeltaT, currentDt,
                    (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                    gTemp1, gTemp2, currentDt);
    }

    // Live console status line once per second
    if (now - lastStatus >= 1000) {
      lastStatus = now;
      Serial.printf("[STEP 2 COOLING] TS1 (Red): %.2f C | TS2 (Yellow): %.2f C | Delta-T: %.2f C / %.1f C target | Time: %.1f s\n",
                    gTemp1, gTemp2, currentDt, targetDeltaT, (now - t0) / 1000.0f);
    }

    // Condition check
    if (currentDt >= targetDeltaT) {
      Serial.printf("\n[STEP 2 INTERLOCK MET] Delta-T = %.2f C >= %.1f C reached!\n", currentDt, targetDeltaT);
      break;
    }

    // Timeout check
    if (now - t0 >= (timeoutSec * 1000)) {
      fault("Step 2 Thermal Interlock: Timeout exceeded without reaching target Delta-T!");
      ok = false;
      break;
    }
  }

  setChilling(false); // Stop Pump 4, close SV5
  allOff();

  if (ok && !gAbort) {
    Serial.println("[STEP 2 SAFE STATE] Chilling Pump OFF, SV5 CLOSED. Handover to Step 3...");
    return true;
  }
  return false;
}

static bool runDay2Sequence() {
  if (!beginBusy()) return false;
  Serial.println("\n##############################################################");
  Serial.println("   LAM RESEARCH CHALLENGE 2026 - STAGE 3 - DAY 2 SEQUENCE    ");
  Serial.println("   FULL 3-STEP AUTONOMOUS PROCESS (ZERO OPERATOR HANDOVER)   ");
  Serial.println("##############################################################");

  uint32_t tStart = millis();

  // STEP 1: FLUSH
  bool ok = runStep1FlushInternal(2000.0f);
  if (!ok || gAbort) {
    Serial.println("\n[DAY 2 ABORTED] Step 1 Flush failed or stopped!");
    endBusy(false);
    return false;
  }

  Serial.println("\n>>> Step 1 Complete. Auto-pausing 2 seconds for fluid settle...");
  waitMs(2000);

  // STEP 2: THERMAL INTERLOCK
  ok = runStep2ThermalInterlockInternal(TARGET_DELTA_T, 300);
  if (!ok || gAbort) {
    Serial.println("\n[DAY 2 ABORTED] Step 2 Thermal Interlock failed or stopped!");
    endBusy(false);
    return false;
  }

  Serial.println("\n>>> Step 2 Complete. Auto-pausing 2 seconds before recipe dosing...");
  waitMs(2000);

  // STEP 3: MUSTARD RECIPE DOSING (3,000 mL)
  Serial.println("\n==========================================================");
  Serial.println(">>> DAY 2 - STEP 3: MUSTARD RECIPE PRECISION DOSING (3,000 mL) <<<");
  Serial.println("  Yellow = 2,400 mL (80%) | Red = 300 mL (10%) | Blue = 300 mL (10%)");
  Serial.println("==========================================================");

  int lines[3] = {L_YELLOW, L_RED, L_BLUE};
  float targets[3] = {
    BATCH_ML * PCT[L_YELLOW] / 100.0f,
    BATCH_ML * PCT[L_RED]    / 100.0f,
    BATCH_ML * PCT[L_BLUE]   / 100.0f
  };
  DoseResult res[3];
  ok = doseParallel(lines, targets, 3, res);

  allOff();
  uint32_t totalDuration = millis() - tStart;

  if (ok && !gAbort) {
    float tot = 0.0f;
    Serial.println("\n##############################################################");
    Serial.println("   DAY 2 AUTOMATED 3-STEP SEQUENCE SUCCESSFULLY COMPLETED!   ");
    Serial.println("##############################################################");
    Serial.println("  Step 1 Flush:      2,000.0 mL into Jug (Flow sensor FT-03 verified)");
    Serial.printf("  Step 2 Interlock:  Reached Delta-T = %.2f C (TS1=%.2f C, TS2=%.2f C)\n",
                  fabsf(gTemp2 - gTemp1), gTemp1, gTemp2);
    Serial.println("  Step 3 Recipe Dosing Summary:");
    for (int i = 0; i < 3; i++) {
      int l = lines[i];
      Serial.printf("    %-7s target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n",
                    LINE_NAME[l], res[i].targetMl, res[i].deliveredMl,
                    (res[i].deliveredMl - res[i].targetMl) / res[i].targetMl * 100.0f);
      tot += res[i].deliveredMl;
    }
    Serial.printf("    TOTAL   target %7.1f mL  delivered %7.1f mL  (%+.2f%%)\n",
                  BATCH_ML, tot, (tot - BATCH_ML) / BATCH_ML * 100.0f);
    Serial.printf("  Total Elapsed Time: %.1f seconds\n", totalDuration / 1000.0f);
    Serial.println("  Safe State: ALL PUMPS OFF (0% PWM), ALL VALVES CLOSED.");
    Serial.println("##############################################################\n");
  }

  endBusy(ok);
  return ok;
}

static bool runStep1Flush(float targetMl = 2000.0f) {
  if (!beginBusy()) return false;
  bool ok = runStep1FlushInternal(targetMl);
  endBusy(ok);
  return ok;
}

static bool runStep2ThermalInterlock(float targetDeltaT = 5.0f, uint32_t timeoutSec = 300) {
  if (!beginBusy()) return false;
  bool ok = runStep2ThermalInterlockInternal(targetDeltaT, timeoutSec);
  endBusy(ok);
  return ok;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
static void saveCfg() {
  prefs.begin("lam", false);
  char k[8];
  for (int i = 0; i < NUM_LINES; i++) {
    snprintf(k, sizeof(k), "k%d", i);  prefs.putFloat(k, cfg[i].kPpm);
    snprintf(k, sizeof(k), "df%d", i); prefs.putFloat(k, cfg[i].dutyFast);
    snprintf(k, sizeof(k), "ds%d", i); prefs.putFloat(k, cfg[i].dutySlow);
    snprintf(k, sizeof(k), "ld%d", i); prefs.putFloat(k, cfg[i].leadMl);
    snprintf(k, sizeof(k), "sf%d", i); prefs.putFloat(k, cfg[i].slowFrac);
  }
  prefs.putFloat("trunk", gTrunkMl);
  prefs.end();
  Serial.println("[CFG] saved to flash");
}

static void loadCfg() {
  for (int i = 0; i < NUM_LINES; i++) {
    cfg[i].kPpm     = DEFAULT_LINE_K[i];
    cfg[i].dutyFast = DEFAULT_LINE_FAST[i];
    cfg[i].dutySlow = DEFAULT_LINE_SLOW[i];
    cfg[i].leadMl   = DEFAULT_LINE_LEAD[i];
    cfg[i].slowFrac = DEFAULT_LINE_SLOWFRAC[i];
  }
  if (!prefs.begin("lam", true)) return;
  char k[8];
  for (int i = 0; i < NUM_LINES; i++) {
    snprintf(k, sizeof(k), "k%d", i);  cfg[i].kPpm     = prefs.getFloat(k, cfg[i].kPpm);
    snprintf(k, sizeof(k), "df%d", i); cfg[i].dutyFast = prefs.getFloat(k, cfg[i].dutyFast);
    snprintf(k, sizeof(k), "ds%d", i); cfg[i].dutySlow = prefs.getFloat(k, cfg[i].dutySlow);
    snprintf(k, sizeof(k), "ld%d", i); cfg[i].leadMl   = prefs.getFloat(k, cfg[i].leadMl);
    snprintf(k, sizeof(k), "sf%d", i); cfg[i].slowFrac = prefs.getFloat(k, cfg[i].slowFrac);
  }
  gTrunkMl = prefs.getFloat("trunk", gTrunkMl);
  prefs.end();
  bool needSave = false;
  for (int i = 0; i < NUM_LINES; i++) {
    if (cfg[i].dutyFast < 90.0f) { cfg[i].dutyFast = DEFAULT_LINE_FAST[i]; needSave = true; }
    if (cfg[i].dutySlow < 50.0f) { cfg[i].dutySlow = DEFAULT_LINE_SLOW[i]; needSave = true; }
    if (cfg[i].kPpm < 0.5f || cfg[i].kPpm > 3.0f) { cfg[i].kPpm = DEFAULT_LINE_K[i]; needSave = true; }
  }
  if (cfg[L_YELLOW].kPpm < 1.8f) { cfg[L_YELLOW].kPpm = DEFAULT_LINE_K[L_YELLOW]; needSave = true; }
  if (cfg[L_FLUSH].kPpm < 1.4f)  { cfg[L_FLUSH].kPpm  = DEFAULT_LINE_K[L_FLUSH];  needSave = true; }
  if (needSave) saveCfg();
}

// ---------------------------------------------------------------------------
// Calibration helpers (run through the real path, water first, then paint)
// ---------------------------------------------------------------------------
static void openPath(int line, bool toWaste) {
  allValvesOff();
  setValve(LINE_SV[line], true);
  setValve(toWaste ? SV_WASTE_IDX : SV_TANK_IDX, true);
}

// run <line> <duty> <sec> [waste]: constant duty for a time, reports pulses for 'meas'
static void cmdRun(int line, float duty, float sec, bool waste) {
  if (!beginBusy()) return;
  const int ft = LINE_FT[line], pump = LINE_PUMP[line];
  openPath(line, waste);
  bool ok = waitMs(SV_OPEN_LEAD_MS);
  const uint32_t base = cnt[ft];
  const uint32_t t0 = millis();
  if (ok) ok = rampPump(pump, 0.0f, duty, START_RAMP_MS);
  if (ok) {
    uint32_t runStart = millis();
    uint32_t durationMs = (uint32_t)(sec * 1000.0f);
    uint32_t lastTelem = millis();
    float lastRunMl = 0.0f;
    while (millis() - runStart < durationMs) {
      if (!waitMs(5)) { ok = false; break; }
      uint32_t now = millis();
      if (now - lastTelem >= 200) {
        float dt = (now - lastTelem) / 1000.0f;
        lastTelem = now;
        float curMl = (float)(uint32_t)(cnt[ft] - base) / cfg[line].kPpm;
        float flowLpm = (dt > 0.0f) ? (curMl - lastRunMl) / dt * 0.06f : 0.0f;
        lastRunMl = curMl;
        float dt_temp = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
        Serial.printf("/*%.2f,%d,1,%.1f,%.1f,%.0f,%.2f,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                      now / 1000.0f, line, sec * 10.0f, curMl, duty, flowLpm,
                      (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                      gTemp1, gTemp2, dt_temp);
      }
    }
  }
  setPumpPct(pump, 0.0f);
  if (ok) ok = waitMs(SETTLE_MS);
  if (ok) {
    const uint32_t p = cnt[ft] - base;
    const float s = (millis() - t0) / 1000.0f;
    lastRunLine = line;
    lastRunPulses = p;
    Serial.printf("[RUN] %s duty %.0f%%: pulses %lu in %.1f s = %.1f Hz, %.1f mL at K=%.3f\n", LINE_NAME[line], duty, (unsigned long)p, s,
                  p / s, p / cfg[line].kPpm, cfg[line].kPpm);
    Serial.println("[RUN] collect the liquid, weigh/measure it, then: meas <line> <actual_mL>");
  }
  endBusy(ok);
}

static void cmdSweep(int line, bool waste) {
  if (!beginBusy()) return;
  const int ft = LINE_FT[line], pump = LINE_PUMP[line];
  openPath(line, waste);
  bool ok = waitMs(SV_OPEN_LEAD_MS);
  Serial.println("[SWEEP] duty_pct, freq_Hz, mL_per_s, Q_Lmin   (K from current config)");
  for (int duty = 20; ok && duty <= 100; duty += 10) {
    setPumpPct(pump, (float)duty);
    ok = waitMs(3000);
    if (!ok) break;
    const uint32_t p0 = cnt[ft], t0 = millis();
    ok = waitMs(4000);
    if (!ok) break;
    const float dt = (millis() - t0) / 1000.0f;
    const float f = (float)(uint32_t)(cnt[ft] - p0) / dt;
    const float mlps = f / cfg[line].kPpm;
    Serial.printf("[SWEEP] %d, %.1f, %.2f, %.3f\n", duty, f, mlps, mlps * 0.06f);
  }
  endBusy(ok);
}

static void cmdDeadband(int line, bool waste) {
  if (!beginBusy()) return;
  const int ft = LINE_FT[line], pump = LINE_PUMP[line];
  openPath(line, waste);
  bool ok = waitMs(SV_OPEN_LEAD_MS);
  int startDuty = -1, stopDuty = -1;
  for (int d = 5; ok && d <= 100; d++) {
    setPumpPct(pump, (float)d);
    const uint32_t p0 = cnt[ft];
    ok = waitMs(500);
    if (ok && (uint32_t)(cnt[ft] - p0) >= 3) { startDuty = d; break; }
  }
  if (ok && startDuty > 0) {
    for (int d = startDuty; ok && d >= 1; d--) {
      setPumpPct(pump, (float)d);
      const uint32_t p0 = cnt[ft];
      ok = waitMs(500);
      if (ok && (uint32_t)(cnt[ft] - p0) < 2) { stopDuty = d; break; }
    }
    Serial.printf("[DEADBAND] %s: starts at %d%%, stops below %d%%. Keep slow duty well above %d%%.\n", LINE_NAME[line], startDuty, stopDuty, startDuty);
  } else if (ok) {
    Serial.println("[DEADBAND] no flow detected up to 100% - check priming, valve, sensor");
  }
  endBusy(ok);
}

// coast <line> <duty> [waste]: steady flow, then stop; reports the overrun = lead to use
static void cmdCoast(int line, float duty, bool waste) {
  if (!beginBusy()) return;
  const int ft = LINE_FT[line], pump = LINE_PUMP[line];
  openPath(line, waste);
  bool ok = waitMs(SV_OPEN_LEAD_MS);
  if (ok) ok = rampPump(pump, 0.0f, duty, START_RAMP_MS);
  if (ok) ok = waitMs(4000);
  const uint32_t pa = cnt[ft];
  if (ok) ok = waitMs(2000);
  const uint32_t pb = cnt[ft];
  setPumpPct(pump, 0.0f);
  if (ok) ok = waitMs(3000);
  if (ok) {
    const uint32_t pc = cnt[ft];
    const float hz = (float)(pb - pa) / 2.0f;
    const float coastMl = (float)(pc - pb) / cfg[line].kPpm;
    Serial.printf("[COAST] %s at %.0f%%: steady %.1f Hz (%.3f L/min), overrun after stop %.2f mL\n", LINE_NAME[line], duty, hz,
                  hz / cfg[line].kPpm * 0.06f, coastMl);
    Serial.printf("[COAST] suggested: lead %s %.1f   (use the value measured at the SLOW duty)\n", LINE_NAME[line], coastMl);
  }
  endBusy(ok);
}

// ---------------------------------------------------------------------------
// Command interpreter
// ---------------------------------------------------------------------------
static int splitArgs(const String &s, String *a, int maxN) {
  int n = 0, i = 0;
  const int L = s.length();
  while (i < L && n < maxN) {
    while (i < L && s[i] == ' ') i++;
    if (i >= L) break;
    int j = i;
    while (j < L && s[j] != ' ') j++;
    a[n++] = s.substring(i, j);
    i = j;
  }
  return n;
}

static int parseLine(const String &s) {
  if (s == "r" || s == "red" || s == "0") return L_RED;
  if (s == "b" || s == "blue" || s == "1") return L_BLUE;
  if (s == "y" || s == "yellow" || s == "2") return L_YELLOW;
  if (s == "f" || s == "flush" || s == "3") return L_FLUSH;
  return -1;
}

static void printHelp() {
  Serial.println("---- LAM Paint Mixing Controller Commands ----");
  Serial.println(" day2                      START FULL DAY 2 SEQUENCE (Flush -> Thermal -> Mustard Recipe)");
  Serial.println(" step1 [mL]                run Step 1: Flush Blue line (default 2000 mL into jug)");
  Serial.println(" step2 [delta]             run Step 2: Thermal Interlock (default Delta-T >= 10.0 C)");
  Serial.println(" cool [on|off|duty%]       control Chilling Pump 4 (Driver 2 M2) & Solenoid SV5");
  Serial.println(" temp                      display live TS1, TS2, and Delta-T temperatures");
  Serial.println(" batch / start             run the full Mustard batch (All 3 lines parallel)");
  Serial.println(" start 2step               run Mustard batch (Yellow first, then Red & Blue)");
  Serial.println(" dose rb <mL>              dose Red and Blue concurrently at equal PWM (e.g. dose rb 300)");
  Serial.println(" dose <line> <mL>          dose specified line to mixing tank (e.g. dose y 2400)");
  Serial.println(" x / stop                  emergency stop everything immediately");
  Serial.println(" status                    show link, pulse counts, temperatures, and line config");
  Serial.println(" valve <1..6> <0|1>        manually toggle a solenoid valve");
  Serial.println(" pump <1..5> <duty%>       manually test pump duty");
  Serial.println(" run <line> <duty> <sec>   constant-duty run, reports pulses");
  Serial.println(" meas <line> <actual_mL>   trim K-factor from weighed volume");
  Serial.println(" sweep <line>              duty vs flow table, 20..100 %");
  Serial.println(" deadband <line>           minimum duty where the pump starts / stops");
  Serial.println(" coast <line> <duty>       measure coast pulses and recommended lead");
  Serial.println(" k|lead|slowfrac <line>    adjust tuning constants");
  Serial.println(" default                   restore TEAM_DISTRO empirical calibration");
  Serial.println(" save | load               store / restore settings in flash");
}

static void handleCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  String a[6];
  const int n = splitArgs(cmd, a, 6);
  if (n == 0) return;
  const String &c = a[0];

  if (c == "help" || c == "?") { printHelp(); return; }
  if (c == "x" || c == "stop") { allOff(); Serial.println("[STOP] all outputs off"); return; }

  if (c == "day2" || (c == "start" && n >= 2 && a[1] == "day2")) {
    runDay2Sequence();
    return;
  }

  if (c == "step1") {
    float ml = (n >= 2 && a[1].toFloat() > 0) ? a[1].toFloat() : 2000.0f;
    runStep1Flush(ml);
    return;
  }

  if (c == "step2") {
    float dt = (n >= 2 && a[1].toFloat() > 0) ? a[1].toFloat() : TARGET_DELTA_T;
    runStep2ThermalInterlock(dt, 300);
    return;
  }

  if (c == "cool") {
    if (n >= 2 && a[1] == "off") {
      setChilling(false);
      Serial.println("[COOL] Chilling Pump 4 & SV5 OFF");
    } else {
      float pct = 100.0f;
      if (n >= 2 && a[1] != "on") pct = a[1].toFloat();
      if (n >= 3) pct = a[2].toFloat();
      setChilling(true, pct);
      Serial.printf("[COOL] Chilling Pump 4 ON (%.1f%% duty / PWM_4) | SV5 OPEN (GPIO 18)\n", pct);
    }
    return;
  }

  if (c == "temp") {
    float dt = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
    Serial.println("\n[TEMPERATURE TELEMETRY]");
    Serial.printf("  TS1 (GPIO 32 - Red Tank Cooled):   %.2f °C\n", gTemp1);
    Serial.printf("  TS2 (GPIO 13 - Yellow Tank Ref):   %.2f °C\n", gTemp2);
    Serial.printf("  Delta-T (|TS2 - TS1|):             %.2f °C  %s\n\n",
                  dt, (dt >= 10.0f) ? "[OK: >= 10.0 C TARGET REACHED]" : "[BELOW 10.0 C TARGET]");
    return;
  }

  if (c == "status") {
    float dt = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
    Serial.printf("[STATUS] link=%s frames=%lu bad=%lu fsw=0x%lX\n", linkSeen ? "OK" : "NONE",
                  (unsigned long)goodFrames, (unsigned long)badFrames, (unsigned long)gFsw);
    Serial.printf("  Temperatures: TS1(Red Cooled)=%.2f C  TS2(Yellow Ref)=%.2f C  Delta-T=%.2f C\n",
                  gTemp1, gTemp2, dt);
    Serial.printf("  Chilling:     Pump 4 (PWM_4 / GPIO 26) = %.0f%% | SV5 (GPIO 18) = %s\n",
                  pumpPct[PUMP_CHILL_CH], valveOn[SV_CHILL_IDX] ? "OPEN" : "CLOSED");
    for (int i = 0; i < NUM_LINES; i++) {
      Serial.printf("  %-6s K=%.4f p/mL  fast=%.0f%% slow=%.0f%% lead=%.1f mL slowfrac=%.2f  Pump:PWM_%d SV:SV%d FT%d count=%lu  trials=%d\n", LINE_NAME[i], cfg[i].kPpm,
                    cfg[i].dutyFast, cfg[i].dutySlow, cfg[i].leadMl, cfg[i].slowFrac, LINE_PUMP[i] + 1, LINE_SV[i] + 1, LINE_FT[i] + 1, (unsigned long)cnt[LINE_FT[i]], calCount[i]);
    }
    return;
  }

  if (c == "valve" && n >= 3) {
    const int v = a[1].toInt();
    if (v < 1 || v > 6) { Serial.println("valve 1..6"); return; }
    setValve(v - 1, a[2].toInt() != 0);
    Serial.printf("[MANUAL] SV%d %s (send 'x' to turn everything off)\n", v, valveOn[v - 1] ? "ON" : "OFF");
    return;
  }
  if (c == "pump" && n >= 3) {
    const int p = a[1].toInt();
    if (p < 1 || p > 5) { Serial.println("pump 1..5"); return; }
    setPumpPct(p - 1, a[2].toFloat());
    Serial.printf("[MANUAL] PWM_%d = %.0f%% (dry running is brief only; 'x' to stop)\n", p, pumpPct[p - 1]);
    return;
  }

  if (c == "batch" || c == "start") {
    const bool twoStage = (n >= 2 && (a[1] == "2step" || a[1] == "seq"));
    runBatch(twoStage);
    return;
  }

  if (c == "dose" && n >= 3 && (a[1] == "rb" || a[1] == "br")) {
    const float ml = a[2].toFloat();
    if (ml <= 0) { Serial.println("dose rb <mL>"); return; }
    if (!beginBusy()) return;
    int lines[2] = {L_RED, L_BLUE};
    float targets[2] = {ml, ml};
    DoseResult res[2];
    const bool ok = doseParallel(lines, targets, 2, res);
    endBusy(ok);
    return;
  }
  if (c == "flush") {
    const float ml = (n >= 2 && a[1].toFloat() > 0) ? a[1].toFloat() : FLUSH_ML;
    if (!beginBusy()) return;
    const bool ok = runFlush(ml);
    endBusy(ok);
    return;
  }

  if (c == "trunk" && n >= 2) { gTrunkMl = a[1].toFloat(); Serial.printf("[CFG] trunk = %.1f mL\n", gTrunkMl); return; }
  if (c == "default" || c == "resetcal") {
    for (int i = 0; i < NUM_LINES; i++) {
      cfg[i].kPpm     = DEFAULT_LINE_K[i];
      cfg[i].dutyFast = DEFAULT_LINE_FAST[i];
      cfg[i].dutySlow = DEFAULT_LINE_SLOW[i];
      cfg[i].leadMl   = DEFAULT_LINE_LEAD[i];
      cfg[i].slowFrac = DEFAULT_LINE_SLOWFRAC[i];
    }
    saveCfg();
    Serial.println("[CAL] Reset to TEAM_DISTRO empirical calibration and saved to flash!");
    return;
  }
  if (c == "save") { saveCfg(); return; }
  if (c == "load") { loadCfg(); Serial.println("[CFG] loaded"); return; }

  // everything below needs a line argument
  const int line = (n >= 2) ? parseLine(a[1]) : -1;
  if (line < 0) { Serial.println("unknown command or bad line, type 'help'"); return; }
  const bool waste = (n >= 3 && a[n - 1] == "waste");

  if (c == "dose" && n >= 3) {
    const float ml = a[2].toFloat();
    if (ml <= 0) { Serial.println("dose <line> <mL>"); return; }
    if (!beginBusy()) return;
    DoseResult r;
    const bool ok = doseLine(line, ml, false, true, r);
    endBusy(ok);
    if (ok) Serial.println("[DOSE] weigh/measure the tank contents, then: meas <line> <actual_mL>");
    return;
  }
  if (c == "run" && n >= 4) { cmdRun(line, a[2].toFloat(), a[3].toFloat(), waste); return; }
  if (c == "sweep") { cmdSweep(line, waste); return; }
  if (c == "deadband") { cmdDeadband(line, waste); return; }
  if (c == "coast" && n >= 3) { cmdCoast(line, a[2].toFloat(), waste); return; }

  if (c == "meas" && n >= 3) {
    const float actual = a[2].toFloat();
    if (actual <= 0 || lastRunLine != line || lastRunPulses == 0) {
      Serial.println("[CAL] needs a fresh 'dose' or 'run' on that line first, and a volume > 0");
      return;
    }
    const float kTrial = (float)lastRunPulses / actual;
    calN[line] += lastRunPulses;
    calV[line] += actual;
    calCount[line]++;
    const float old = cfg[line].kPpm;
    cfg[line].kPpm = (float)(calN[line] / calV[line]);
    lastRunPulses = 0;
    Serial.printf("[CAL] %s trial %d: %.3f p/mL | pooled K %.4f (was %.4f) | type 'save' to keep\n", LINE_NAME[line], calCount[line], kTrial,
                  cfg[line].kPpm, old);
    return;
  }
  if (c == "clearcal") { calN[line] = 0; calV[line] = 0; calCount[line] = 0; Serial.println("[CAL] cleared"); return; }

  if (c == "k" && n >= 3)         { cfg[line].kPpm = a[2].toFloat(); }
  else if (c == "lead" && n >= 3) { cfg[line].leadMl = a[2].toFloat(); }
  else if (c == "slowfrac" && n >= 3) { cfg[line].slowFrac = constrain(a[2].toFloat(), 0.02f, 0.5f); }
  else if (c == "duty" && n >= 4) { cfg[line].dutyFast = a[2].toFloat(); cfg[line].dutySlow = a[3].toFloat(); }
  else { Serial.println("unknown command or missing value, type 'help'"); return; }
  Serial.printf("[CFG] %s: K=%.4f fast=%.0f slow=%.0f lead=%.1f slowfrac=%.2f (not saved yet)\n", LINE_NAME[line], cfg[line].kPpm,
                cfg[line].dutyFast, cfg[line].dutySlow, cfg[line].leadMl, cfg[line].slowFrac);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  LINK.setRxBufferSize(1024);
  LINK.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);

  for (int i = 0; i < 6; i++) {
    if (PIN_SV[i] >= 0) { pinMode(PIN_SV[i], OUTPUT); digitalWrite(PIN_SV[i], LOW); }
  }
  for (int i = 0; i < 5; i++) {
    if (PIN_PWM[i] >= 0) { ledcAttach(PIN_PWM[i], PWM_HZ, 8); ledcWrite(PIN_PWM[i], 0); }
  }
  if (PIN_SW >= 0) pinMode(PIN_SW, INPUT);
  for (int i = 0; i < 4; i++) {
    if (PIN_FSW[i] >= 0) pinMode(PIN_FSW[i], INPUT);
  }

  loadCfg();
  Serial.println("\n==============================================================");
  Serial.println("  LAM RESEARCH CHALLENGE 2026 - STAGE 3 - DAY 1: MUSTARD MIX");
  Serial.println("  ESP32 #1 controller node.  Type 'help'.");
  Serial.println("==============================================================");
  Serial.println("[READY] Press the start switch or type 'batch'. Press again or type 'x' to stop.");
}

void loop() {
  service();
  static uint32_t lastIdleTelem = 0;
  if (!gBusy && millis() - lastIdleTelem >= 200) {
    lastIdleTelem = millis();
    float maxActiveDuty = 0.0f;
    int activeLine = -1;
    for (int i = 0; i < NUM_LINES; i++) {
      int p = LINE_PUMP[i];
      if (pumpPct[p] > maxActiveDuty) {
        maxActiveDuty = pumpPct[p];
        activeLine = i;
      }
    }
    float dt_temp = (gTemp1 > -50.0f && gTemp2 > -50.0f) ? fabsf(gTemp2 - gTemp1) : 0.0f;
    Serial.printf("/*%.2f,%d,0,0.0,0.0,%.0f,0.00,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f*/\n",
                  millis() / 1000.0f, activeLine, maxActiveDuty,
                  (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2], (unsigned long)cnt[3],
                  gTemp1, gTemp2, dt_temp);
  }
  if (gStartReq && !gBusy) {
    gStartReq = false;
    handleCommand("batch");
  }
  String l;
  if (!gBusy && getLine(l)) handleCommand(l);
  delay(1);
}
