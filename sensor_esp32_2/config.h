// ESP32 #2 (U7) "sensor node" - pin map and link settings.
// LAM Research Challenge 2026 - Stage 3 - Day 1 (Industrial Paint Mixing)
#pragma once

// ---- PIN MAP: POPULATED FROM SCHEMATIC SHEET 1 ----
// Flow sensors FS1..FS5 (open-collector pulse outputs, 3V3 pull-up on the PCB)
// Note: From Sheet 1, GPIO 14 is FS4 and GPIO 27 is FS5
#define PIN_FS1  33
#define PIN_FS2  25
#define PIN_FS3  26
#define PIN_FS4  14
#define PIN_FS5  27

// Float switches FSW5, FSW6 (external 10k pull-up, closes to GND = active LOW)
#define PIN_FSW5 34
#define PIN_FSW6 35

// ---- Additional PCB hardware from Schematic Sheet 1 (for reference / future expansion) ----
#define PIN_TS1   32   // DS18B20 Temperature Sensor 1 (4.7k pull-up on PCB)
#define PIN_TS2   13   // DS18B20 Temperature Sensor 2 (4.7k pull-up on PCB)
#define PIN_SDA1  21   // Colour Sensor 1 I2C SDA
#define PIN_SCL1  22   // Colour Sensor 1 I2C SCL
#define PIN_SDA2  19   // Colour Sensor 2 I2C SDA
#define PIN_SCL2  18   // Colour Sensor 2 I2C SCL
#define PIN_H1    23   // Heater connector H1

// ---- UART link to ESP32 #1 (labelled TX/RX on the schematic) ----
// Default Serial2 pins on NodeMCU-32S are RX2 = GPIO16, TX2 = GPIO17.
// If ESP32 #1 shows "no data", the TX/RX pair is swapped: swap these two numbers.
#define LINK_RX        16
#define LINK_TX        17
#define LINK_BAUD      115200
#define LINK_RX_PIN    16
#define LINK_TX_PIN    17
#define LINK_BAUD_RATE 115200

// ---- Timing ----
#define TX_PERIOD_MS     20      // 50 frames per second
#define PCNT_GLITCH_NS   5000    // ignore pulses shorter than 5 us
