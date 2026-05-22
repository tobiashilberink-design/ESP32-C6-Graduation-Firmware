/* ═══════════════════════════════════════════════════════════════════════════
   20_UI_Demo3  —  9-screen sleep-prep UI
   Screens: Clock | Alarm | Guide | Wind timer | Heart Coherence |
            Music | Podcast | Background noise | Volume
   ═══════════════════════════════════════════════════════════════════════════ */

#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "esp_timer.h"

extern "C" {
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
}

/* ── Rotary encoder ───────────────────────────────────────────────────────── */
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

/* ── LED ring ─────────────────────────────────────────────────────────────── */
#define LED_PIN    2
#define LED_COUNT  24
#define LED_OFFSET 6                                    /* 1/4 turn anti-clockwise */
#define LED_IDX(i) (((i) + LED_OFFSET) % LED_COUNT)    /* logical → physical index */

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ── Audio (background noise) ─────────────────────────────────────────────── */
#define SAMPLE_RATE  24000
#define CHANNELS     2
#define BUF_BYTES    1024
#define BUF_FRAMES   (BUF_BYTES / (CHANNELS * 2))

static esp_codec_dev_handle_t   playback    = NULL;
static esp_codec_dev_handle_t   record_dev  = NULL;
static esp_io_expander_handle_t io_expander = NULL;
static int16_t                 *audio_buf   = NULL;
static float                    pink_state  = 0.0f;

