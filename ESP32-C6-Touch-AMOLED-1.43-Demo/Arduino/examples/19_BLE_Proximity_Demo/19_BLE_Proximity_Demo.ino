/* ═══════════════════════════════════════════════════════════════════════════
   19_BLE_Proximity_Demo
   Advertises as "Intent" over BLE. Connect via NRF Connect.
   RSSI → LED flicker intensity + audio character.
   3 audio modes rotate every 10 s:
     0  Smooth drone      — continuous theremin-like pitch shift
     1  Karplus-Strong    — plucked string, faster when closer
     2  Pink noise        — wind/breath, louder when closer
   Display: proximity arc | mode name | NEAR / CLOSE / FAR / — text
   ═══════════════════════════════════════════════════════════════════════════ */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_gap_ble_api.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "user_config.h"

extern "C" {
#include "src/sh8601/esp_lcd_sh8601.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
}

/* ── LED ring ─────────────────────────────────────────────────────────────── */
#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ── Audio ────────────────────────────────────────────────────────────────── */
#define SAMPLE_RATE  24000
#define CHANNELS     2
#define BUF_BYTES    1024
#define BUF_FRAMES   (BUF_BYTES / (CHANNELS * 2))   /* 256 stereo frames */

static esp_codec_dev_handle_t   playback    = NULL;
static esp_codec_dev_handle_t   record      = NULL;
static esp_io_expander_handle_t io_expander = NULL;
static int16_t                 *audio_buf   = NULL;

/* ── Mode cycling ─────────────────────────────────────────────────────────── */
#define NUM_MODES      3
#define MODE_DURATION  10000UL   /* ms per mode */

static const char *MODE_NAMES[NUM_MODES] = {
    "Smooth drone",
    "Karplus-Strong",
    "Pink noise"
};
static uint8_t current_mode = 255;   /* 255 forces init on first loop */

/* ── Per-mode state ───────────────────────────────────────────────────────── */

/* mode 0: drone */
static float    drone_phase = 0.0f;
static float    drone_trem  = 0.0f;

/* mode 1: Karplus-Strong */
#define KS_MAX 220
static float    ks_buf[KS_MAX];
static int      ks_size    = 110;    /* 24000/110 ≈ 218 Hz */
static int      ks_pos     = 0;
static uint32_t ks_last_ms = 0;

/* mode 2: pink noise */
static float    pink_state = 0.0f;

/* ── BLE state ────────────────────────────────────────────────────────────── */
static bool            connected = false;
static esp_bd_addr_t   peer_bda  = {};
static volatile int8_t raw_rssi  = -90;

/* ── RSSI smoothing ───────────────────────────────────────────────────────── */
static float ema_rssi  = -90.0f;
#define EMA_ALPHA  0.08f
#define RSSI_CLOSE (-55.0f)
#define RSSI_FAR   (-75.0f)

/* ── Perlin noise (1-D fBm) ───────────────────────────────────────────────── */
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
static float perlin_fade(float t) { return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f); }
static float perlin_grad(int h, float x) { return (h & 1) ? x : -x; }
static float perlin1d(float x) {
    int   i = (int)floorf(x) & 255;
    float f = x - floorf(x);
    return perlin_grad(perm[i], f) + perlin_fade(f) *
           (perlin_grad(perm[i + 1], f - 1.0f) - perlin_grad(perm[i], f));
}
static float fbm(float t) {
    float v = 0, amp = 1, freq = 1, mx = 0;
    for (int i = 0; i < 4; i++) {
        v += perlin1d(t * freq) * amp; mx += amp;
        amp *= 0.5f; freq *= 2.1f;
    }
    return v / mx;
}

