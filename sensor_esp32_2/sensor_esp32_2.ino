// LAM Research Challenge 2026 - Stage 3 - Day 1 (Industrial Paint Mixing)
// ESP32 #2 (U7) = SENSOR NODE
//   Counts flow-sensor pulses FS1..FS5 with the hardware pulse counter (PCNT) and streams the
//   running totals to ESP32 #1 over UART at 50 Hz. It makes no process decisions.
//
// Frame:  $seq,ms,c1,c2,c3,c4,c5,fsw*HH\n
//   c1..c5 = cumulative pulse totals (32-bit, wraps are handled by the receiver)
//   fsw    = bit0: FSW5 active, bit1: FSW6 active
//   HH     = XOR checksum of the characters between '$' and '*', two hex digits
//
// Board: ESP32 Dev Module, Arduino-ESP32 core 3.x.

#include <Arduino.h>
#include "driver/pulse_cnt.h"
#include "config.h"

static const int FS_PIN[5] = {PIN_FS1, PIN_FS2, PIN_FS3, PIN_FS4, PIN_FS5};

static pcnt_unit_handle_t unitH[5];
static bool     unitOk[5]   = {false, false, false, false, false};
static int      lastRaw[5]  = {0, 0, 0, 0, 0};
static uint32_t total[5]    = {0, 0, 0, 0, 0};
static uint32_t seqNo       = 0;

static void initCounter(int i) {
  if (FS_PIN[i] < 0) {
    Serial.printf("[PCNT] FS%d pin not set in config.h - skipped\n", i + 1);
    return;
  }
  pinMode(FS_PIN[i], INPUT_PULLUP);

  pcnt_unit_config_t uc = {};
  uc.low_limit  = -32768;
  uc.high_limit = 32767;
  if (pcnt_new_unit(&uc, &unitH[i]) != ESP_OK) { Serial.printf("[PCNT] FS%d unit failed\n", i + 1); return; }

  pcnt_chan_config_t cc = {};
  cc.edge_gpio_num  = FS_PIN[i];
  cc.level_gpio_num = -1;
  pcnt_channel_handle_t ch;
  if (pcnt_new_channel(unitH[i], &cc, &ch) != ESP_OK) { Serial.printf("[PCNT] FS%d channel failed\n", i + 1); return; }

  pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
  pcnt_channel_set_level_action(ch, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
  gpio_set_pull_mode((gpio_num_t)FS_PIN[i], GPIO_PULLUP_ONLY);

  pcnt_glitch_filter_config_t fc = {};
  fc.max_glitch_ns = PCNT_GLITCH_NS;
  pcnt_unit_set_glitch_filter(unitH[i], &fc);

  pcnt_unit_enable(unitH[i]);
  pcnt_unit_clear_count(unitH[i]);
  pcnt_unit_start(unitH[i]);
  unitOk[i] = true;
  Serial.printf("[PCNT] FS%d counting on GPIO %d\n", i + 1, FS_PIN[i]);
}

// Reads every counter, adds the new pulses to the 32-bit totals, and clears a counter once it
// passes 16000 so the 16-bit hardware range is never exceeded. Called every loop (~1 ms).
static void pollCounters() {
  for (int i = 0; i < 5; i++) {
    if (!unitOk[i]) continue;
    int c = 0;
    pcnt_unit_get_count(unitH[i], &c);
    int d = c - lastRaw[i];
    if (d < 0) d += 32768;
    total[i] += (uint32_t)d;
    if (c > 16000) {
      pcnt_unit_clear_count(unitH[i]);
      lastRaw[i] = 0;
    } else {
      lastRaw[i] = c;
    }
  }
}

static float gTemp1 = -999.0f; // TS1: Red Tank (°C)
static float gTemp2 = -999.0f; // TS2: Yellow Tank (°C)

// ---------------------------------------------------------------------------
// DS18B20 1-Wire Implementation (Dedicated Pins TS1=GPIO 32, TS2=GPIO 13)
// ---------------------------------------------------------------------------
static void dsPinInit(int pin) {
  if (pin < 0) return;
  pinMode(pin, INPUT_PULLUP);
}

static bool dsReset(int pin) {
  if (pin < 0) return false;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(480);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(70);
  int presence = digitalRead(pin);
  delayMicroseconds(410);
  return (presence == LOW);
}

static void dsWriteBit(int pin, int bit) {
  noInterrupts();
  if (bit) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delayMicroseconds(6);
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(64);
  } else {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delayMicroseconds(60);
    pinMode(pin, INPUT_PULLUP);
    delayMicroseconds(10);
  }
  interrupts();
}

