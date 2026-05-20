/* ═══════════════════════════════════════════════════════════════════════════
   18_UI_Demo2  —  4-screen sleep-prep UI with encoder + touch swipe
   Screens:  0 Clock (analog)  |  1 Sleep time  |  2 Wind-down  |  3 Volume
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

/* ── clock geometry ───────────────────────────────────────────────────────── */
#define CLOCK_CX     (LCD_H_RES / 2)        /* 233 */
#define CLOCK_CY     (LCD_V_RES / 2)        /* 233 */
#define CLOCK_R_FACE 175                     /* face circle radius */
#define CLOCK_R_MARK 155                     /* hour marker radius */
#define HAND_MIN_LEN 130                     /* minute hand length */
#define HAND_HR_LEN   85                     /* hour hand length   */

/* ── app state ────────────────────────────────────────────────────────────── */
static int  clock_total_min = 12 * 60;       /* 0-1439, displayed as H:M     */
static int  alarm_total_min =  7 * 60;       /* 0-1439, sleep target time    */
static int  volume_val      = 50;
static int  active_screen   = 0;

/* ── wind-down state ──────────────────────────────────────────────────────── */
static wind_state_t wind_state               = WIND_SELECTING;
static int          wind_minutes             = 10;
static int          wind_n_lit               = 4;    /* 24 * 10/60 = 4 */
static int64_t      wind_enter_us            = 0;
static int64_t      wind_transition_start_us = 0;
static int64_t      wind_start_us            = 0;

/* ── LVGL handles ─────────────────────────────────────────────────────────── */
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_obj_t *g_tileview       = NULL;
static lv_obj_t *g_tiles[4];
static lv_obj_t *g_dots[4];

/* clock */
static lv_obj_t    *g_hour_hand;
static lv_obj_t    *g_min_hand;
static lv_point_t   g_hour_pts[2];
static lv_point_t   g_min_pts[2];

/* sleep time */
static lv_obj_t *g_sleep_lbl;

/* wind-down screen */
static lv_obj_t *g_wind_arc;
static lv_obj_t *g_wind_min_lbl;
static lv_obj_t *g_wind_min_sublbl;
static lv_obj_t *g_wind_question;
static lv_obj_t *g_wind_done_lbl;

/* volume */
static lv_obj_t *g_vol_arc;
static lv_obj_t *g_vol_lbl;

/* ── display handles ──────────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t amoled_panel_io_handle = NULL;
static i2c_master_dev_handle_t   disp_touch_dev_handle  = NULL;
i2c_master_bus_handle_t          user_i2c_port0_handle  = NULL;

/* ── forward declarations ─────────────────────────────────────────────────── */
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
static void example_lvgl_flush_cb(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *, lv_area_t *);
static void i2c_indev_init(void);
static void example_lvgl_touch_cb(lv_indev_drv_t *, lv_indev_data_t *);
static void example_increase_lvgl_tick(void *);
static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
static void example_lvgl_port_task(void *);
static void create_ui(void);
static void tileview_event_cb(lv_event_t *);
static void update_clock_hands(void);
static void navigate_to(int idx);
static void reset_wind_screen(void);

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
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    amoled_panel_io_handle = io_handle;

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds      = sh8601_lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]);

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

    i2c_indev_init();

    lv_init();
    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * LVGL_BUF_HEIGHT);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_H_RES;
    disp_drv.ver_res    = LCD_V_RES;
    disp_drv.flush_cb   = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf   = &disp_buf;
    disp_drv.user_data  = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.disp    = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &example_increase_lvgl_tick;
    tick_args.name     = "lvgl_tick";
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    if (example_lvgl_lock(-1)) {
        create_ui();
        example_lvgl_unlock();
    }
}

/* ── getter functions ─────────────────────────────────────────────────────── */