/* ── proximity 0..1 from EMA RSSI ────────────────────────────────────────── */
static float rssi_to_proximity(float rssi) {
    float t = (rssi - RSSI_FAR) / (RSSI_CLOSE - RSSI_FAR);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* ── LED flicker intensity 0..255 ────────────────────────────────────────── */
static float compute_led_intensity(float t, float proximity) {
    float speed  = 2.5f + proximity * 12.5f;
    float raw    = (fbm(t * speed) + 1.0f) * 0.5f;
    float shaped = powf(raw, 1.0f + proximity * 4.0f);
    float floor_ = 20.0f * (1.0f - proximity);
    return floor_ + shaped * (255.0f - floor_);
}

/* ═══════════════════════════════════════════════════════════════════════════
   DISPLAY  (LVGL 8 + SH8601 QSPI)
   ═══════════════════════════════════════════════════════════════════════════ */

#define LCD_HOST           SPI2_HOST
#define LCD_BIT_PER_PIXEL  16

#define C_BG    lv_color_make( 15,  4,  4)
#define C_RED   lv_color_make(220, 48, 48)
#define C_MID   lv_color_make(140, 30, 30)
#define C_DIM   lv_color_make( 65, 14, 14)
#define C_TRACK lv_color_make( 38,  9,  9)

static SemaphoreHandle_t lvgl_mux   = NULL;
static lv_obj_t         *g_prox_arc = NULL;
static lv_obj_t         *g_mode_lbl = NULL;
static lv_obj_t         *g_dist_lbl = NULL;
static lv_obj_t         *g_dbm_lbl  = NULL;

/* ── LCD init sequence ────────────────────────────────────────────────────── */
static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 80},
    {0xC4, (uint8_t []){0x80}, 1,  0},
    {0x53, (uint8_t []){0x20}, 1,  1},
    {0x63, (uint8_t []){0xFF}, 1,  1},
    {0x51, (uint8_t []){0x00}, 1,  1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1,  0},
};

/* ── LVGL callbacks ───────────────────────────────────────────────────────── */
static bool lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t,
                                esp_lcd_panel_io_event_data_t *, void *user_ctx) {
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    /* SH8601 on this board has a 6-pixel x-offset */
    esp_lcd_panel_draw_bitmap(panel,
        area->x1 + 0x06, area->y1,
        area->x2 + 0x06 + 1, area->y2 + 1, color_map);
}

