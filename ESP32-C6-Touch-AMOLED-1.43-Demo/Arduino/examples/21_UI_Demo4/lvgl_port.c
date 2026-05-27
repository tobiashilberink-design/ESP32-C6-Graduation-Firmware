/* ═══════════════════════════════════════════════════════════════════════════
   21_UI_Demo4  —  11-screen sleep-prep UI
   0 Clock  |  1 Alarm  |  2 Guide  |  3 Wind timer  |  4 Heart Coherence
   5 Connect  |  6 Music  |  7 Podcast  |  8 Background noise  |  9 Volume
   10 Reset
   ═══════════════════════════════════════════════════════════════════════════ */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "user_config.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "src/sh8601/esp_lcd_sh8601.h"
#include <math.h>

/* ── hardware ─────────────────────────────────────────────────────────────── */
#define LCD_HOST          SPI2_HOST
#define LCD_BIT_PER_PIXEL 16

/* ── colour palette ───────────────────────────────────────────────────────── */
#define C_BG    lv_color_make( 15,  4,  4)
#define C_RED   lv_color_make(220, 48, 48)
#define C_MID   lv_color_make(140, 30, 30)
#define C_DIM   lv_color_make( 65, 14, 14)
#define C_TRACK lv_color_make( 38,  9,  9)

/* ── BLE proximity thresholds ─────────────────────────────────────────────── */
#define PROX_TOO_CLOSE    0.20f /* > this → border starts showing             */
#define PROX_VERY_CLOSE   0.65f /* > this → fast-flash mode                   */
#define PROX_ALARM_BLOCK  0.25f /* rising:  alarm encoder blocks above this   */
#define PROX_ALARM_UNBLOCK 0.12f/* falling: alarm only unblocks below this    */

/* ── clock geometry ───────────────────────────────────────────────────────── */
#define CLOCK_CX     (LCD_H_RES / 2)
#define CLOCK_CY     (LCD_V_RES / 2)
#define CLOCK_R_MARK 155
#define HAND_MIN_LEN 130
#define HAND_HR_LEN   85

/* ── guide list ───────────────────────────────────────────────────────────── */
#define GUIDE_ITEM_SPACING  52    /* px between list item centres */
#define GUIDE_LIST_X        238   /* left edge of list items      */
#define GUIDE_LIST_W        212   /* width of list items          */
#define GUIDE_LIST_CY       (LCD_V_RES / 2 - 12)   /* centre y for selected item */

static const char *GUIDE_NAMES[GUIDE_COUNT] = {
    "Im good",
    "Heart Coherence",
    "Connect",
    "Background noise",
    "Music",
    "Podcast"
};

/* ── conversation card deck ───────────────────────────────────────────────── */
static const char *CONNECT_CARDS[] = {
    "What was the best\npart of your day?",
    "What are you looking\nforward to tomorrow?",
    "What made you\nsmile today?",
    "Is there anything on\nyour mind you'd like\nto share?",
    "What are you grateful\nfor today?",
    "What's one thing you\nappreciated about\nme today?",
    "If today were a movie,\nwhat would it be called?",
    "What's one thing you'd\nlike to do together soon?",
    "What made you feel\nloved today?",
    "What's something\nyou learned today?",
    "What would have made\ntoday even better?",
    "One word to describe\nyour day?",
    "What are you leaving\nbehind as we sleep?",
    "What do you want to\nremember about today?",
    "Is there anything\nyou need from me\nright now?"
};
#define CONNECT_CARD_COUNT  ((int)(sizeof(CONNECT_CARDS) / sizeof(CONNECT_CARDS[0])))

/* ── app state ────────────────────────────────────────────────────────────── */
static int            clock_total_min =  8 * 60;   /* default 08:00 */
static int            alarm_total_min =  7 * 60;
static int            volume_val      = 50;
static int            active_screen   = 0;

/* ── guide state ──────────────────────────────────────────────────────────── */
static guide_choice_t guide_choice      = GUIDE_IM_GOOD;
static int            guide_sel         = 0;
static int            guide_enc_acc     = 0;   /* accumulator: scroll 1 item per 4 pulses */
static int            connect_card_idx  = 0;
static int            connect_enc_acc   = 0;   /* accumulator: scroll 1 card per 4 pulses */

/* ── wind-down state ──────────────────────────────────────────────────────── */
static wind_state_t wind_state    = WIND_SELECTING;
static int          wind_minutes  = 10;
static int          wind_n_lit    = 4;
static int64_t      wind_enter_us = 0;
static int64_t      wind_start_us = 0;

/* ── Current Time Service (CTS) state ────────────────────────────────────── */
static bool    cts_active   = false;
static int64_t cts_sync_us  = 0;    /* esp_timer timestamp of last sync     */
static int     cts_sync_min = 0;    /* clock_total_min value at last sync   */

/* ── breath animation state ───────────────────────────────────────────────── */
static bool          g_breath_in_phase = true;

/* ── BLE proximity state ──────────────────────────────────────────────────── */
static float     g_ble_proximity     = 0.0f;
static lv_obj_t *g_prox_border       = NULL;
static lv_obj_t *g_alarm_blocked_lbl = NULL;
static float     g_prox_phase        = 0.0f;
static bool      alarm_blocked       = false;  /* hysteresis state           */

/* ── LVGL handles ─────────────────────────────────────────────────────────── */
static SemaphoreHandle_t lvgl_mux   = NULL;
static lv_obj_t         *g_tileview = NULL;
static lv_obj_t         *g_tiles[NUM_SCREENS];
static lv_obj_t         *g_dots[NUM_SCREENS];

/* clock */
static lv_obj_t   *g_hour_hand;
static lv_obj_t   *g_min_hand;
static lv_point_t  g_hour_pts[2];
static lv_point_t  g_min_pts[2];

/* alarm */
static lv_obj_t *g_alarm_lbl;

/* guide list */
static lv_obj_t *g_guide_items[GUIDE_COUNT];