int          get_active_screen(void)            { return active_screen; }
wind_state_t get_wind_state(void)               { return wind_state; }
int          get_wind_minutes(void)             { return wind_minutes; }
int          get_wind_n_lit(void)               { return wind_n_lit; }
int64_t      get_wind_enter_us(void)            { return wind_enter_us; }
int64_t      get_wind_transition_start_us(void) { return wind_transition_start_us; }
int64_t      get_wind_start_us(void)            { return wind_start_us; }

/* ── wind-down state machine tick — call from Arduino loop() ─────────────── */

void timer_update_tick(void)
{
    if (!lvgl_mux) return;

    /* RUNNING → DONE when elapsed ≥ total */
    if (wind_state != WIND_RUNNING) return;

    int64_t elapsed = esp_timer_get_time() - wind_start_us;
    int64_t total   = (int64_t)wind_minutes * 60LL * 1000000LL;

    if (elapsed >= total) {
        if (!example_lvgl_lock(5)) return;
        wind_state = WIND_DONE;
        lv_obj_clear_flag(g_wind_done_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_fade_in(g_wind_done_lbl,  2000,      0);   /* fade in over 2 s  */
        lv_obj_fade_out(g_wind_done_lbl, 8000, 120000);   /* fade out after 2 min, over 8 s */
        example_lvgl_unlock();
    }
}

/* ── encoder input ────────────────────────────────────────────────────────── */

void on_encoder_delta(int delta)
{
    if (!lvgl_mux) return;
    if (!example_lvgl_lock(10)) return;

    switch (active_screen) {

        case 0: /* ── Analog clock — continuous 0-1439 min scroll ─────────── */
            clock_total_min = ((clock_total_min + delta) % 1440 + 1440) % 1440;
            update_clock_hands();
            break;

        case 1: /* ── Sleep time — continuous 0-1439 min scroll ──────────── */
            alarm_total_min = ((alarm_total_min + delta) % 1440 + 1440) % 1440;
            lv_label_set_text_fmt(g_sleep_lbl, "%02d:%02d",
                alarm_total_min / 60, alarm_total_min % 60);
            break;

        case 2: /* ── Wind-down — clamp 10-60 min ─────────────────────────── */
            if (wind_state == WIND_SELECTING) {
                wind_minutes += delta;
                if (wind_minutes < 10) wind_minutes = 10;
                if (wind_minutes > 60) wind_minutes = 60;
                lv_arc_set_value(g_wind_arc, wind_minutes);
                lv_label_set_text_fmt(g_wind_min_lbl, "%d", wind_minutes);
            }
            break;

        case 3: /* ── Volume — clamp 0-100 ────────────────────────────────── */
            volume_val += delta;
            if (volume_val <   0) volume_val =   0;
            if (volume_val > 100) volume_val = 100;
            lv_arc_set_value(g_vol_arc, volume_val);
            lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
            break;
    }

    example_lvgl_unlock();
}

/* ── button press ─────────────────────────────────────────────────────────── */

void on_button_press(void)
{
    if (!lvgl_mux) return;

    /* ── 500 ms debounce — ignore accidental double-presses ─────────────── */
    static int64_t last_btn_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_btn_us < 500000LL) return;
    last_btn_us = now_us;

    if (!example_lvgl_lock(10)) return;

    if (active_screen == 0) {
        /* clock → sleep time */
        navigate_to(1);

    } else if (active_screen == 1) {
        /* sleep time → wind-down */
        navigate_to(2);

    } else if (active_screen == 2) {
        if (wind_state == WIND_SELECTING) {
            /* compute how many LEDs correspond to the chosen time */
            wind_n_lit = (int)roundf((float)wind_minutes / 60.0f * 24.0f);
            if (wind_n_lit > 24) wind_n_lit = 24;
            if (wind_n_lit <  0) wind_n_lit = 0;

            /* fade out UI elements over 4 s */
            lv_obj_fade_out(g_wind_arc,        4000, 0);
            lv_obj_fade_out(g_wind_question,   4000, 0);
            lv_obj_fade_out(g_wind_min_lbl,    4000, 0);
            lv_obj_fade_out(g_wind_min_sublbl, 4000, 0);

            /* start countdown immediately — display fades over 4 s independently */
            wind_state    = WIND_RUNNING;
            wind_start_us = esp_timer_get_time();

        } else if (wind_state == WIND_DONE) {
            /* press after "Sleep well" → reset wind screen */
            reset_wind_screen();
        }
        /* ignore press during TRANSITIONING or RUNNING */
    }
    /* screen 3 (volume): button does nothing */

    example_lvgl_unlock();
}

