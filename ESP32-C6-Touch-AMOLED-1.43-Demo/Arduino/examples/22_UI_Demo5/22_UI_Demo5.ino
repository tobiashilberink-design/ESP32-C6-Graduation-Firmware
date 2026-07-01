/* ═══════════════════════════════════════════════════════════════════════════
   21_UI_Demo4  —  11-screen sleep-prep UI (user-test build)
   Screens: Clock | Alarm | Guide | Wind timer | Heart Coherence |
            Connect | Music | Podcast | Background noise | Volume | Reset
   ═══════════════════════════════════════════════════════════════════════════ */

#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"
#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

extern "C" {
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
}

/* ── Dial: two hall sensors in quadrature + push button ───────────────────── */
/*  The two KY-003 hall sensors feed the same A/B quadrature decoder a rotary
 *  encoder uses, so direction is correct from any starting position. The push
 *  button is on GPIO 17 and is polled. */
#define HALL_A  15
#define HALL_B  16
#define BTN_PIN 17

static volatile int32_t  enc_pos    = 0;
static volatile uint8_t  enc_prev   = 0;
static volatile uint32_t hall_edges = 0;   /* any edge on either sensor */

static const int8_t dir_table[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

void IRAM_ATTR enc_isr(void) {
    hall_edges++;
    uint8_t curr = (digitalRead(HALL_A) << 1) | digitalRead(HALL_B);
    enc_pos -= dir_table[(enc_prev << 2) | curr];   /* '-' reverses direction */
    enc_prev = curr;
}

/* Polled-debounce button — calls on_button_press() once per press (the make). */
static void poll_button(void) {
    static bool     confirmed = false;
    static bool     raw_last  = false;
    static uint32_t stable_ms = 0;
    bool     raw = (digitalRead(BTN_PIN) == LOW);
    uint32_t now = millis();
    if (raw != raw_last) { raw_last = raw; stable_ms = now; return; }
    if (now - stable_ms < 8) return;        /* wait for 8 ms of stability */
    if (raw != confirmed) {
        confirmed = raw;
        if (confirmed) on_button_press();
    }
}

/* ── Haptic engine (DRV2605L) on the shared I2C bus ───────────────────────── */
/*  Effect 1 (Strong Click) is preloaded once; each tick is a single GO write
 *  over the codec's shared I2C bus. Set ENABLE_HAPTIC to 0 to leave the chip
 *  completely untouched by software (for isolating a bus problem). */
#define ENABLE_HAPTIC 1
#define DRV2605_ADDR  0x5A
#define HAPTIC_EVERY  1      /* fire one click every N scroll counts (1 = every tick) */
#define HAPTIC_EFFECT 24     /* ROM effect: 24 = Sharp Tick (short, light). Softer
                                options: 6 = Sharp Click 30%, 7 = Soft Bump.        */

static i2c_master_dev_handle_t drv_dev   = NULL;
static bool                    haptic_ok = false;

static esp_err_t drv_w(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(drv_dev, b, sizeof(b), 50);
}
static uint8_t drv_r(uint8_t reg) {
    uint8_t v = 0;
    i2c_master_transmit_receive(drv_dev, &reg, 1, &v, 1, 50);
    return v;
}

/* Load the working register configuration. Called at boot, and again to
   self-heal if the chip ever drops out of its configured state. */
static void haptic_config(void) {
    drv_w(0x01, 0x00);                  /* mode: internal trigger (I2C GO)      */
    drv_w(0x02, 0x00);                  /* RTP off                              */
    drv_w(0x04, HAPTIC_EFFECT);         /* waveform slot 0 = short light tick    */
    drv_w(0x05, 0);                     /* slot 1 = end of sequence             */
    drv_w(0x0D, 0); drv_w(0x0E, 0); drv_w(0x0F, 0); drv_w(0x10, 0);
    drv_w(0x11, 0x64);                  /* audio-to-vibe peak                   */
    drv_w(0x1A, drv_r(0x1A) | 0x80);    /* LRA mode (set N_ERM_LRA bit)         */
    drv_w(0x03, 6);                     /* ROM library 6 = LRA effects          */
}

static void haptic_init(i2c_master_bus_handle_t bus) {
#if ENABLE_HAPTIC
    /* Probe first — never add/talk to a device that isn't ACKing. */
    if (i2c_master_probe(bus, DRV2605_ADDR, 50) != ESP_OK) return;

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = DRV2605_ADDR;
    dc.scl_speed_hz    = 100000;
    if (i2c_master_bus_add_device(bus, &dc, &drv_dev) != ESP_OK) return;

    haptic_config();
    haptic_ok = true;
#else
    (void)bus;
#endif
}

static inline void haptic_click(void) {
    if (!haptic_ok) return;
    /* If the GO write fails, the chip lost its state (or the bus hiccupped) —
       re-apply the config and fire once more. This self-heals the "stops after
       ~1 minute" problem without needing a reboot. */
    if (drv_w(0x0C, 0x01) != ESP_OK) {
        haptic_config();
        drv_w(0x0C, 0x01);
    }
}

/* ── BLE proximity ────────────────────────────────────────────────────────── */
#define BLE_EMA_ALPHA   0.08f
#define BLE_RSSI_CLOSE  (-55.0f)
#define BLE_RSSI_FAR    (-75.0f)

static bool            ble_connected = false;
static esp_bd_addr_t   ble_peer_bda  = {};
static volatile int8_t ble_raw_rssi  = -90;
static float           ble_ema_rssi  = -90.0f;

class ProxServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
        memcpy(ble_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        ble_ema_rssi  = -90.0f;
        ble_connected = true;
        lvgl_set_ble_connected(true);
    }
    void onDisconnect(BLEServer *pServer) override {
        ble_connected = false;
        ble_raw_rssi  = -90;
        ble_ema_rssi  = -90.0f;
        lvgl_set_ble_connected(false);
        BLEDevice::startAdvertising();
    }
};