/* wind-down */
static lv_obj_t *g_wind_arc;
static lv_obj_t *g_wind_min_lbl;
static lv_obj_t *g_wind_min_sublbl;
static lv_obj_t *g_wind_question;

/* heart coherence */
static lv_obj_t    *g_breath_in_lbl;
static lv_obj_t    *g_breath_out_lbl;
static lv_timer_t  *g_breath_timer  = NULL;

/* connect / conversation cards */
static lv_obj_t    *g_connect_lbl;
static lv_obj_t    *g_connect_num_lbl;

/* music */
static lv_obj_t   *g_music_dots[3];
static lv_timer_t *g_music_timer = NULL;

/* volume */
static lv_obj_t *g_vol_arc;
static lv_obj_t *g_vol_lbl;

/* done label per screen (NULL where unused) */
static lv_obj_t *g_done_lbl[NUM_SCREENS];

/* ── display handle ───────────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t amoled_panel_io_handle = NULL;
static lv_disp_t                *g_disp                 = NULL;

/* ── touch handle ─────────────────────────────────────────────────────────── */
static i2c_master_dev_handle_t   disp_touch_dev_handle  = NULL;

/* ── forward declarations ─────────────────────────────────────────────────── */
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *, void *);
static void example_lvgl_flush_cb(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *, lv_area_t *);
static void example_increase_lvgl_tick(void *);
static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
static void example_lvgl_port_task(void *);
static void create_ui(void);
static void tileview_event_cb(lv_event_t *);
static void update_clock_hands(void);
static void navigate_to(int idx);
static void reset_wind_screen(void);
static void update_guide_list_anim(void);
static void breath_timer_cb(lv_timer_t *);
static void music_timer_cb(lv_timer_t *);
static void guide_anim_y_cb(void *obj, int32_t val);
static void create_connect_screen(lv_obj_t *tile);
static void create_reset_screen(lv_obj_t *tile);
static void do_software_reset(void);
static void prox_timer_cb(lv_timer_t *);
static void cts_clock_tick(void);
static void example_lvgl_touch_cb(lv_indev_drv_t *, lv_indev_data_t *);

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

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

void lvgl_port_init(void)
{
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t      disp_drv;

    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num    = LCD_D0_PIN;
    buscfg.data1_io_num    = LCD_D1_PIN;
    buscfg.sclk_io_num     = LCD_PCLK_PIN;
    buscfg.data2_io_num    = LCD_D2_PIN;
    buscfg.data3_io_num    = LCD_D3_PIN;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num         = LCD_CS_PIN;
    io_config.dc_gpio_num         = -1;
    io_config.spi_mode            = 0;
    io_config.pclk_hz             = 40 * 1000 * 1000;
    io_config.trans_queue_depth   = 10;
    io_config.on_color_trans_done = example_notify_lvgl_flush_ready;
    io_config.user_ctx            = &disp_drv;
    io_config.lcd_cmd_bits        = 32;
    io_config.lcd_param_bits      = 8;
    io_config.flags.quad_mode     = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    amoled_panel_io_handle = io_handle;

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

    /* no touch input — codec owns I2C */

    lv_init();
    lv_color_t *buf1 = heap_caps_malloc(
        LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(
        LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * LVGL_BUF_HEIGHT);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_H_RES;
    disp_drv.ver_res    = LCD_V_RES;
    disp_drv.flush_cb   = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf   = &disp_buf;
    disp_drv.user_data  = panel_handle;
    g_disp = lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &example_increase_lvgl_tick;
    tick_args.name     = "lvgl_tick";
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE,
                NULL, LVGL_TASK_PRIORITY, NULL);

    if (example_lvgl_lock(-1)) {
        create_ui();
        example_lvgl_unlock();
    }
}

/* ── getters ──────────────────────────────────────────────────────────────── */
int            get_active_screen(void)  { return active_screen; }
wind_state_t   get_wind_state(void)     { return wind_state; }
guide_choice_t get_guide_choice(void)   { return guide_choice; }
int            get_wind_minutes(void)   { return wind_minutes; }
int            get_wind_n_lit(void)     { return wind_n_lit; }
int            get_volume_val(void)     { return volume_val; }
int64_t        get_wind_enter_us(void)  { return wind_enter_us; }
int64_t        get_wind_start_us(void)  { return wind_start_us; }

/* ── BLE proximity setter (called from Arduino loop at ~5 Hz) ─────────────── */
void lvgl_set_ble_proximity(float proximity) { g_ble_proximity = proximity; }

/* ── CTS time sync — called once per BLE CTS write ───────────────────────── */
void lvgl_set_cts_time(int hours, int minutes)
{
    /* Called from BLE task — use a generous timeout so we don't silently bail
       if the LVGL task happens to hold the mutex at this moment.            */
    if (!example_lvgl_lock(200)) return;
    clock_total_min = (hours * 60 + minutes) % 1440;
    cts_sync_us     = esp_timer_get_time();
    cts_sync_min    = clock_total_min;
    cts_active      = true;
    update_clock_hands();
    example_lvgl_unlock();
}

/* ── timer tick — call from Arduino loop() ────────────────────────────────── */
void timer_update_tick(void)
{
    if (!lvgl_mux) return;

    /* ── CTS real-time clock tick ── */
    cts_clock_tick();

    /* ── wind-down timer expiry ── */
    if (wind_state != WIND_RUNNING) return;

    int64_t elapsed = esp_timer_get_time() - wind_start_us;
    int64_t total   = (int64_t)wind_minutes * 60LL * 1000000LL;

    if (elapsed >= total) {
        if (!example_lvgl_lock(5)) return;
        wind_state = WIND_DONE;

        /* pause breath animation */
        if (g_breath_timer) lv_timer_pause(g_breath_timer);

        /* show "Sleep well" on whichever content screen is active */
        int show = (guide_choice == GUIDE_IM_GOOD) ? SCR_WIND : active_screen;
        if (show >= 0 && show < NUM_SCREENS && g_done_lbl[show]) {
            lv_obj_clear_flag(g_done_lbl[show], LV_OBJ_FLAG_HIDDEN);
            lv_obj_fade_in (g_done_lbl[show], 2000,      0);
            lv_obj_fade_out(g_done_lbl[show], 8000, 120000);
        }
        example_lvgl_unlock();
    }
}