/* ── Audio task — runs independently, never blocks loop() ────────────────── */
static void audio_task(void *arg)
{
    for (;;) {
        if (audio_buf && playback) {
            if (get_active_screen() == SCR_BGNOISE &&
                get_wind_state()    == WIND_RUNNING) {
                fill_wind_noise();
            } else {
                memset(audio_buf, 0, BUF_BYTES);
                pink_state = 0.0f;   /* reset filter to avoid pop on resume */
            }
            esp_codec_dev_write(playback, audio_buf, BUF_BYTES);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* pink noise at 20 % volume — wind-like */
static void fill_wind_noise(void) {
    float amp = 0.20f * 0.75f * 32767.0f;
    for (int i = 0; i < BUF_FRAMES; i++) {
        pink_state = pink_state * 0.99f +
                     (rand() / (float)RAND_MAX - 0.5f) * 0.2f;
        int16_t s  = (int16_t)(pink_state * amp);
        audio_buf[i * 2]     = s;
        audio_buf[i * 2 + 1] = s;
    }
}

/* ── Perlin noise (1-D fBm campfire flicker) ──────────────────────────────── */
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
    int   i = (int)floorf(x) & 255;
    float f = x - floorf(x);
    return perlin_grad(perm[i], f) + perlin_fade(f) *
           (perlin_grad(perm[i+1], f-1.0f) - perlin_grad(perm[i], f));
}
static float fbm(float t) {
    float v=0, amp=1, freq=1, mx=0;
    for (int i=0; i<4; i++) { v+=perlin1d(t*freq)*amp; mx+=amp; amp*=0.5f; freq*=2.1f; }
    return v / mx;
}
static float campfire_flicker(float t) {
    return 5.0f + ((fbm(t*2.5f)+1.0f)*0.5f) * 250.0f;
}

/* ── LED update ───────────────────────────────────────────────────────────── */
static void update_leds(void) {
    int          screen = get_active_screen();
    wind_state_t state  = get_wind_state();
    float        t      = millis() / 1000.0f;

    /* ── Wind-down timer screen (Im good) ── */
    if (screen == SCR_WIND) {
        if (state == WIND_SELECTING) {
            /* fade all LEDs in over 8 s from screen entry */
            float progress = (float)(esp_timer_get_time() - get_wind_enter_us()) / 8000000.0f;
            if (progress > 1.0f) progress = 1.0f;
            uint8_t bri  = (uint8_t)(progress * 255.0f);
            uint32_t col = ring.Color(bri, 0, 0);
            for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(LED_IDX(i), col);
            ring.show();

        } else if (state == WIND_RUNNING) {
            /* campfire flicker, count shrinks with remaining time */
            int64_t elapsed_us   = esp_timer_get_time() - get_wind_start_us();
            int64_t total_us     = (int64_t)get_wind_minutes() * 60LL * 1000000LL;
            int64_t remaining_us = total_us - elapsed_us;
            if (remaining_us < 0) remaining_us = 0;

            float active = (float)remaining_us / (float)total_us * (float)LED_COUNT;
            int   n_full = (int)active;
            float frac   = active - (float)n_full;
            float flicker = campfire_flicker(t) / 255.0f;

            for (int i = 0; i < LED_COUNT; i++) {
                float intensity = 0.0f;
                if (i < n_full)       intensity = flicker;
                else if (i == n_full) intensity = flicker * frac;
                ring.setPixelColor(LED_IDX(i),
                    ring.Color((uint8_t)(intensity * 255.0f), 0, 0));
            }
            ring.show();

        } else {
            ring.clear(); ring.show();
        }

    /* ── Heart Coherence: smooth sine between 5-40 % brightness, 10 s period ── */
    } else if (screen == SCR_HEART && state == WIND_RUNNING) {
        /* bri = 13 (5%) to 102 (40%), full 10 s cycle */
        float bri_f = 57.5f + 44.5f * sinf(2.0f * (float)M_PI * t / 10.0f);
        uint8_t bri = (uint8_t)bri_f;
        uint32_t col = ring.Color(bri, 0, 0);
        for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(LED_IDX(i), col);
        ring.show();

    /* ── Connect: very slow soft pulse 8-20 % brightness, 16 s period ────── */
    } else if (screen == SCR_CONNECT && state == WIND_RUNNING) {
        /* bri = 20 (8%) to 51 (20%), gentle 16 s cycle */
        float bri_f = 20.0f + 15.5f * (1.0f + sinf(2.0f * (float)M_PI * t / 16.0f));
        uint8_t bri = (uint8_t)bri_f;
        uint32_t col = ring.Color(bri, 0, 0);
        for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(LED_IDX(i), col);
        ring.show();

    } else {
        ring.clear(); ring.show();
    }
}

/* ── setup ────────────────────────────────────────────────────────────────── */
void setup()
{
    Serial.begin(115200);
    delay(2000);

    perlin_init(esp_random());
    ring.begin();
    ring.setBrightness(255);
    ring.clear();
    ring.show();

    /* display + LVGL (SPI — independent of I2C) */
    lvgl_port_init();

    /* codec (creates and owns I2C port 0) */
    set_codec_board_type("C6_AMOLED_1_43");
    codec_init_cfg_t codec_cfg = {};
    int codec_ret = init_codec(&codec_cfg);
    Serial.printf("init_codec: %d\n", codec_ret);

    /* TCA9554 borrows codec's I2C bus to enable speaker amplifier */
    i2c_master_bus_handle_t i2c_bus =
        (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    esp_io_expander_new_i2c_tca9554(i2c_bus,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
    esp_io_expander_set_dir  (io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);

    playback   = get_playback_handle();
    record_dev = get_record_handle();
    Serial.printf("playback: %p  record: %p\n", playback, record_dev);

    esp_codec_dev_set_out_vol(playback,   90.0f);
    esp_codec_dev_set_in_gain(record_dev, 35.0f);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = SAMPLE_RATE;
    fs.channel         = CHANNELS;
    fs.bits_per_sample = 16;
    esp_codec_dev_open(playback,   &fs);
    esp_codec_dev_open(record_dev, &fs);

    audio_buf = (int16_t *)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DEFAULT);
    assert(audio_buf);
    memset(audio_buf, 0, BUF_BYTES);

    /* touch — shares codec I2C bus, must init after codec */
    lvgl_touch_init(i2c_bus);

    /* audio task — runs independently so loop() is never blocked by I2S */
    xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL);

    /* encoder */
    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);
    enc_prev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  sw_isr,  FALLING);

    Serial.println("20_UI_Demo3 ready");
}

/* ── loop ─────────────────────────────────────────────────────────────────── */
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