static void on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT &&
        param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS)
        ble_raw_rssi = param->read_rssi_cmpl.rssi;
}

/* ── CTS (Current Time Service) — phone writes 10-byte time on connect ───── */
/*   Byte layout: year(2 LE) | month | day | hours | minutes | seconds | ...   */
/*   NOTE: use getData() not getValue() — Arduino String truncates at 0x00,    */
/*   which breaks hours=0 (midnight) or minutes=0.                             */
class CTSCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        uint8_t *data = pChar->getData();
        size_t   len  = pChar->getLength();
        Serial.printf("CTS write received: len=%d\n", (int)len);
        if (data && len >= 6) {
            uint8_t h = data[4];
            uint8_t m = data[5];
            Serial.printf("CTS raw bytes: h=%d m=%d\n", h, m);
            if (h < 24 && m < 60) {
                lvgl_set_cts_time((int)h, (int)m);
                Serial.printf("CTS synced: %02d:%02d\n", h, m);
            } else {
                Serial.printf("CTS invalid time values\n");
            }
        } else {
            Serial.printf("CTS write too short or null\n");
        }
    }
};

/* ── LED ring ─────────────────────────────────────────────────────────────── */
#define LED_PIN    2
#define LED_COUNT  24
#define LED_OFFSET 4                                    /* start LED 6 more anti-clockwise */
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

/* ── Karplus-Strong alarm audio ───────────────────────────────────────────── */
#define ALARM_KS_BUF_LEN  220   /* buffer capacity */
#define ALARM_KS_SIZE     110   /* active delay line: 24000/110 ≈ 218 Hz */

static float    alarm_ks_buf[ALARM_KS_BUF_LEN];
static int      alarm_ks_pos    = 0;
static uint32_t alarm_ks_last_ms = 0;

static void alarm_ks_pluck(void) {
    for (int i = 0; i < ALARM_KS_SIZE; i++)
        alarm_ks_buf[i] = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    alarm_ks_pos = 0;
}

/* Fill audio_buf with Karplus-Strong string.
   ramp 0..1 over 120 s: volume 0 → vol_pct/100, interval 5 s → 2 s.   */