/* advance the clock by whole elapsed minutes since the last CTS anchor */
static void cts_clock_tick(void)
{
    if (!cts_active) return;

    int64_t elapsed_min = (esp_timer_get_time() - cts_sync_us) / 60000000LL;
    int new_min = (int)((cts_sync_min + elapsed_min) % 1440);
    if (new_min == clock_total_min) return;   /* nothing to update yet */

    if (!example_lvgl_lock(5)) return;
    clock_total_min = new_min;
    update_clock_hands();
    example_lvgl_unlock();
}

/* ── encoder delta ────────────────────────────────────────────────────────── */
void on_encoder_delta(int delta)
{
    if (!lvgl_mux) return;
    if (!example_lvgl_lock(10)) return;

    switch (active_screen) {

        case SCR_CLOCK:
            clock_total_min = ((clock_total_min + delta) % 1440 + 1440) % 1440;
            if (cts_active) {
                /* re-anchor so ticking continues from the manually-adjusted position */
                cts_sync_us  = esp_timer_get_time();
                cts_sync_min = clock_total_min;
            }
            update_clock_hands();
            break;

        case SCR_ALARM:
            if (alarm_blocked) break;   /* phone too close — hysteresis state */
            alarm_total_min = ((alarm_total_min + delta) % 1440 + 1440) % 1440;
            lv_label_set_text_fmt(g_alarm_lbl, "%02d:%02d",
                alarm_total_min / 60, alarm_total_min % 60);
            break;

        case SCR_GUIDE:
            guide_enc_acc += delta;
            while (guide_enc_acc >=  4) { guide_enc_acc -= 4; guide_sel++; }
            while (guide_enc_acc <= -4) { guide_enc_acc += 4; guide_sel--; }
            if (guide_sel < 0)                guide_sel = 0;
            if (guide_sel >= (int)GUIDE_COUNT) guide_sel = GUIDE_COUNT - 1;
            update_guide_list_anim();
            break;

        case SCR_CONNECT:
            connect_enc_acc += delta;
            while (connect_enc_acc >=  4) { connect_enc_acc -= 4; connect_card_idx++; }
            while (connect_enc_acc <= -4) { connect_enc_acc += 4; connect_card_idx--; }
            if (connect_card_idx < 0)                   connect_card_idx = 0;
            if (connect_card_idx >= CONNECT_CARD_COUNT) connect_card_idx = CONNECT_CARD_COUNT - 1;
            lv_label_set_text(g_connect_lbl, CONNECT_CARDS[connect_card_idx]);
            lv_label_set_text_fmt(g_connect_num_lbl, "%d / %d",
                connect_card_idx + 1, CONNECT_CARD_COUNT);
            break;

        case SCR_WIND:
            if (wind_state == WIND_SELECTING) {
                wind_minutes += delta;
                if (wind_minutes < 10) wind_minutes = 10;
                if (wind_minutes > 60) wind_minutes = 60;
                lv_arc_set_value(g_wind_arc, wind_minutes);
                lv_label_set_text_fmt(g_wind_min_lbl, "%d", wind_minutes);
            }
            break;

        case SCR_VOLUME:
            volume_val += delta;
            if (volume_val <   0) volume_val =   0;
            if (volume_val > 100) volume_val = 100;
            lv_arc_set_value(g_vol_arc, volume_val);
            lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
            break;

        default:
            break;
    }

    example_lvgl_unlock();
}

/* ── button press ─────────────────────────────────────────────────────────── */
void on_button_press(void)
{
    if (!lvgl_mux) return;

    static int64_t last_btn_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_btn_us < 500000LL) return;
    last_btn_us = now_us;

    if (!example_lvgl_lock(10)) return;

    if (active_screen == SCR_CLOCK) {
        navigate_to(SCR_ALARM);

    } else if (active_screen == SCR_ALARM) {
        navigate_to(SCR_GUIDE);

    } else if (active_screen == SCR_GUIDE) {
        /* confirm choice and proceed to wind-down timer */
        guide_choice = (guide_choice_t)guide_sel;
        navigate_to(SCR_WIND);

    } else if (active_screen == SCR_WIND) {
        if (wind_state == WIND_SELECTING) {
            /* compute LED count for .ino */
            wind_n_lit = (int)roundf((float)wind_minutes / 60.0f * 24.0f);
            if (wind_n_lit > 24) wind_n_lit = 24;
            if (wind_n_lit <  0) wind_n_lit = 0;

            /* fade out wind-timer UI (arc/question/labels) */
            lv_obj_fade_out(g_wind_arc,       4000, 0);
            lv_obj_fade_out(g_wind_question,  4000, 0);
            lv_obj_fade_out(g_wind_min_lbl,   4000, 0);
            lv_obj_fade_out(g_wind_min_sublbl, 4000, 0);

            wind_state    = WIND_RUNNING;
            wind_start_us = esp_timer_get_time();

            /* navigate based on guided choice */
            switch (guide_choice) {
                case GUIDE_HEART_COHERENCE:
                    /* kick off breath animation */
                    g_breath_in_phase = true;
                    lv_obj_set_style_opa(g_breath_in_lbl,  LV_OPA_TRANSP, LV_PART_MAIN);
                    lv_obj_set_style_opa(g_breath_out_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
                    lv_obj_fade_in(g_breath_in_lbl, 1000, 0);
                    lv_timer_reset(g_breath_timer);
                    lv_timer_resume(g_breath_timer);
                    navigate_to(SCR_HEART);
                    break;
                case GUIDE_CONVERSATION:
                    /* reset to first card and navigate */
                    connect_card_idx = 0;
                    lv_label_set_text(g_connect_lbl, CONNECT_CARDS[0]);
                    lv_label_set_text_fmt(g_connect_num_lbl, "1 / %d", CONNECT_CARD_COUNT);
                    navigate_to(SCR_CONNECT);
                    break;
                case GUIDE_MUSIC:
                    navigate_to(SCR_MUSIC);
                    break;
                case GUIDE_PODCAST:
                    navigate_to(SCR_PODCAST);
                    break;
                case GUIDE_BACKGROUND_NOISE:
                    navigate_to(SCR_BGNOISE);
                    break;
                case GUIDE_IM_GOOD:
                default:
                    /* stay on SCR_WIND — campfire LEDs + arc countdown */
                    break;
            }

        } else if (wind_state == WIND_DONE) {
            reset_wind_screen();
            navigate_to(SCR_GUIDE);
        }

    } else if (active_screen == SCR_HEART   ||
               active_screen == SCR_CONNECT ||
               active_screen == SCR_MUSIC   ||
               active_screen == SCR_PODCAST ||
               active_screen == SCR_BGNOISE) {
        if (wind_state == WIND_DONE) {
            reset_wind_screen();
            navigate_to(SCR_GUIDE);
        }
    } else if (active_screen == SCR_RESET) {
        do_software_reset();
    }
    /* SCR_VOLUME: button does nothing */

    example_lvgl_unlock();
}