static int dsReadBit(int pin) {
  noInterrupts();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(3);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(10);
  int b = digitalRead(pin);
  delayMicroseconds(55);
  interrupts();
  return b;
}

static void dsWriteByte(int pin, uint8_t byte) {
  for (int i = 0; i < 8; i++) {
    dsWriteBit(pin, (byte >> i) & 1);
  }
}

static uint8_t dsReadByte(int pin) {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    if (dsReadBit(pin)) b |= (1 << i);
  }
  return b;
}

static bool dsStartConvert(int pin) {
  if (!dsReset(pin)) return false;
  dsWriteByte(pin, 0xCC); // SKIP ROM
  dsWriteByte(pin, 0x44); // CONVERT T
  return true;
}

static float dsReadTemp(int pin) {
  if (!dsReset(pin)) return -999.0f;
  dsWriteByte(pin, 0xCC); // SKIP ROM
  dsWriteByte(pin, 0xBE); // READ SCRATCHPAD
  uint8_t lsb = dsReadByte(pin);
  uint8_t msb = dsReadByte(pin);
  for (int i = 0; i < 7; i++) dsReadByte(pin);
  int16_t raw = (int16_t)((msb << 8) | lsb);
  if (raw == -1 || raw == 0x0550) return -999.0f;
  float temp = (float)raw * 0.0625f;
  if (temp < -55.0f || temp > 125.0f) return -999.0f;
  return temp;
}

static uint32_t readFloats() {
  uint32_t v = 0;
  if (PIN_FSW5 >= 0 && digitalRead(PIN_FSW5) == LOW) v |= 1;
  if (PIN_FSW6 >= 0 && digitalRead(PIN_FSW6) == LOW) v |= 2;
  return v;
}

static void sendFrame() {
  char body[128];
  int n = snprintf(body, sizeof(body), "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f",
                   (unsigned long)seqNo++, (unsigned long)millis(),
                   (unsigned long)total[0], (unsigned long)total[1], (unsigned long)total[2],
                   (unsigned long)total[3], (unsigned long)total[4], (unsigned long)readFloats(),
                   gTemp1, gTemp2);
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= (uint8_t)body[i];
  char out[160];
  int m = snprintf(out, sizeof(out), "$%s*%02X\n", body, x);
  Serial2.write((const uint8_t*)out, m);
}

void setup() {
  Serial.begin(115200);
  Serial2.setTxBufferSize(512);
  Serial2.begin(LINK_BAUD_RATE, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  if (PIN_FSW5 >= 0) pinMode(PIN_FSW5, INPUT);
  if (PIN_FSW6 >= 0) pinMode(PIN_FSW6, INPUT);
  dsPinInit(PIN_TS1);
  dsPinInit(PIN_TS2);
  Serial.println("\n[SENSOR NODE] ESP32 #2 - flow pulse & DS18B20 streamer");
  for (int i = 0; i < 5; i++) initCounter(i);
}

void loop() {
  static uint32_t lastTx = 0, lastDbg = 0, lastTempSample = 0;
  static bool converting = false;
  pollCounters();
  uint32_t now = millis();

  // Non-blocking DS18B20 state machine (1 Hz)
  if (!converting && (now - lastTempSample >= 1000)) {
    dsStartConvert(PIN_TS1);
    dsStartConvert(PIN_TS2);
    converting = true;
    lastTempSample = now;
  } else if (converting && (now - lastTempSample >= 750)) {
    gTemp1 = dsReadTemp(PIN_TS1);
    gTemp2 = dsReadTemp(PIN_TS2);
    converting = false;
  }

  if (now - lastTx >= TX_PERIOD_MS) {
    lastTx = now;
    sendFrame();
  }
  if (now - lastDbg >= 1000) {
    lastDbg = now;
    Serial.printf("[SENSOR NODE] totals FS1..5 = %lu %lu %lu %lu %lu | TS1: %.2f C | TS2: %.2f C\n",
                  (unsigned long)total[0], (unsigned long)total[1], (unsigned long)total[2],
                  (unsigned long)total[3], (unsigned long)total[4], gTemp1, gTemp2);
  }
  delay(1);
}