static void fill_alarm_karplus(float ramp, float vol_pct)
{
    /* pluck interval in ms: 5000 → 2000 */
    uint32_t interval_ms = (uint32_t)(5000.0f - ramp * 3000.0f);
    if (millis() - alarm_ks_last_ms >= interval_ms) {
        alarm_ks_pluck();
        alarm_ks_last_ms = millis();
    }
    float amp = ramp * (vol_pct / 100.0f) * 32767.0f;
    for (int i = 0; i < BUF_FRAMES; i++) {
        float s    = alarm_ks_buf[alarm_ks_pos];
        int   next = (alarm_ks_pos + 1) % ALARM_KS_SIZE;
        alarm_ks_buf[alarm_ks_pos] = (alarm_ks_buf[alarm_ks_pos] + alarm_ks_buf[next]) * 0.498f;
        alarm_ks_pos = next;
        int16_t out = (int16_t)(s * amp);
        audio_buf[i * 2]     = out;
        audio_buf[i * 2 + 1] = out;
    }
}

/* ── Audio task — runs independently, never blocks loop() ────────────────── */
static void audio_task(void *arg)
{
    int last_vol = -1;
    for (;;) {
        if (audio_buf && playback) {
            int vol = get_volume_val();
            if (vol != last_vol) {
                esp_codec_dev_set_out_vol(playback, (float)vol);
                last_vol = vol;
            }

            /* Background noise has highest priority — it must play whenever
               the user is on the noise screen during wind-down, regardless
               of alarm state (alarm audio is signalled via LEDs too).     */
            if (get_active_screen() == SCR_BGNOISE &&
                get_wind_state()    == WIND_RUNNING) {
                fill_wind_noise();

            } else if (get_alarm_ringing()) {
                /* alarm audio: Karplus-Strong string ramping over 120 s */
                int64_t elapsed_us = esp_timer_get_time() - get_alarm_ringing_start_us();
                float   ramp       = (float)elapsed_us / 120000000.0f;
                if (ramp > 1.0f) ramp = 1.0f;
                fill_alarm_karplus(ramp, (float)vol);

            } else {
                memset(audio_buf, 0, BUF_BYTES);
                pink_state = 0.0f;
                alarm_ks_last_ms = 0;
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

/* ── Top-down LED priority ────────────────────────────────────────────────── */
/* LED_TOP_LOGICAL: logical index (before LED_IDX offset) of the 12-o'clock LED.
   Adjust this constant if the top LED is off after hardware testing.          */
#define LED_TOP_LOGICAL  0

/* Returns 0..23: 0 = last to go dark (bottom), 23 = first to go dark (top).
   LED i is lit when n_lit > led_top_prio(i).                                  */
static int led_top_prio(int logical_i)
{
    int d = (logical_i - LED_TOP_LOGICAL + LED_COUNT) % LED_COUNT;
    if (d > 12) d = LED_COUNT - d;          /* fold: 0=top, 12=bottom           */
    if (d == 0)  return 23;                  /* top LED — first off               */
    if (d == 12) return 0;                   /* bottom LED — last off             */
    /* clockwise (d ≤ 12 from top going CW) gets even priorities,
       counterclockwise gets odd — creates a very slight CW-first tiebreak.    */
    bool cw = ((logical_i - LED_TOP_LOGICAL + LED_COUNT) % LED_COUNT) <= 12;
    return cw ? (24 - 2 * d) : (23 - 2 * d);
}

/* ── LED update ───────────────────────────────────────────────────────────── */
static void update_leds(void) {
    int          screen = get_active_screen();
    wind_state_t state  = get_wind_state();
    float        t      = millis() / 1000.0f;

    /* ── Priority 1: alarm ringing — slow 3 s pulse, cool white/blue ─────── */
    /* Same white/blue as the end of the pre-alarm sunrise (all channels equal;
       WS2812B blue is efficient, so it reads slightly cool).                   */
    if (get_alarm_ringing()) {
        float t_alarm = (float)(esp_timer_get_time() - get_alarm_ringing_start_us())
                        / 1000000.0f;
        float pulse = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * t_alarm / 3.0f));
        uint8_t w = (uint8_t)(pulse * 255.0f);
        for (int i = 0; i < LED_COUNT; i++)
            ring.setPixelColor(LED_IDX(i), ring.Color(w, w, w));
        ring.show();
        return;
    }

    /* ── Priority 2: pre-alarm sunrise — 30 min before alarm, CLOCK page only ─ */
    /* Not shown on the alarm page (you'd be setting the time there). Brightens
       from off, and the colour walks red → warm white → cool white/blue as the
       alarm nears: red ramps first, green lags, blue lags most.                */
    if (screen == SCR_CLOCK) {
        int alarm_min = get_alarm_total_min();
        int now_min   = get_clock_total_min();
        int diff      = (alarm_min - now_min + 1440) % 1440;   /* minutes until alarm */
        if (diff > 0 && diff <= 30) {
            float frac = 1.0f - (float)diff / 30.0f;   /* 0 at 30 min before → 1 at alarm */
            uint8_t r = (uint8_t)(frac * 255.0f);              /* red leads      */
            uint8_t g = (uint8_t)(frac * frac * 255.0f);       /* green follows  */
            uint8_t b = (uint8_t)(frac * frac * frac * 255.0f);/* blue last → cool white */
            for (int i = 0; i < LED_COUNT; i++)
                ring.setPixelColor(LED_IDX(i), ring.Color(r, g, b));
            ring.show();
            return;
        }
    }

    /* ── Wind-down timer screen ──────────────────────────────────────────── */
    if (screen == SCR_WIND) {
        if (state == WIND_SELECTING) {
            /* fade all LEDs to 20 % over 8 s — warm orange glow */
            float progress = (float)(esp_timer_get_time() - get_wind_enter_us()) / 8000000.0f;
            if (progress > 1.0f) progress = 1.0f;
            uint8_t r = (uint8_t)(progress * 51.0f);
            uint8_t g = (uint8_t)(progress * 18.0f);
            for (int i = 0; i < LED_COUNT; i++)
                ring.setPixelColor(LED_IDX(i), ring.Color(r, g, 0));
            ring.show();

        } else if (state == WIND_RUNNING) {
            int64_t elapsed_us   = esp_timer_get_time() - get_wind_start_us();
            int64_t total_us     = (int64_t)get_wind_minutes() * 60LL * 1000000LL;
            int64_t remaining_us = total_us - elapsed_us;
            if (remaining_us < 0) remaining_us = 0;

            /* floating-point count of how many LEDs should be lit */
            float active = (float)remaining_us / (float)total_us * (float)LED_COUNT;
            int   n_full = (int)active;
            float frac   = active - (float)n_full;
            float flicker = campfire_flicker(t) / 255.0f;

            /* last 30 % of wind-down → green fades out (orange → deep red) */
            float elapsed_frac = 1.0f - (float)remaining_us / (float)total_us;
            float color_factor = 1.0f;
            if (elapsed_frac > 0.70f)
                color_factor = 1.0f - (elapsed_frac - 0.70f) / 0.30f;
            if (color_factor < 0.0f) color_factor = 0.0f;

            for (int i = 0; i < LED_COUNT; i++) {
                /* top-down: determine intensity using priority rank */
                int   prio      = led_top_prio(i);
                float intensity = 0.0f;
                if      (prio < n_full)  intensity = flicker;
                else if (prio == n_full) intensity = flicker * frac;

                uint8_t r = (uint8_t)(intensity * 255.0f);
                uint8_t g = (uint8_t)(intensity * intensity * 140.0f * color_factor);
                ring.setPixelColor(LED_IDX(i), ring.Color(r, g, 0));
            }
            ring.show();

        } else {
            ring.clear(); ring.show();
        }

    /* ── Heart Coherence: -cosine breath pulse synced to wind_start ─────── */
    } else if (screen == SCR_HEART && state == WIND_RUNNING) {
        float t_breath = (float)(esp_timer_get_time() - get_wind_start_us()) / 1000000.0f;
        float bri_f    = 26.0f * (1.0f - cosf(2.0f * (float)M_PI * t_breath / 10.0f));
        uint8_t r = (uint8_t)bri_f;
        uint8_t g = (uint8_t)(bri_f * bri_f * 0.55f / 255.0f);
        for (int i = 0; i < LED_COUNT; i++)
            ring.setPixelColor(LED_IDX(i), ring.Color(r, g, 0));
        ring.show();

    /* ── Connect: gentle 16 s pulse ─────────────────────────────────────── */
    } else if (screen == SCR_CONNECT && state == WIND_RUNNING) {
        float bri_f = 20.0f + 15.5f * (1.0f + sinf(2.0f * (float)M_PI * t / 16.0f));
        uint8_t r = (uint8_t)bri_f;
        uint8_t g = (uint8_t)(bri_f * bri_f * 0.55f / 255.0f);
        for (int i = 0; i < LED_COUNT; i++)
            ring.setPixelColor(LED_IDX(i), ring.Color(r, g, 0));
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

    /* haptic engine — shares the same I2C bus, init after codec owns it */
    haptic_init(i2c_bus);
    Serial.printf("haptic DRV2605: %s\n", haptic_ok ? "OK" : "NOT FOUND");

    /* audio task — runs independently so loop() is never blocked by I2S */
    xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL);

    /* dial — hall sensors (quadrature) + polled button */
    pinMode(HALL_A,  INPUT_PULLUP);
    pinMode(HALL_B,  INPUT_PULLUP);
    pinMode(BTN_PIN, INPUT_PULLUP);
    enc_prev = (digitalRead(HALL_A) << 1) | digitalRead(HALL_B);
    attachInterrupt(digitalPinToInterrupt(HALL_A), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(HALL_B), enc_isr, CHANGE);
    /* button is polled in loop() */

    /* BLE — advertise as "Intent", scan RSSI of connected phone */
    BLEDevice::init("Intent");
    BLEDevice::setCustomGapHandler(on_gap_event);
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ProxServerCallbacks());
    BLEService *pService = pServer->createService(
        "4FAFC201-1FB5-459E-8FCC-C5C9C331914B");
    pService->start();

    /* Current Time Service 0x1805 — iOS writes current time on connection */
    BLEService *pCTSService = pServer->createService(BLEUUID((uint16_t)0x1805));
    BLECharacteristic *pCTSChar = pCTSService->createCharacteristic(
        BLEUUID((uint16_t)0x2A2B),
        BLECharacteristic::PROPERTY_READ  |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY);
    pCTSChar->addDescriptor(new BLE2902());
    pCTSChar->setCallbacks(new CTSCallbacks());
    pCTSService->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID("4FAFC201-1FB5-459E-8FCC-C5C9C331914B");
    pAdv->addServiceUUID(BLEUUID((uint16_t)0x1805));   /* CTS — triggers iOS auto-sync */
    pAdv->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("BLE advertising as 'Intent'");

    Serial.println("21_UI_Demo4 ready");
}

/* ── loop ─────────────────────────────────────────────────────────────────── */
void loop()
{
    static int32_t  last_enc    = 0;
    static uint32_t last_ble_ms = 0;

    int32_t enc = enc_pos;
    if (enc != last_enc) {
        on_encoder_delta(enc - last_enc);
        last_enc = enc;
        /* haptic: only one click per HAPTIC_EVERY counts → a gentle tick, not a buzz */
        static int32_t last_haptic_enc = 0;
        int32_t d = enc - last_haptic_enc;
        if (d < 0) d = -d;
        if (d >= HAPTIC_EVERY) {
            haptic_click();
            last_haptic_enc = enc;
        }
    }
    poll_button();

    /* ── BLE proximity — poll RSSI every 200 ms ── */
    uint32_t now_ms = millis();
    if (ble_connected && now_ms - last_ble_ms >= 200) {
        esp_ble_gap_read_rssi(ble_peer_bda);
        last_ble_ms = now_ms;
    }
    float ble_proximity = 0.0f;
    if (ble_connected) {
        ble_ema_rssi = BLE_EMA_ALPHA * (float)ble_raw_rssi +
                       (1.0f - BLE_EMA_ALPHA) * ble_ema_rssi;
        float t = (ble_ema_rssi - BLE_RSSI_FAR) /
                  (BLE_RSSI_CLOSE - BLE_RSSI_FAR);
        ble_proximity = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    }
    lvgl_set_ble_proximity(ble_proximity);

    timer_update_tick();
    update_leds();
}