/* ═══════════════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

static void navigate_to(int idx)
{
    active_screen = idx;
    lv_obj_scroll_to(g_tileview, (lv_coord_t)idx * LCD_H_RES, 0, LV_ANIM_ON);

    /* record wind-screen entry time for LED fade-in */
    if (idx == SCR_WIND && wind_state == WIND_SELECTING)
        wind_enter_us = esp_timer_get_time();

    /* update page dots */
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (!g_dots[i]) continue;
        lv_obj_set_style_bg_color(g_dots[i],
            i == idx ? C_RED : C_DIM, LV_PART_MAIN);
    }
}

static void reset_wind_screen(void)
{
    /* cancel fade animations on wind-timer widgets */
    lv_anim_del(g_wind_arc,        NULL);
    lv_anim_del(g_wind_question,   NULL);
    lv_anim_del(g_wind_min_lbl,    NULL);
    lv_anim_del(g_wind_min_sublbl, NULL);

    /* restore arc + labels to full opacity */
    lv_obj_set_style_opa(g_wind_arc,        LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_question,   LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_min_lbl,    LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_min_sublbl, LV_OPA_COVER, LV_PART_MAIN);

    /* hide all done labels */
    for (int i = 0; i < NUM_SCREENS; i++) {
        if (!g_done_lbl[i]) continue;
        lv_anim_del(g_done_lbl[i], NULL);
        lv_obj_add_flag(g_done_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(g_done_lbl[i], LV_OPA_TRANSP, LV_PART_MAIN);
    }

    /* stop breath animation */
    if (g_breath_timer) lv_timer_pause(g_breath_timer);
    if (g_breath_in_lbl)  lv_obj_set_style_opa(g_breath_in_lbl,  LV_OPA_TRANSP, LV_PART_MAIN);
    if (g_breath_out_lbl) lv_obj_set_style_opa(g_breath_out_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    g_breath_in_phase = true;

    /* reset wind state */
    wind_state    = WIND_SELECTING;
    wind_minutes  = 10;
    wind_n_lit    = 4;
    wind_enter_us = esp_timer_get_time();
    lv_arc_set_value(g_wind_arc, 10);
    lv_label_set_text(g_wind_min_lbl, "10");
}

/* ── guide list smooth scroll animation ──────────────────────────────────── */
static void guide_anim_y_cb(void *obj, int32_t val)
{
    lv_obj_set_y((lv_obj_t *)obj, val);
}

static void update_guide_list_anim(void)
{
    for (int i = 0; i < (int)GUIDE_COUNT; i++) {
        int dist       = i - guide_sel;
        int target_y   = GUIDE_LIST_CY + dist * GUIDE_ITEM_SPACING;
        int current_y  = lv_obj_get_y(g_guide_items[i]);

        /* animate y position */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_guide_items[i]);
        lv_anim_set_exec_cb(&a, guide_anim_y_cb);
        lv_anim_set_values(&a, current_y, target_y);
        lv_anim_set_time(&a, 800);   /* 4× slower than v3 for smoother feel */
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);

        /* opacity by distance from selected */
        int abs_dist = dist < 0 ? -dist : dist;
        lv_opa_t opa = (abs_dist == 0) ? LV_OPA_COVER :
                       (abs_dist == 1) ? LV_OPA_70    : LV_OPA_40;
        lv_obj_set_style_opa(g_guide_items[i], opa, LV_PART_MAIN);
    }
}

/* ── breath timer callback — fires every 5 s ─────────────────────────────── */
static void breath_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (wind_state != WIND_RUNNING) return;

    g_breath_in_phase = !g_breath_in_phase;

    if (g_breath_in_phase) {
        lv_obj_set_style_opa(g_breath_in_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_fade_in (g_breath_in_lbl,  1000, 0);
        lv_obj_fade_out(g_breath_out_lbl, 800,  0);
    } else {
        lv_obj_set_style_opa(g_breath_out_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_fade_in (g_breath_out_lbl, 1000, 0);
        lv_obj_fade_out(g_breath_in_lbl,  800,  0);
    }
}

/* ── music dot cycling timer — fires every 400 ms ────────────────────────── */
static void music_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int idx = 0;
    idx = (idx + 1) % 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_opa(g_music_dots[i],
            (i == idx) ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
    }
}