static void lvgl_rounder_cb(struct _lv_disp_drv_t *, lv_area_t *area) {
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_tick_cb(void *) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static bool lvgl_lock(int timeout_ms) {
    assert(lvgl_mux);
    TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, t) == pdTRUE;
}
static void lvgl_unlock(void) {
    assert(lvgl_mux);
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_task(void *) {
    uint32_t delay = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (lvgl_lock(-1)) {
            delay = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay > LVGL_TASK_MAX_DELAY_MS) delay = LVGL_TASK_MAX_DELAY_MS;
        if (delay < LVGL_TASK_MIN_DELAY_MS) delay = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

/* ── UI construction ──────────────────────────────────────────────────────── */
static void create_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── proximity arc (135° → 45°, i.e. 270° sweep) ── */
    g_prox_arc = lv_arc_create(scr);
    lv_obj_set_size(g_prox_arc, 350, 350);
    lv_obj_center(g_prox_arc);
    lv_arc_set_mode(g_prox_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_prox_arc, 0, 100);
    lv_arc_set_value(g_prox_arc, 0);
    lv_arc_set_bg_angles(g_prox_arc, 135, 45);
    lv_obj_set_style_arc_color(g_prox_arc,  C_RED,         LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_prox_arc,  22,            LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_prox_arc, true,         LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_prox_arc,  C_TRACK,       LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_prox_arc,  22,            LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_prox_arc,     LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_prox_arc,     LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_prox_arc,    0,             LV_PART_KNOB);
    lv_obj_clear_flag(g_prox_arc, LV_OBJ_FLAG_CLICKABLE);

    /* ── mode name (small, above centre) ── */
    g_mode_lbl = lv_label_create(scr);
    lv_label_set_text(g_mode_lbl, "—");
    lv_obj_set_style_text_font(g_mode_lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_mode_lbl, C_MID,                  LV_PART_MAIN);
    lv_obj_align(g_mode_lbl, LV_ALIGN_CENTER, 0, -55);

    /* ── distance text (large, centre) ── */
    g_dist_lbl = lv_label_create(scr);
    lv_label_set_text(g_dist_lbl, "—");
    lv_obj_set_style_text_font(g_dist_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_dist_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(g_dist_lbl, LV_ALIGN_CENTER, 0, 10);

    /* ── dBm (small, below centre) ── */
    g_dbm_lbl = lv_label_create(scr);
    lv_label_set_text(g_dbm_lbl, "");
    lv_obj_set_style_text_font(g_dbm_lbl,  &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_dbm_lbl, C_DIM,                  LV_PART_MAIN);
    lv_obj_align(g_dbm_lbl, LV_ALIGN_CENTER, 0, 80);
}

/* ── update display from loop() ──────────────────────────────────────────── */
static void update_display(float proximity, uint8_t mode, float rssi) {
    if (!lvgl_mux) return;
    if (!lvgl_lock(10)) return;

    lv_arc_set_value(g_prox_arc, (int16_t)(proximity * 100.0f));

    if (mode < NUM_MODES)
        lv_label_set_text(g_mode_lbl, MODE_NAMES[mode]);

    const char *dist;
    if (!connected)          dist = "—";
    else if (proximity >= 0.75f) dist = "NEAR";
    else if (proximity >= 0.40f) dist = "CLOSE";
    else                         dist = "FAR";
    lv_label_set_text(g_dist_lbl, dist);

    if (connected)
        lv_label_set_text_fmt(g_dbm_lbl, "%.0f dBm", rssi);
    else
        lv_label_set_text(g_dbm_lbl, "");

    lvgl_unlock();
}

/* ── display + LVGL init ─────────────────────────────────────────────────── */
static void display_init(void) {
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t      disp_drv;

    /* SPI bus */
    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num    = LCD_D0_PIN;
    buscfg.data1_io_num    = LCD_D1_PIN;
    buscfg.sclk_io_num     = LCD_PCLK_PIN;
    buscfg.data2_io_num    = LCD_D2_PIN;
    buscfg.data3_io_num    = LCD_D3_PIN;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* panel IO (QSPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num         = LCD_CS_PIN;
    io_config.dc_gpio_num         = -1;
    io_config.spi_mode            = 0;
    io_config.pclk_hz             = 40 * 1000 * 1000;
    io_config.trans_queue_depth   = 10;
    io_config.on_color_trans_done = lvgl_flush_ready_cb;
    io_config.user_ctx            = &disp_drv;
    io_config.lcd_cmd_bits        = 32;
    io_config.lcd_param_bits      = 8;
    io_config.flags.quad_mode     = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                 &io_config, &io_handle));

    /* SH8601 panel */
    sh8601_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds      = sh8601_lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(sh8601_lcd_init_cmds) /
                                   sizeof(sh8601_lcd_init_cmds[0]);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RST_PIN;
    panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config  = &vendor_config;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* LVGL */
    lv_init();
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
        LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
        LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * LVGL_BUF_HEIGHT);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_H_RES;
    disp_drv.ver_res    = LCD_V_RES;
    disp_drv.flush_cb   = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf   = &disp_buf;
    disp_drv.user_data  = panel_handle;
    lv_disp_drv_register(&disp_drv);

    /* tick timer */
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lvgl_tick_cb;
    tick_args.name     = "lvgl_tick";
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(lvgl_task, "LVGL", 6 * 1024, NULL, LVGL_TASK_PRIORITY, NULL);

    if (lvgl_lock(-1)) {
        create_ui();
        lvgl_unlock();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   BLE CALLBACKS
   ═══════════════════════════════════════════════════════════════════════════ */

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer,
                   esp_ble_gatts_cb_param_t *param) override {
        memcpy(peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        ema_rssi  = -90.0f;
        connected = true;
        Serial.println("BLE connected");
    }
    void onDisconnect(BLEServer *pServer) override {
        connected = false;
        raw_rssi  = -90;
        ema_rssi  = -90.0f;
        ring.clear();
        ring.show();
        BLEDevice::startAdvertising();
        Serial.println("BLE disconnected — advertising restarted");
    }
};

static void on_gap_event(esp_gap_ble_cb_event_t  event,
                         esp_ble_gap_cb_param_t *param) {
    if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT &&
        param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        raw_rssi = param->read_rssi_cmpl.rssi;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   AUDIO MODES
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── MODE 0: Smooth drone / theremin ─────────────────────────────────────── */
static void fill_drone(float proximity) {
    if (!connected || proximity < 0.05f) {
        memset(audio_buf, 0, BUF_BYTES);
        return;
    }
    float freq     = 80.0f + proximity * 360.0f;
    float amp      = (0.15f + proximity * 0.6f) * 32767.0f;
    float inc      = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
    float trem_inc = 2.0f * (float)M_PI * 3.0f / SAMPLE_RATE;   /* 3 Hz tremolo */
    for (int i = 0; i < BUF_FRAMES; i++) {
        float   trem = 0.7f + 0.3f * sinf(drone_trem);
        int16_t s    = (int16_t)(sinf(drone_phase) * amp * trem);
        audio_buf[i * 2]     = s;
        audio_buf[i * 2 + 1] = s;
        drone_phase += inc;
        if (drone_phase > 2.0f * (float)M_PI) drone_phase -= 2.0f * (float)M_PI;
        drone_trem += trem_inc;
        if (drone_trem  > 2.0f * (float)M_PI) drone_trem  -= 2.0f * (float)M_PI;
    }
}

/* ── MODE 1: Karplus-Strong plucked string ───────────────────────────────── */
static void ks_pluck(void) {
    for (int i = 0; i < ks_size; i++)
        ks_buf[i] = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    ks_pos = 0;
}
static void fill_karplus(float proximity) {
    if (!connected || proximity < 0.05f) {
        memset(audio_buf, 0, BUF_BYTES);
        return;
    }
    /* pluck interval: 2000 ms (far) → 300 ms (close) */
    uint32_t interval = (uint32_t)(2000.0f - proximity * 1700.0f);
    if (millis() - ks_last_ms >= interval) {
        ks_pluck();
        ks_last_ms = millis();
    }
    float amp = (0.4f + proximity * 0.5f) * 32767.0f;
    for (int i = 0; i < BUF_FRAMES; i++) {
        float s    = ks_buf[ks_pos];
        int   next = (ks_pos + 1) % ks_size;
        ks_buf[ks_pos] = (ks_buf[ks_pos] + ks_buf[next]) * 0.498f;   /* low-pass avg */
        ks_pos = next;
        audio_buf[i * 2]     = (int16_t)(s * amp);
        audio_buf[i * 2 + 1] = (int16_t)(s * amp);
    }
}

/* ── MODE 2: Pink noise ──────────────────────────────────────────────────── */
static void fill_pink(float proximity) {
    if (!connected || proximity < 0.05f) {
        memset(audio_buf, 0, BUF_BYTES);
        return;
    }
    float amp = proximity * 0.75f * 32767.0f;
    for (int i = 0; i < BUF_FRAMES; i++) {
        /* leaky integrator of white noise → pink-ish spectrum */
        pink_state = pink_state * 0.99f +
                     (rand() / (float)RAND_MAX - 0.5f) * 0.2f;
        int16_t s  = (int16_t)(pink_state * amp);
        audio_buf[i * 2]     = s;
        audio_buf[i * 2 + 1] = s;
    }
}

/* ── reset per-mode state on switch ──────────────────────────────────────── */
static void on_mode_change(uint8_t new_mode) {
    current_mode = new_mode;
    drone_phase  = 0.0f;
    drone_trem   = 0.0f;
    memset(ks_buf, 0, sizeof(ks_buf));
    ks_pos       = 0;
    ks_last_ms   = 0;
    pink_state   = 0.0f;
    if (audio_buf) memset(audio_buf, 0, BUF_BYTES);
    Serial.printf("── Mode %d / %d: %s ──\n",
                  new_mode + 1, NUM_MODES, MODE_NAMES[new_mode]);
}

/* ── dispatch to active mode ─────────────────────────────────────────────── */
static void fill_audio(float proximity) {
    switch (current_mode) {
        case 0:  fill_drone(proximity);   break;
        case 1:  fill_karplus(proximity); break;
        case 2:  fill_pink(proximity);    break;
        default: memset(audio_buf, 0, BUF_BYTES); break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SETUP
   ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
    Serial.begin(115200);
    delay(2000);

    /* LEDs */
    perlin_init(esp_random());
    ring.begin();
    ring.setBrightness(255);
    ring.clear();
    ring.show();

    /* display + LVGL (SPI — independent of codec I2C) */
    display_init();

    /* codec (creates and owns I2C port 0 internally) */
    set_codec_board_type("C6_AMOLED_1_43");
    codec_init_cfg_t codec_cfg = {};
    int codec_ret = init_codec(&codec_cfg);
    Serial.printf("init_codec: %d\n", codec_ret);

    /* borrow the codec's I2C bus to reach the TCA9554 GPIO expander */
    i2c_master_bus_handle_t i2c_bus =
        (i2c_master_bus_handle_t)get_i2c_bus_handle(0);

    /* TCA9554 pin 7 enables the speaker amplifier */
    esp_io_expander_new_i2c_tca9554(i2c_bus,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
    esp_io_expander_set_dir  (io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);

    playback = get_playback_handle();
    record   = get_record_handle();
    Serial.printf("playback: %p  record: %p\n", playback, record);

    esp_codec_dev_set_out_vol(playback, 90.0f);
    esp_codec_dev_set_in_gain(record,   35.0f);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = SAMPLE_RATE;
    fs.channel         = CHANNELS;
    fs.bits_per_sample = 16;
    esp_codec_dev_open(playback, &fs);
    esp_codec_dev_open(record,   &fs);

    audio_buf = (int16_t *)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DEFAULT);
    assert(audio_buf);
    memset(audio_buf, 0, BUF_BYTES);

    /* BLE — advertise as "Intent" */
    BLEDevice::init("Intent");
    BLEDevice::setCustomGapHandler(on_gap_event);
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    BLEService *pService = pServer->createService(
        "4FAFC201-1FB5-459E-8FCC-C5C9C331914B");
    pService->start();
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID("4FAFC201-1FB5-459E-8FCC-C5C9C331914B");
    pAdv->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("Advertising as \"Intent\" — connect via NRF Connect");
    Serial.printf("Rotating through %d audio modes every %lu s\n",
                  NUM_MODES, MODE_DURATION / 1000UL);
}

/* ═══════════════════════════════════════════════════════════════════════════
   LOOP
   ═══════════════════════════════════════════════════════════════════════════ */
void loop() {
    uint32_t now = millis();

    /* ── mode rotation every 10 s ── */
    uint8_t new_mode = (uint8_t)((now / MODE_DURATION) % NUM_MODES);
    if (new_mode != current_mode) on_mode_change(new_mode);

    /* ── RSSI polling every 200 ms ── */
    static uint32_t last_rssi_ms = 0;
    if (connected && now - last_rssi_ms >= 200) {
        esp_ble_gap_read_rssi(peer_bda);
        last_rssi_ms = now;
    }

    /* ── EMA smoothing + proximity ── */
    float proximity = 0.0f;
    if (connected) {
        ema_rssi  = EMA_ALPHA * (float)raw_rssi + (1.0f - EMA_ALPHA) * ema_rssi;
        proximity = rssi_to_proximity(ema_rssi);
    }

    /* ── LEDs ── */
    if (connected) {
        float    intensity = compute_led_intensity(now / 1000.0f, proximity);
        uint32_t color     = ring.Color((uint8_t)intensity, 0, 0);
        for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, color);
        ring.show();
    }

    /* ── audio ── */
    if (audio_buf) {
        fill_audio(proximity);
        esp_codec_dev_write(playback, audio_buf, BUF_BYTES);
    }

    /* ── display update every 300 ms ── */
    static uint32_t last_disp_ms = 0;
    if (now - last_disp_ms >= 300) {
        update_display(proximity, current_mode, ema_rssi);
        last_disp_ms = now;
    }

    /* ── serial debug every 1 s ── */
    static uint32_t last_log_ms = 0;
    if (connected && now - last_log_ms >= 1000) {
        uint32_t secs_left = MODE_DURATION / 1000 -
                             (now / 1000) % (MODE_DURATION / 1000);
        Serial.printf("[%s] RSSI %4d  EMA %5.1f  prox %.2f  next in %lus\n",
                      MODE_NAMES[current_mode],
                      (int)raw_rssi, ema_rssi, proximity,
                      (unsigned long)secs_left);
        last_log_ms = now;
    }
}
