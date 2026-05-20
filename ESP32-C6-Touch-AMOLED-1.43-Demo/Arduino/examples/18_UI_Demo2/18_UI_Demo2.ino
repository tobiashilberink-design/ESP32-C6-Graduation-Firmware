#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "esp_timer.h"

/* ── Rotary encoder ──────────────────────────────────────────────────────── */
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

/* ── LED ring ────────────────────────────────────────────────────────────── */
#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ── Perlin noise (1D fBm campfire flicker) ──────────────────────────────── */
static uint8_t perm[512];

static void perlin_init(uint32_t seed) {
    srand(seed);
    for (int i = 0; i < 256; i++) perm[i] = i;
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (int i = 0; i < 256; i++) perm[i + 256] = perm[i];
}
static float perlin_fade(float t) { return t*t*t*(t*(t*6.0f-15.0f)+10.0f); }
static float perlin_grad(int h, float x) { return (h & 1) ? x : -x; }
static float perlin1d(float x) {
    int i = (int)floorf(x) & 255; float f = x - floorf(x);
    return perlin_grad(perm[i], f) + perlin_fade(f) *
           (perlin_grad(perm[i+1], f-1.0f) - perlin_grad(perm[i], f));
}
static float fbm(float t) {
    float v=0, amp=1, freq=1, mx=0;
    for (int i=0; i<4; i++) { v+=perlin1d(t*freq)*amp; mx+=amp; amp*=0.5f; freq*=2.1f; }
    return v/mx;
}
static float campfire_flicker(float t) {
    return 5.0f + ((fbm(t*2.5f)+1.0f)*0.5f) * 250.0f;
}

/* ── LED update ──────────────────────────────────────────────────────────── */
static void update_leds(void) {
    int          screen = get_active_screen();
    wind_state_t state  = get_wind_state();
    float        t      = millis() / 1000.0f;

    /* LEDs only active on wind down screen (screen 2) */
    if (screen != 2) {
        ring.clear();
        ring.show();
        return;
    }

    if (state == WIND_SELECTING) {
        /* ── all LEDs fade in over 4 seconds from when screen was entered ── */
        float progress = (float)(esp_timer_get_time() - get_wind_enter_us()) / 8000000.0f;
        if (progress > 1.0f) progress = 1.0f;
        uint8_t bri = (uint8_t)(progress * 255.0f);
        uint32_t col = ring.Color(bri, 0, 0);
        for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, col);
        ring.show();

    } else if (state == WIND_RUNNING) {
        /* ── campfire flicker, LED count shrinks with remaining time ─────── */
        int64_t elapsed_us  = esp_timer_get_time() - get_wind_start_us();
        int64_t total_us    = (int64_t)get_wind_minutes() * 60LL * 1000000LL;
        int64_t remaining_us = total_us - elapsed_us;
        if (remaining_us < 0) remaining_us = 0;

        /* active_leds is a float: integer part = full LEDs, fraction = partial */
        float active = (float)remaining_us / (float)total_us * (float)LED_COUNT;
        int   n_full = (int)active;
        float frac   = active - (float)n_full;

        float flicker = campfire_flicker(t) / 255.0f;  /* 0..1 */
        for (int i = 0; i < LED_COUNT; i++) {
            float intensity = 0.0f;
            if (i < n_full)        intensity = flicker;
            else if (i == n_full)  intensity = flicker * frac;
            ring.setPixelColor(i, ring.Color((uint8_t)(intensity * 255.0f), 0, 0));
        }
        ring.show();

    } else {
        /* WIND_DONE — all off */
        ring.clear();
        ring.show();
    }
}

/* ── setup ───────────────────────────────────────────────────────────────── */
void setup()
{
    Serial.begin(115200);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);
    enc_prev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  sw_isr,  FALLING);

    perlin_init(esp_random());
    ring.begin();
    ring.setBrightness(255);
    ring.clear();
    ring.show();

    lvgl_port_init();
}

/* ── loop ────────────────────────────────────────────────────────────────── */
void loop()
{
    static int32_t last_enc = 0;
    int32_t enc = enc_pos;

    if (enc != last_enc) {
        on_encoder_delta(enc - last_enc);
        last_enc = enc;
    }
    if (btn_flag) {
        btn_flag = false;
        on_button_press();
    }

    timer_update_tick();
    update_leds();
}