/* ── clock hands ──────────────────────────────────────────────────────────── */
static void update_clock_hands(void)
{
    int   h   = clock_total_min / 60;
    int   m   = clock_total_min % 60;
    float ma  = ((float)m / 60.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
    float ha  = ((float)(h % 12) / 12.0f + (float)m / 720.0f) * 2.0f * (float)M_PI
                - (float)M_PI / 2.0f;

    g_min_pts[0].x = CLOCK_CX;
    g_min_pts[0].y = CLOCK_CY;
    g_min_pts[1].x = CLOCK_CX + (int)(HAND_MIN_LEN * cosf(ma));
    g_min_pts[1].y = CLOCK_CY + (int)(HAND_MIN_LEN * sinf(ma));
    lv_line_set_points(g_min_hand, g_min_pts, 2);

    g_hour_pts[0].x = CLOCK_CX;
    g_hour_pts[0].y = CLOCK_CY;
    g_hour_pts[1].x = CLOCK_CX + (int)(HAND_HR_LEN * cosf(ha));
    g_hour_pts[1].y = CLOCK_CY + (int)(HAND_HR_LEN * sinf(ha));
    lv_line_set_points(g_hour_hand, g_hour_pts, 2);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SCREEN CREATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* helper: style a tile with the dark background */
static void style_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
}

/* helper: create a "Sleep well" done label, hidden by default */
static lv_obj_t *make_done_label(lv_obj_t *tile)
{
    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, "Sleep well");
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    return lbl;
}

