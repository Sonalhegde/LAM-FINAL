// LAM Research Challenge 2026 - Stage 3 - Day 1 (Industrial Paint Mixing)
// ESP32 #2 (U7) = SENSOR NODE (Native ESP-IDF v5.x)
//   Counts flow-sensor pulses FS1..FS5 with hardware pulse counter (PCNT) and streams
//   running totals to ESP32 #1 over UART at 50 Hz.
//
// Frame format:  $seq,ms,c1,c2,c3,c4,c5,fsw*HH\n
//   c1..c5 = cumulative pulse totals (32-bit)
//   fsw    = bit0: FSW5 active, bit1: FSW6 active
//   HH     = XOR checksum of characters between '$' and '*', two hex digits

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "config.h"

static const char *TAG = "SENSOR_NODE";

static const int FS_PIN[5] = {PIN_FS1, PIN_FS2, PIN_FS3, PIN_FS4, PIN_FS5};

static pcnt_unit_handle_t unitH[5];
static bool     unitOk[5]   = {false, false, false, false, false};
static int      lastRaw[5]  = {0, 0, 0, 0, 0};
static uint32_t total[5]    = {0, 0, 0, 0, 0};
static uint32_t seqNo       = 0;

static portMUX_TYPE dsMux   = portMUX_INITIALIZER_UNLOCKED;
static float    gTemp1      = -999.0f; // TS1: Red Tank (°C)
static float    gTemp2      = -999.0f; // TS2: Yellow Tank (°C)

static void initCounter(int i) {
    if (FS_PIN[i] < 0) {
        ESP_LOGW(TAG, "FS%d pin not configured - skipped", i + 1);
        return;
    }

    gpio_set_pull_mode((gpio_num_t)FS_PIN[i], GPIO_PULLUP_ONLY);

    pcnt_unit_config_t uc = {};
    uc.low_limit  = -32768;
    uc.high_limit = 32767;
    uc.flags.accum_count = 0;

    esp_err_t err = pcnt_new_unit(&uc, &unitH[i]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FS%d unit creation failed: %s", i + 1, esp_err_to_name(err));
        return;
    }

    pcnt_chan_config_t cc = {};
    cc.edge_gpio_num  = FS_PIN[i];
    cc.level_gpio_num = -1;

    pcnt_channel_handle_t ch = NULL;
    err = pcnt_new_channel(unitH[i], &cc, &ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FS%d channel creation failed: %s", i + 1, esp_err_to_name(err));
        return;
    }

    pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(ch, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    pcnt_glitch_filter_config_t fc = {};
    fc.max_glitch_ns = PCNT_GLITCH_NS;
    pcnt_unit_set_glitch_filter(unitH[i], &fc);

    pcnt_unit_enable(unitH[i]);
    pcnt_unit_clear_count(unitH[i]);
    pcnt_unit_start(unitH[i]);
    unitOk[i] = true;
    ESP_LOGI(TAG, "FS%d counting on GPIO %d", i + 1, FS_PIN[i]);
}

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

static uint32_t readFloats() {
    uint32_t v = 0;
    if (PIN_FSW5 >= 0 && gpio_get_level((gpio_num_t)PIN_FSW5) == 0) v |= 1;
    if (PIN_FSW6 >= 0 && gpio_get_level((gpio_num_t)PIN_FSW6) == 0) v |= 2;
    return v;
}

// ---------------------------------------------------------------------------
// DS18B20 1-Wire Implementation (Dedicated Pins TS1=GPIO 32, TS2=GPIO 13)
// ---------------------------------------------------------------------------
static void dsPinInit(int pin) {
    if (pin < 0) return;
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD; // Open drain allows bi-directional 1-Wire
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;   // 4.7k external on PCB + internal assist
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)pin, 1);       // Default idle state HIGH
}

static bool dsReset(int pin) {
    if (pin < 0) return false;
    taskENTER_CRITICAL(&dsMux);
    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(480);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(70);
    int presence = gpio_get_level((gpio_num_t)pin);
    taskEXIT_CRITICAL(&dsMux);
    esp_rom_delay_us(410);
    return (presence == 0);
}

static void dsWriteBit(int pin, int bit) {
    taskENTER_CRITICAL(&dsMux);
    if (bit) {
        gpio_set_level((gpio_num_t)pin, 0);
        esp_rom_delay_us(6);
        gpio_set_level((gpio_num_t)pin, 1);
        esp_rom_delay_us(64);
    } else {
        gpio_set_level((gpio_num_t)pin, 0);
        esp_rom_delay_us(60);
        gpio_set_level((gpio_num_t)pin, 1);
        esp_rom_delay_us(10);
    }
    taskEXIT_CRITICAL(&dsMux);
}