/* ── backlight ────────────────────────────────────────────────────────────── */

esp_err_t set_amoled_backlight(uint8_t brig)
{
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    return esp_lcd_panel_io_tx_param(amoled_panel_io_handle, lcd_cmd, &brig, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── programmatic tileview navigation ────────────────────────────────────── */

static void navigate_to(int idx)
{
    active_screen = idx;
    /* Scroll the tileview directly by pixel offset — more reliable than
       lv_obj_scroll_to_view() for programmatic navigation in LVGL8.       */
    lv_obj_scroll_to(g_tileview, (lv_coord_t)idx * LCD_H_RES, 0, LV_ANIM_ON);

    /* record wind-screen entry time */
    if (idx == 2 && wind_state == WIND_SELECTING)
        wind_enter_us = esp_timer_get_time();

    /* update page-dot indicator */
    for (int i = 0; i < 4; i++) {
        if (!g_dots[i]) continue;
        lv_obj_set_style_bg_color(g_dots[i],
            i == idx ? C_RED : C_DIM, LV_PART_MAIN);
    }
}

/* ── reset wind-down screen to initial SELECTING state ───────────────────── */

static void reset_wind_screen(void)
{
    /* cancel any running fade animations */
    lv_anim_del(g_wind_arc,        NULL);
    lv_anim_del(g_wind_question,   NULL);
    lv_anim_del(g_wind_min_lbl,    NULL);
    lv_anim_del(g_wind_min_sublbl, NULL);
    lv_anim_del(g_wind_done_lbl,   NULL);

    /* restore full opacity on the selectable elements */
    lv_obj_set_style_opa(g_wind_arc,        LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_question,   LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_min_lbl,    LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(g_wind_min_sublbl, LV_OPA_COVER, LV_PART_MAIN);

    /* hide & re-zero the "Sleep well" label for next run */
    lv_obj_add_flag(g_wind_done_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_wind_done_lbl, LV_OPA_TRANSP, LV_PART_MAIN);

    /* reset state */
    wind_state    = WIND_SELECTING;
    wind_minutes  = 10;
    wind_n_lit    = 4;
    wind_enter_us = esp_timer_get_time();

    lv_arc_set_value(g_wind_arc, 10);
    lv_label_set_text(g_wind_min_lbl, "10");
}

/* ═══════════════════════════════════════════════════════════════════════════
   UI CREATION
   ═══════════════════════════════════════════════════════════════════════════ */

static void style_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
}

/* ── clock hand computation ───────────────────────────────────────────────── */

static void update_clock_hands(void)
{
    int h24 = clock_total_min / 60;
    int m   = clock_total_min % 60;

    /* minute hand */
    float min_angle = ((float)m / 60.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
    g_min_pts[0].x = CLOCK_CX;
    g_min_pts[0].y = CLOCK_CY;
    g_min_pts[1].x = CLOCK_CX + (int)(HAND_MIN_LEN * cosf(min_angle));
    g_min_pts[1].y = CLOCK_CY + (int)(HAND_MIN_LEN * sinf(min_angle));
    lv_line_set_points(g_min_hand, g_min_pts, 2);

    /* hour hand — includes fractional hour from minutes */
    float hour_angle = (((float)(h24 % 12) + (float)m / 60.0f) / 12.0f)
                       * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
    g_hour_pts[0].x = CLOCK_CX;
    g_hour_pts[0].y = CLOCK_CY;
    g_hour_pts[1].x = CLOCK_CX + (int)(HAND_HR_LEN * cosf(hour_angle));
    g_hour_pts[1].y = CLOCK_CY + (int)(HAND_HR_LEN * sinf(hour_angle));
    lv_line_set_points(g_hour_hand, g_hour_pts, 2);
}

/* ── screen 0: analog clock (no title, no edit mode label) ──────────────── */

static void create_clock_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* face circle */
    lv_obj_t *face = lv_obj_create(tile);
    lv_obj_set_size(face, CLOCK_R_FACE * 2, CLOCK_R_FACE * 2);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(face, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 12 hour markers */
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

    /* minute hand */
    g_min_hand = lv_line_create(tile);
    lv_obj_set_style_line_color(g_min_hand, C_RED, LV_PART_MAIN);
    lv_obj_set_style_line_width(g_min_hand, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(g_min_hand, true, LV_PART_MAIN);

    /* hour hand */
    g_hour_hand = lv_line_create(tile);
    lv_obj_set_style_line_color(g_hour_hand, C_RED, LV_PART_MAIN);
    lv_obj_set_style_line_width(g_hour_hand, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(g_hour_hand, true, LV_PART_MAIN);

    /* centre dot */
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

/* ── screen 1: sleep time (digital, continuous scroll) ───────────────────── */

static void create_sleep_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* small title */
    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "A L A R M");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* large digital time */
    g_sleep_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_sleep_lbl, "%02d:%02d",
        alarm_total_min / 60, alarm_total_min % 60);
    lv_obj_set_style_text_font(g_sleep_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_sleep_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_sleep_lbl, LV_ALIGN_CENTER, 0, 0);
}

/* ── screen 2: wind-down ─────────────────────────────────────────────────── */

static void create_wind_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* ── question label ─── */
    g_wind_question = lv_label_create(tile);
    lv_label_set_text(g_wind_question, "how long do you\nwant to wind down?");
    lv_obj_set_style_text_font(g_wind_question, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_question, C_MID, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_wind_question, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(g_wind_question, LV_ALIGN_CENTER, 0, -88);

    /* ── arc ─── */
    g_wind_arc = lv_arc_create(tile);
    lv_obj_set_size(g_wind_arc, 390, 390);
    lv_obj_center(g_wind_arc);
    lv_arc_set_mode(g_wind_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_wind_arc, 10, 60);
    lv_arc_set_value(g_wind_arc, 10);
    lv_arc_set_bg_angles(g_wind_arc, 0, 360);
    lv_arc_set_rotation(g_wind_arc, 270);

    lv_obj_set_style_arc_color(g_wind_arc, C_RED,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_wind_arc, 22,      LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_wind_arc, true,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_wind_arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_wind_arc, 22,      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wind_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wind_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_wind_arc, 0, LV_PART_KNOB);

    /* ── minute number ─── */
    g_wind_min_lbl = lv_label_create(tile);
    lv_label_set_text(g_wind_min_lbl, "10");
    lv_obj_set_style_text_font(g_wind_min_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_min_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_wind_min_lbl, LV_ALIGN_CENTER, 0, -16);

    /* ── "min" sublabel ─── */
    g_wind_min_sublbl = lv_label_create(tile);
    lv_label_set_text(g_wind_min_sublbl, "min");
    lv_obj_set_style_text_font(g_wind_min_sublbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_min_sublbl, C_MID, LV_PART_MAIN);
    lv_obj_align(g_wind_min_sublbl, LV_ALIGN_CENTER, 0, 28);

    /* ── "Sleep well" label — invisible until WIND_DONE ─── */
    g_wind_done_lbl = lv_label_create(tile);
    lv_label_set_text(g_wind_done_lbl, "Sleep well");
    lv_obj_set_style_text_font(g_wind_done_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wind_done_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_wind_done_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_wind_done_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(g_wind_done_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
}

/* ── screen 3: volume (title centred inside arc) ─────────────────────────── */

static void create_volume_screen(lv_obj_t *tile)
{
    style_tile(tile);

    /* title centred inside the arc gap */
    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "V O L U M E");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, C_MID, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -88);

    g_vol_arc = lv_arc_create(tile);
    lv_obj_set_size(g_vol_arc, 350, 350);
    lv_obj_center(g_vol_arc);
    lv_arc_set_mode(g_vol_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_vol_arc, 0, 100);
    lv_arc_set_value(g_vol_arc, volume_val);
    lv_arc_set_bg_angles(g_vol_arc, 135, 45);

    lv_obj_set_style_arc_color(g_vol_arc, C_RED,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_vol_arc, 22,      LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_vol_arc, true,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_vol_arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_vol_arc, 22,      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_vol_arc, 0, LV_PART_KNOB);

    /* number and % placed below centre, sitting in the arc's bottom gap */
    g_vol_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
    lv_obj_set_style_text_font(g_vol_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_vol_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_vol_lbl, LV_ALIGN_CENTER, 0, 72);

    lv_obj_t *pct = lv_label_create(tile);
    lv_label_set_text(pct, "%");
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(pct, C_MID, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, 118);
}

/* ── tileview + page-dot overlay ─────────────────────────────────────────── */

static void tileview_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *tv  = lv_event_get_target(e);
    int prev      = active_screen;
    active_screen = lv_obj_get_scroll_x(tv) / LCD_H_RES;
    if (active_screen < 0) active_screen = 0;
    if (active_screen > 3) active_screen = 3;

    /* entering wind-down screen by swipe: record entry time */
    if (active_screen == 2 && prev != 2 && wind_state == WIND_SELECTING)
        wind_enter_us = esp_timer_get_time();

    /* swiping away from wind-down mid-countdown: reset to SELECTING */
    if (prev == 2 && active_screen != 2) {
        if (wind_state == WIND_RUNNING)
            reset_wind_screen();
    }

    /* update page-dot indicator */
    for (int i = 0; i < 4; i++) {
        if (!g_dots[i]) continue;
        lv_obj_set_style_bg_color(g_dots[i],
            i == active_screen ? C_RED : C_DIM, LV_PART_MAIN);
    }
}

static void create_ui(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), C_BG, LV_PART_MAIN);

    g_tileview = lv_tileview_create(lv_scr_act());
    lv_obj_set_size(g_tileview, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(g_tileview, 0, 0);
    lv_obj_set_style_bg_color(g_tileview, C_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(g_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_tileview, tileview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    g_tiles[0] = lv_tileview_add_tile(g_tileview, 0, 0, LV_DIR_RIGHT);
    g_tiles[1] = lv_tileview_add_tile(g_tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[2] = lv_tileview_add_tile(g_tileview, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    g_tiles[3] = lv_tileview_add_tile(g_tileview, 3, 0, LV_DIR_LEFT);

    create_clock_screen(g_tiles[0]);
    create_sleep_screen(g_tiles[1]);
    create_wind_screen(g_tiles[2]);
    create_volume_screen(g_tiles[3]);

    /* page dots */
    lv_obj_t *dot_row = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(dot_row, 10, LV_PART_MAIN);
    lv_obj_set_size(dot_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_layout(dot_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        g_dots[i] = lv_obj_create(dot_row);
        lv_obj_set_size(g_dots[i], 9, 9);
        lv_obj_set_style_radius(g_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_dots[i], i == 0 ? C_RED : C_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_dots[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(g_dots[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(g_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   LVGL INFRASTRUCTURE
   ═══════════════════════════════════════════════════════════════════════════ */

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
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
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void i2c_indev_init(void)
{
    i2c_master_bus_config_t cfg = {};
    cfg.clk_source   = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port     = I2C_NUM_0;
    cfg.scl_io_num   = ESP32_SCL_NUM;
    cfg.sda_io_num   = ESP32_SDA_NUM;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &user_i2c_port0_handle));

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.scl_speed_hz    = 300000;
    dev.device_address  = DISP_TOUCH_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev, &disp_touch_dev_handle));
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
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

static void example_increase_lvgl_tick(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

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