/* ── screen 0: analog clock ───────────────────────────────────────────────── */
static void create_clock_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* face circle */
    lv_obj_t *face = lv_obj_create(tile);
    lv_obj_set_size(face, CLOCK_R_MARK * 2, CLOCK_R_MARK * 2);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(face, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(face, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
    lv_obj_center(face);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* hour markers */
    for (int i = 0; i < 12; i++) {
        float angle = (float)i * 30.0f * (float)M_PI / 180.0f - (float)M_PI / 2.0f;
        int   x     = CLOCK_CX + (int)(CLOCK_R_MARK * cosf(angle));
        int   y     = CLOCK_CY + (int)(CLOCK_R_MARK * sinf(angle));
        int   sz    = (i % 3 == 0) ? 8 : 4;
        lv_obj_t *dot = lv_obj_create(tile);
        lv_obj_set_size(dot, sz, sz);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, (i % 3 == 0) ? C_MID : C_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_pos(dot, x - sz / 2, y - sz / 2);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    g_min_hand = lv_line_create(tile);
    lv_obj_set_style_line_color(g_min_hand, C_RED, LV_PART_MAIN);
    lv_obj_set_style_line_width(g_min_hand, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(g_min_hand, true, LV_PART_MAIN);

    g_hour_hand = lv_line_create(tile);
    lv_obj_set_style_line_color(g_hour_hand, C_RED, LV_PART_MAIN);
    lv_obj_set_style_line_width(g_hour_hand, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(g_hour_hand, true, LV_PART_MAIN);

    lv_obj_t *cdot = lv_obj_create(tile);
    lv_obj_set_size(cdot, 12, 12);
    lv_obj_set_style_radius(cdot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cdot, C_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cdot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cdot, 0, LV_PART_MAIN);
    lv_obj_align(cdot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(cdot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    update_clock_hands();
}

/* ── screen 1: alarm time ─────────────────────────────────────────────────── */
static void create_alarm_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "A L A R M");
    lv_obj_set_style_text_font(title,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID,                  LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    g_alarm_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_alarm_lbl, "%02d:%02d",
        alarm_total_min / 60, alarm_total_min % 60);
    lv_obj_set_style_text_font(g_alarm_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_alarm_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(g_alarm_lbl, LV_ALIGN_CENTER, 0, -20);

    /* phone-too-close warning — hidden until BLE proximity > threshold */
    g_alarm_blocked_lbl = lv_label_create(tile);
    lv_label_set_text(g_alarm_blocked_lbl,
        "Your phone is too\nclose to set your alarm.");
    lv_obj_set_style_text_font(g_alarm_blocked_lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_alarm_blocked_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_set_style_text_align(g_alarm_blocked_lbl, LV_TEXT_ALIGN_CENTER,   LV_PART_MAIN);
    lv_obj_align(g_alarm_blocked_lbl, LV_ALIGN_CENTER, 0, 54);
    lv_obj_add_flag(g_alarm_blocked_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ── screen 2: guided wind-down choice ───────────────────────────────────── */
static void create_guide_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* question text — left half */
    lv_obj_t *q = lv_label_create(tile);
    lv_label_set_text(q, "Do you want a\nguided\nwind down?");
    lv_obj_set_style_text_font(q,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(q, C_MID,                  LV_PART_MAIN);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_LEFT,     LV_PART_MAIN);
    lv_obj_set_width(q, 185);
    lv_obj_align(q, LV_ALIGN_LEFT_MID, 50, 0);

    /* selection highlight bar — fixed at centre of list */
    lv_obj_t *sel_bg = lv_obj_create(tile);
    lv_obj_set_size(sel_bg, GUIDE_LIST_W + 4, 38);
    lv_obj_set_style_bg_color(sel_bg, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sel_bg, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(sel_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sel_bg, 6, LV_PART_MAIN);
    lv_obj_set_pos(sel_bg, GUIDE_LIST_X - 2, GUIDE_LIST_CY - 7);
    lv_obj_clear_flag(sel_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* list items */
    for (int i = 0; i < (int)GUIDE_COUNT; i++) {
        lv_obj_t *item = lv_label_create(tile);
        lv_label_set_text(item, GUIDE_NAMES[i]);
        lv_obj_set_style_text_font(item,  &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(item, C_RED,                  LV_PART_MAIN);
        lv_obj_set_width(item, GUIDE_LIST_W);
        lv_obj_set_x(item, GUIDE_LIST_X);
        /* initial y positions with guide_sel = 0 */
        lv_obj_set_y(item, GUIDE_LIST_CY + (i - guide_sel) * GUIDE_ITEM_SPACING);
        /* initial opacity */
        int abs_dist = i < 0 ? -i : i;   /* dist from sel=0 */
        lv_opa_t opa = (i == 0)       ? LV_OPA_COVER :
                       (abs_dist == 1) ? LV_OPA_70    : LV_OPA_40;
        lv_obj_set_style_opa(item, opa, LV_PART_MAIN);
        g_guide_items[i] = item;
    }
}

/* ── screen 3: wind-down timer ────────────────────────────────────────────── */
static void create_wind_screen(lv_obj_t *tile)
{
    style_tile(tile);

    g_wind_question = lv_label_create(tile);
    lv_label_set_text(g_wind_question, "how long do you\nwant to wind down?");
    lv_obj_set_style_text_font(g_wind_question,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_question, C_MID,                  LV_PART_MAIN);
    lv_obj_set_style_text_align(g_wind_question, LV_TEXT_ALIGN_CENTER,   LV_PART_MAIN);
    lv_obj_align(g_wind_question, LV_ALIGN_CENTER, 0, -88);

    g_wind_arc = lv_arc_create(tile);
    lv_obj_set_size(g_wind_arc, 390, 390);
    lv_obj_center(g_wind_arc);
    lv_arc_set_mode(g_wind_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_wind_arc, 0, 60);
    lv_arc_set_value(g_wind_arc, 10);
    lv_arc_set_bg_angles(g_wind_arc, 0, 360);
    lv_arc_set_rotation(g_wind_arc, 270);
    lv_obj_set_style_arc_color(g_wind_arc,   C_RED,         LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_wind_arc,   22,            LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_wind_arc, true,          LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_wind_arc,   C_TRACK,       LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_wind_arc,   22,            LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wind_arc,      LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wind_arc,      LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_wind_arc,     0,             LV_PART_KNOB);
    lv_obj_clear_flag(g_wind_arc, LV_OBJ_FLAG_CLICKABLE);

    g_wind_min_lbl = lv_label_create(tile);
    lv_label_set_text(g_wind_min_lbl, "10");
    lv_obj_set_style_text_font(g_wind_min_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_min_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(g_wind_min_lbl, LV_ALIGN_CENTER, 0, -16);

    g_wind_min_sublbl = lv_label_create(tile);
    lv_label_set_text(g_wind_min_sublbl, "min");
    lv_obj_set_style_text_font(g_wind_min_sublbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_min_sublbl, C_MID,                  LV_PART_MAIN);
    lv_obj_align(g_wind_min_sublbl, LV_ALIGN_CENTER, 0, 28);

    g_done_lbl[SCR_WIND] = make_done_label(tile);
}

/* ── screen 4: heart coherence ────────────────────────────────────────────── */
static void create_heart_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* "Breathe in" — starts transparent, fade-in when wind starts */
    g_breath_in_lbl = lv_label_create(tile);
    lv_label_set_text(g_breath_in_lbl, "Breathe in");
    lv_obj_set_style_text_font(g_breath_in_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_breath_in_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(g_breath_in_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(g_breath_in_lbl, LV_OPA_TRANSP, LV_PART_MAIN);

    /* "Breathe out" — starts transparent */
    g_breath_out_lbl = lv_label_create(tile);
    lv_label_set_text(g_breath_out_lbl, "Breathe out");
    lv_obj_set_style_text_font(g_breath_out_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_breath_out_lbl, C_MID,                  LV_PART_MAIN);
    lv_obj_align(g_breath_out_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(g_breath_out_lbl, LV_OPA_TRANSP, LV_PART_MAIN);

    g_done_lbl[SCR_HEART] = make_done_label(tile);

    /* breath timer — 5 s interval, starts paused */
    g_breath_timer = lv_timer_create(breath_timer_cb, 5000, NULL);
    lv_timer_pause(g_breath_timer);
}

/* ── screen 5: connect (conversation cards) ───────────────────────────────── */
static void create_connect_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* header */
    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "C O N N E C T");
    lv_obj_set_style_text_font(title,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID,                  LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* prompt label — wraps automatically */
    g_connect_lbl = lv_label_create(tile);
    lv_label_set_text(g_connect_lbl, CONNECT_CARDS[0]);
    lv_obj_set_style_text_font(g_connect_lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_connect_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_set_style_text_align(g_connect_lbl, LV_TEXT_ALIGN_CENTER,   LV_PART_MAIN);
    lv_label_set_long_mode(g_connect_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_connect_lbl, 360);
    lv_obj_align(g_connect_lbl, LV_ALIGN_CENTER, 0, 0);

    /* card counter */
    g_connect_num_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_connect_num_lbl, "1 / %d", CONNECT_CARD_COUNT);
    lv_obj_set_style_text_font(g_connect_num_lbl,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_connect_num_lbl, C_DIM,                  LV_PART_MAIN);
    lv_obj_align(g_connect_num_lbl, LV_ALIGN_BOTTOM_MID, 0, -60);

    g_done_lbl[SCR_CONNECT] = make_done_label(tile);
}

/* ── screen 6: music ──────────────────────────────────────────────────────── */
static void create_music_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, "Music is playing");
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, C_MID,                  LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);

    /* 3 animated dots */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(tile);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, C_RED, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, i == 0 ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_CENTER, (i - 1) * 28, 20);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        g_music_dots[i] = dot;
    }

    g_done_lbl[SCR_MUSIC] = make_done_label(tile);

    /* music dot timer — always running */
    g_music_timer = lv_timer_create(music_timer_cb, 400, NULL);
}

/* ── screen 7: podcast ────────────────────────────────────────────────────── */
static void create_podcast_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "Nerdland Podcast");
    lv_obj_set_style_text_font(title,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID,                  LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sub = lv_label_create(tile);
    lv_label_set_text(sub, "November");
    lv_obj_set_style_text_font(sub,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, C_DIM,                  LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 15);

    g_done_lbl[SCR_PODCAST] = make_done_label(tile);
}

/* ── screen 8: background noise ───────────────────────────────────────────── */
static void create_bgnoise_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, "Background noise");
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, C_MID,                  LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sub = lv_label_create(tile);
    lv_label_set_text(sub, "Wind");
    lv_obj_set_style_text_font(sub,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, C_DIM,                  LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 30);

    g_done_lbl[SCR_BGNOISE] = make_done_label(tile);
}

/* ── screen 10: reset ─────────────────────────────────────────────────────── */
static void create_reset_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, "Press to reset");
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

/* ── screen 9: volume ─────────────────────────────────────────────────────── */
static void create_volume_screen(lv_obj_t *tile)
{
    style_tile(tile);

    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "V O L U M E");
    lv_obj_set_style_text_font(title,  &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID,                  LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -88);

    g_vol_arc = lv_arc_create(tile);
    lv_obj_set_size(g_vol_arc, 350, 350);
    lv_obj_center(g_vol_arc);
    lv_arc_set_mode(g_vol_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_vol_arc, 0, 100);
    lv_arc_set_value(g_vol_arc, volume_val);
    lv_arc_set_bg_angles(g_vol_arc, 135, 45);
    lv_obj_set_style_arc_color(g_vol_arc,   C_RED,         LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_vol_arc,   22,            LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_vol_arc, true,          LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_vol_arc,   C_TRACK,       LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_vol_arc,   22,            LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc,      LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc,      LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_vol_arc,     0,             LV_PART_KNOB);
    lv_obj_clear_flag(g_vol_arc, LV_OBJ_FLAG_CLICKABLE);

    g_vol_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
    lv_obj_set_style_text_font(g_vol_lbl,  &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_vol_lbl, C_RED,                  LV_PART_MAIN);
    lv_obj_align(g_vol_lbl, LV_ALIGN_CENTER, 0, 72);

    lv_obj_t *pct = lv_label_create(tile);
    lv_label_set_text(pct, "%");
    lv_obj_set_style_text_font(pct,  &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(pct, C_MID,                  LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, 118);
}

/* ── tileview event (handles programmatic scroll → dot update) ───────────── */
static void tileview_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *tv  = lv_event_get_target(e);
    int prev      = active_screen;
    int pos       = lv_obj_get_scroll_x(tv) / LCD_H_RES;
    if (pos < 0) pos = 0;
    if (pos >= NUM_SCREENS) pos = NUM_SCREENS - 1;
    active_screen = pos;

    /* record entry time when swiping onto wind screen */
    if (pos == SCR_WIND && prev != SCR_WIND && wind_state == WIND_SELECTING)
        wind_enter_us = esp_timer_get_time();

    for (int i = 0; i < NUM_SCREENS; i++) {
        if (!g_dots[i]) continue;
        lv_obj_set_style_bg_color(g_dots[i],
            i == pos ? C_RED : C_DIM, LV_PART_MAIN);
    }
}

/* ── assemble all screens ─────────────────────────────────────────────────── */
static void create_ui(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), C_BG, LV_PART_MAIN);

    g_tileview = lv_tileview_create(lv_scr_act());
    lv_obj_set_size(g_tileview, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(g_tileview, 0, 0);
    lv_obj_set_style_bg_color(g_tileview, C_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(g_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_tileview, tileview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    g_tiles[SCR_CLOCK]   = lv_tileview_add_tile(g_tileview, 0, 0, LV_DIR_RIGHT);
    g_tiles[SCR_ALARM]   = lv_tileview_add_tile(g_tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_GUIDE]   = lv_tileview_add_tile(g_tileview, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_WIND]    = lv_tileview_add_tile(g_tileview, 3, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_HEART]   = lv_tileview_add_tile(g_tileview, 4, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_CONNECT] = lv_tileview_add_tile(g_tileview, 5, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_MUSIC]   = lv_tileview_add_tile(g_tileview, 6, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_PODCAST] = lv_tileview_add_tile(g_tileview, 7, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_BGNOISE] = lv_tileview_add_tile(g_tileview, 8, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_VOLUME]  = lv_tileview_add_tile(g_tileview, 9, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[SCR_RESET]   = lv_tileview_add_tile(g_tileview, 10, 0, LV_DIR_LEFT);

    create_clock_screen  (g_tiles[SCR_CLOCK]);
    create_alarm_screen  (g_tiles[SCR_ALARM]);
    create_guide_screen  (g_tiles[SCR_GUIDE]);
    create_wind_screen   (g_tiles[SCR_WIND]);
    create_heart_screen  (g_tiles[SCR_HEART]);
    create_connect_screen(g_tiles[SCR_CONNECT]);
    create_music_screen  (g_tiles[SCR_MUSIC]);
    create_podcast_screen(g_tiles[SCR_PODCAST]);
    create_bgnoise_screen(g_tiles[SCR_BGNOISE]);
    create_volume_screen (g_tiles[SCR_VOLUME]);
    create_reset_screen  (g_tiles[SCR_RESET]);

    /* page dots — 11 small dots at bottom */
    lv_obj_t *dot_row = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(dot_row, 6, LV_PART_MAIN);
    lv_obj_set_size(dot_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_set_layout(dot_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NUM_SCREENS; i++) {
        g_dots[i] = lv_obj_create(dot_row);
        lv_obj_set_size(g_dots[i], 6, 6);
        lv_obj_set_style_radius(g_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_dots[i], i == 0 ? C_RED : C_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_dots[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(g_dots[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(g_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* ── BLE proximity border — full-screen circular ring, on top of everything ── */
    /* 466 px / 36 mm ≈ 12.9 px per mm → 12 px ≈ 1 mm border                      */
    g_prox_border = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_prox_border, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(g_prox_border, 0, 0);
    lv_obj_set_style_bg_opa(g_prox_border,     LV_OPA_TRANSP,    LV_PART_MAIN);
    lv_obj_set_style_border_color(g_prox_border, C_RED,           LV_PART_MAIN);
    lv_obj_set_style_border_width(g_prox_border, 12,              LV_PART_MAIN);
    lv_obj_set_style_border_opa(g_prox_border,   LV_OPA_COVER,   LV_PART_MAIN);
    lv_obj_set_style_radius(g_prox_border,       LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_clear_flag(g_prox_border, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_prox_border, LV_OBJ_FLAG_HIDDEN);   /* hidden until phone near */

    /* 30 ms timer drives border opacity animation */
    lv_timer_create(prox_timer_cb, 30, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
   BLE PROXIMITY TIMER — runs every 30 ms, updates border + alarm label
   ═══════════════════════════════════════════════════════════════════════════ */

static void prox_timer_cb(lv_timer_t *t)
{
    (void)t;
    float prox = g_ble_proximity;

    /* ── alarm blocked — hysteresis prevents flickering near the threshold ── */
    /* block engages above PROX_ALARM_BLOCK, only releases below PROX_ALARM_UNBLOCK */
    if (!alarm_blocked && prox > PROX_ALARM_BLOCK)  alarm_blocked = true;
    if ( alarm_blocked && prox < PROX_ALARM_UNBLOCK) alarm_blocked = false;

    if (g_alarm_blocked_lbl) {
        static bool lbl_visible = false;
        if (alarm_blocked && !lbl_visible) {
            /* fade in */
            lv_obj_clear_flag(g_alarm_blocked_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(g_alarm_blocked_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_fade_in(g_alarm_blocked_lbl, 600, 0);
            lbl_visible = true;
        } else if (!alarm_blocked && lbl_visible) {
            /* fade out */
            lv_obj_fade_out(g_alarm_blocked_lbl, 600, 0);
            lbl_visible = false;
        }
    }

    /* ── perimeter border ── */
    if (!g_prox_border) return;

    if (prox <= PROX_TOO_CLOSE) {
        lv_obj_add_flag(g_prox_border, LV_OBJ_FLAG_HIDDEN);
        g_prox_phase = 0.0f;
        return;
    }

    lv_obj_clear_flag(g_prox_border, LV_OBJ_FLAG_HIDDEN);

    /* t_prox: 0 = just at threshold, 1 = maximum close */
    float t_prox = (prox - PROX_TOO_CLOSE) / (1.0f - PROX_TOO_CLOSE);
    if (t_prox > 1.0f) t_prox = 1.0f;

    /* frequency ramps from 0.33 Hz (slow fade) to 2.0 Hz (abrupt flash) */
    float freq = 0.33f + t_prox * 1.67f;
    g_prox_phase += 2.0f * (float)M_PI * freq * 0.030f;
    if (g_prox_phase > 2.0f * (float)M_PI) g_prox_phase -= 2.0f * (float)M_PI;

    /* sharpness: low = soft sine, high = sharp top-heavy pulse */
    float raw       = (sinf(g_prox_phase) + 1.0f) * 0.5f;
    float sharpness = 1.0f + t_prox * 3.0f;               /* 1 → 4 */
    float shaped    = powf(raw, sharpness);

    /* opacity: 30 % at minimum, 100 % at peak */
    uint8_t opa = (uint8_t)(76.0f + shaped * 179.0f);
    lv_obj_set_style_opa(g_prox_border, opa, LV_PART_MAIN);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SOFTWARE RESET — restore all state to power-on defaults, go to clock
   ═══════════════════════════════════════════════════════════════════════════ */

static void do_software_reset(void)
{
    /* ── state variables ── */
    clock_total_min  =  8 * 60;   /* back to 08:00 default */
    alarm_total_min  =  7 * 60;
    cts_active       = false;     /* re-sync on next BLE CTS write */
    cts_sync_us      = 0;
    cts_sync_min     = 0;
    volume_val       = 50;
    guide_sel        = 0;
    guide_enc_acc    = 0;
    guide_choice     = GUIDE_IM_GOOD;
    connect_card_idx = 0;
    connect_enc_acc  = 0;

    /* ── alarm label ── */
    lv_label_set_text_fmt(g_alarm_lbl, "%02d:%02d",
        alarm_total_min / 60, alarm_total_min % 60);

    /* ── clock hands ── */
    update_clock_hands();

    /* ── guide list back to top ── */
    update_guide_list_anim();

    /* ── connect cards back to first ── */
    lv_label_set_text(g_connect_lbl, CONNECT_CARDS[0]);
    lv_label_set_text_fmt(g_connect_num_lbl, "1 / %d", CONNECT_CARD_COUNT);

    /* ── volume arc + label ── */
    lv_arc_set_value(g_vol_arc, volume_val);
    lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);

    /* ── wind-down screen (handles wind state + breath timer + done labels) ── */
    reset_wind_screen();

    /* ── back to clock ── */
    navigate_to(SCR_CLOCK);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TOUCH INPUT  (call after codec I2C bus is created)
   ═══════════════════════════════════════════════════════════════════════════ */

void lvgl_touch_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.scl_speed_hz    = 300000;
    dev.device_address  = DISP_TOUCH_ADDR;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        i2c_master_bus_add_device(bus, &dev, &disp_touch_dev_handle));

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.disp    = g_disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (!disp_touch_dev_handle) { data->state = LV_INDEV_STATE_RELEASED; return; }
    uint8_t cmd = 0x02, buf[5] = {0};
    i2c_master_transmit_receive(disp_touch_dev_handle, &cmd, 1, buf, 5, 1000);
    if (buf[0]) {
        uint16_t x = (((uint16_t)buf[1] & 0x0f) << 8) | buf[2];
        uint16_t y = (((uint16_t)buf[3] & 0x0f) << 8) | buf[4];
        if (x > LCD_H_RES) x = LCD_H_RES;
        if (y > LCD_V_RES) y = LCD_V_RES;
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   LVGL INFRASTRUCTURE
   ═══════════════════════════════════════════════════════════════════════════ */

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io; (void)edata;
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv,
    const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
        area->x1 + 0x06, area->y1,
        area->x2 + 0x06 + 1, area->y2 + 1, color_map);
}

static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void example_increase_lvgl_tick(void *arg) { (void)arg; lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static bool example_lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux);
    TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, t) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
    assert(lvgl_mux);
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
    (void)arg;
    uint32_t delay = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (example_lvgl_lock(-1)) {
            delay = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (delay > LVGL_TASK_MAX_DELAY_MS) delay = LVGL_TASK_MAX_DELAY_MS;
        if (delay < LVGL_TASK_MIN_DELAY_MS) delay = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