static int dsReadBit(int pin) {
    taskENTER_CRITICAL(&dsMux);
    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(3);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(10);
    int b = gpio_get_level((gpio_num_t)pin);
    esp_rom_delay_us(55);
    taskEXIT_CRITICAL(&dsMux);
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
    dsWriteByte(pin, 0xCC); // SKIP ROM (each pin has its own single probe)
    dsWriteByte(pin, 0x44); // CONVERT T
    return true;
}

static float dsReadTemp(int pin) {
    if (!dsReset(pin)) return -999.0f;
    dsWriteByte(pin, 0xCC); // SKIP ROM
    dsWriteByte(pin, 0xBE); // READ SCRATCHPAD
    uint8_t lsb = dsReadByte(pin);
    uint8_t msb = dsReadByte(pin);
    for (int i = 0; i < 7; i++) dsReadByte(pin); // Flush remaining scratchpad bytes
    int16_t raw = (int16_t)((msb << 8) | lsb);
    if (raw == -1 || raw == 0x0550) {
        return -999.0f; // Disconnected or uninitialized reset value (85.0 C)
    }
    float temp = (float)raw * 0.0625f;
    if (temp < -55.0f || temp > 125.0f) return -999.0f;
    return temp;
}

// Background FreeRTOS task to sample temperatures every 1 second
static void dsTask(void *pv) {
    dsPinInit(PIN_TS1);
    dsPinInit(PIN_TS2);
    while (1) {
        bool ok1 = dsStartConvert(PIN_TS1);
        bool ok2 = dsStartConvert(PIN_TS2);
        vTaskDelay(pdMS_TO_TICKS(750)); // 12-bit DS18B20 conversion delay
        if (ok1) gTemp1 = dsReadTemp(PIN_TS1);
        else gTemp1 = -999.0f;
        if (ok2) gTemp2 = dsReadTemp(PIN_TS2);
        else gTemp2 = -999.0f;
        vTaskDelay(pdMS_TO_TICKS(250)); // 1 Hz total loop
    }
}

static void sendFrame() {
    char body[128];
    uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    int n = snprintf(body, sizeof(body), "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f",
                     (unsigned long)seqNo++, (unsigned long)nowMs,
                     (unsigned long)total[0], (unsigned long)total[1], (unsigned long)total[2],
                     (unsigned long)total[3], (unsigned long)total[4], (unsigned long)readFloats(),
                     gTemp1, gTemp2);

    uint8_t x = 0;
    for (int i = 0; i < n; i++) x ^= (uint8_t)body[i];

    char out[160];
    int m = snprintf(out, sizeof(out), "$%s*%02X\n", body, x);
    uart_write_bytes(LINK_UART_NUM, out, m);
}

static void initHardware() {
    // 1. Configure Float switches
    uint64_t pin_mask = 0;
    if (PIN_FSW5 >= 0) pin_mask |= (1ULL << PIN_FSW5);
    if (PIN_FSW6 >= 0) pin_mask |= (1ULL << PIN_FSW6);
    if (pin_mask) {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = pin_mask;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;   // External 10k on PCB
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&io_conf);
    }

    // 2. Configure UART2 link to ESP32 #1
    uart_config_t uart_config = {};
    uart_config.baud_rate = LINK_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, 512, 512, 0, NULL, 0));

    // 3. Initialize flow pulse counters FS1..FS5
    for (int i = 0; i < 5; i++) {
        initCounter(i);
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "Starting ESP32 #2 Sensor Node (PCNT + DS18B20 + UART2)");
    initHardware();

    // Start background DS18B20 temperature measurement task
    xTaskCreate(dsTask, "ds_temp", 3072, NULL, 2, NULL);

    int64_t lastTxUs = esp_timer_get_time();
    int64_t lastLogUs = esp_timer_get_time();

    while (1) {
        pollCounters();

        int64_t nowUs = esp_timer_get_time();
        if ((nowUs - lastTxUs) >= (TX_PERIOD_MS * 1000)) {
            lastTxUs = nowUs;
            sendFrame();
        }

        if ((nowUs - lastLogUs) >= 1000000) {
            lastLogUs = nowUs;
            ESP_LOGI(TAG, "Pulses FS1..5: %lu %lu %lu %lu %lu | FSW: %lu | TS1(Red): %.2f C | TS2(Yel): %.2f C",
                     (unsigned long)total[0], (unsigned long)total[1], (unsigned long)total[2],
                     (unsigned long)total[3], (unsigned long)total[4], (unsigned long)readFloats(),
                     gTemp1, gTemp2);
        }

        vTaskDelay(1);
    }
}

