#include <Adafruit_NeoPixel.h>
#include "esp_timer.h"

// ── LED ring ──────────────────────────────────────────────
#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── Rotary encoder ────────────────────────────────────────
#define ENC_CLK 15
#define ENC_DT  16
#define ENC_SW  17

static volatile int32_t enc_pos  = 0;
static volatile uint8_t enc_prev = 0;
static volatile bool    btn_flag = false;

static const int8_t dir_table[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

void IRAM_ATTR enc_isr(void) {
    uint8_t curr = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    enc_pos += dir_table[(enc_prev << 2) | curr];
    enc_prev = curr;
}

void IRAM_ATTR sw_isr(void) {
    static uint32_t last_us = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - last_us > 50000) { btn_flag = true; last_us = now; }
}

// ── State ─────────────────────────────────────────────────
// lit_count: how many LEDs are on (0–24), controlled by encoder
static int lit_count = 0;

void setup() {
    Serial.begin(115200);

    // Encoder
    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);
    enc_prev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  sw_isr,  FALLING);

    // LED ring
    ring.begin();
    ring.setBrightness(80);  // 0–255; keep low to limit current draw on 3.3 V
    ring.show();
}

void loop() {
    static int32_t last_enc = 0;

    // Encoder turns change lit_count by 1 per step, clamped to 0–24
    int32_t enc = enc_pos;
    if (enc != last_enc) {
        int delta = enc - last_enc;
        lit_count = constrain(lit_count + delta, 0, LED_COUNT);
        last_enc = enc;
        update_ring();
        Serial.printf("LEDs lit: %d\n", lit_count);
    }

    // Button press resets the ring
    if (btn_flag) {
        btn_flag  = false;
        lit_count = 0;
        enc_pos   = 0;
        last_enc  = 0;
        update_ring();
        Serial.println("Reset");
    }
}

void update_ring() {
    ring.clear();
    for (int i = 0; i < lit_count; i++) {
        ring.setPixelColor(i, ring.Color(220, 0, 0));  // red
    }
    ring.show();
}
