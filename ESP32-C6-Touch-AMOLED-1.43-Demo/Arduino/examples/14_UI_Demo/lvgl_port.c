/* ═══════════════════════════════════════════════════════════════════════════
   14_UI_Demo  —  4-screen warm-red UI with encoder + touch swipe
   Screens:  0 Clock  |  1 Arc  |  2 Alarm  |  3 Volume
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

/* ── hardware ─────────────────────────────────────────────────────────────── */
#define LCD_HOST          SPI2_HOST
#define LCD_BIT_PER_PIXEL 16

/* ── colour palette ───────────────────────────────────────────────────────── */
#define C_BG    lv_color_make( 15,  4,  4)   /* near-black with red tint  */
#define C_RED   lv_color_make(220, 48, 48)   /* warm red – primary        */
#define C_MID   lv_color_make(140, 30, 30)   /* medium red – titles       */
#define C_DIM   lv_color_make( 65, 14, 14)   /* dim red – inactive dots   */
#define C_TRACK lv_color_make( 38,  9,  9)   /* very dark – arc track     */

/* ── app state ────────────────────────────────────────────────────────────── */
static int  clock_h      = 12,  clock_m     = 0;
static int  alarm_h      =  7,  alarm_m     = 0;
static int  volume_val   = 50;
static int  arc_steps    = 0;
static bool clock_edit_h = false;   /* false = editing minutes */
static bool alarm_edit_h = false;
static int  active_screen = 0;

/* ── LVGL handles ─────────────────────────────────────────────────────────── */
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_obj_t *g_tileview       = NULL;
static lv_obj_t *g_tiles[4];
static lv_obj_t *g_dots[4];

static lv_obj_t *g_clock_lbl;    /* "12:00"             */
static lv_obj_t *g_clock_edit;   /* "MINUTES" / "HOURS" */
static lv_obj_t *g_arc;
static lv_obj_t *g_arc_lbl;      /* step number inside arc */
static lv_obj_t *g_alarm_lbl;
static lv_obj_t *g_alarm_edit;
static lv_obj_t *g_vol_arc;
static lv_obj_t *g_vol_lbl;      /* "50"  */

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

    /* SPI bus */
    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num    = LCD_D0_PIN;
    buscfg.data1_io_num    = LCD_D1_PIN;
    buscfg.sclk_io_num     = LCD_PCLK_PIN;
    buscfg.data2_io_num    = LCD_D2_PIN;
    buscfg.data3_io_num    = LCD_D3_PIN;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD IO */
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

    /* Panel */
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

    /* LVGL */
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

/* ─────────────────────────────────────────────────────────────────────────── */

void on_encoder_delta(int delta)
{
    if (!lvgl_mux) return;
    if (!example_lvgl_lock(10)) return;

    switch (active_screen) {

        case 0: /* ── Clock ─────────────────────────────────────────────── */
            if (clock_edit_h)
                clock_h = ((clock_h + delta) % 24 + 24) % 24;
            else
                clock_m = ((clock_m + delta) % 60 + 60) % 60;
            lv_label_set_text_fmt(g_clock_lbl, "%02d:%02d", clock_h, clock_m);
            break;

        case 1: /* ── Arc ───────────────────────────────────────────────── */
            arc_steps += delta;
            {
                int norm = ((arc_steps % 80) + 80) % 80;
                lv_arc_set_value(g_arc, norm);
                lv_label_set_text_fmt(g_arc_lbl, "%d", norm);
            }
            break;

        case 2: /* ── Alarm ─────────────────────────────────────────────── */
            if (alarm_edit_h)
                alarm_h = ((alarm_h + delta) % 24 + 24) % 24;
            else
                alarm_m = ((alarm_m + delta) % 60 + 60) % 60;
            lv_label_set_text_fmt(g_alarm_lbl, "%02d:%02d", alarm_h, alarm_m);
            break;

        case 3: /* ── Volume ────────────────────────────────────────────── */
            volume_val += delta;
            if (volume_val < 0)   volume_val = 0;
            if (volume_val > 100) volume_val = 100;
            lv_arc_set_value(g_vol_arc, volume_val);
            lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
            break;
    }

    example_lvgl_unlock();
}

void on_button_press(void)
{
    if (!lvgl_mux) return;
    if (!example_lvgl_lock(10)) return;

    if (active_screen == 0) {
        clock_edit_h = !clock_edit_h;
        lv_label_set_text(g_clock_edit, clock_edit_h ? "HOURS" : "MINUTES");
        lv_obj_set_style_text_color(g_clock_edit,
            clock_edit_h ? C_RED : C_MID, LV_PART_MAIN);
    } else if (active_screen == 2) {
        alarm_edit_h = !alarm_edit_h;
        lv_label_set_text(g_alarm_edit, alarm_edit_h ? "HOURS" : "MINUTES");
        lv_obj_set_style_text_color(g_alarm_edit,
            alarm_edit_h ? C_RED : C_MID, LV_PART_MAIN);
    }

    example_lvgl_unlock();
}

esp_err_t set_amoled_backlight(uint8_t brig)
{
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    return esp_lcd_panel_io_tx_param(amoled_panel_io_handle, lcd_cmd, &brig, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
   UI CREATION
   ═══════════════════════════════════════════════════════════════════════════ */

/* shared helper: style a tile's background */
static void style_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
}

/* shared helper: small screen title */
static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, C_MID, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 68);
    return lbl;
}

/* shared helper: thin decorative divider */
static void make_divider(lv_obj_t *parent, int y_offset)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, 50, 2);
    lv_obj_set_style_bg_color(line, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, y_offset);
}

/* ── screen 0: clock ─────────────────────────────────────────────────────── */
static void create_clock_screen(lv_obj_t *tile)
{
    style_tile(tile);
    make_title(tile, "C L O C K");

    /* large time */
    g_clock_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_clock_lbl, "%02d:%02d", clock_h, clock_m);
    lv_obj_set_style_text_font(g_clock_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_clock_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_clock_lbl, LV_ALIGN_CENTER, 0, -18);

    make_divider(tile, 38);

    /* edit-mode indicator */
    g_clock_edit = lv_label_create(tile);
    lv_label_set_text(g_clock_edit, "MINUTES");
    lv_obj_set_style_text_font(g_clock_edit, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_clock_edit, C_MID, LV_PART_MAIN);
    lv_obj_align(g_clock_edit, LV_ALIGN_CENTER, 0, 62);
}

/* ── screen 1: arc ───────────────────────────────────────────────────────── */
static void create_arc_screen(lv_obj_t *tile)
{
    style_tile(tile);

    g_arc = lv_arc_create(tile);
    lv_obj_set_size(g_arc, 390, 390);
    lv_obj_center(g_arc);
    lv_arc_set_mode(g_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_arc, 0, 79);
    lv_arc_set_value(g_arc, 0);
    lv_arc_set_bg_angles(g_arc, 0, 360);
    lv_arc_set_rotation(g_arc, 270);   /* start at 12 o'clock */

    lv_obj_set_style_arc_color(g_arc, C_RED,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc, 22,      LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, true,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 22,      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, 0, LV_PART_KNOB);

    g_arc_lbl = lv_label_create(tile);
    lv_label_set_text(g_arc_lbl, "0");
    lv_obj_set_style_text_font(g_arc_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_arc_lbl, C_RED, LV_PART_MAIN);
    lv_obj_center(g_arc_lbl);
}

/* ── screen 2: alarm ─────────────────────────────────────────────────────── */
static void create_alarm_screen(lv_obj_t *tile)
{
    style_tile(tile);
    make_title(tile, "A L A R M");

    g_alarm_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_alarm_lbl, "%02d:%02d", alarm_h, alarm_m);
    lv_obj_set_style_text_font(g_alarm_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_alarm_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_alarm_lbl, LV_ALIGN_CENTER, 0, -18);

    make_divider(tile, 38);

    g_alarm_edit = lv_label_create(tile);
    lv_label_set_text(g_alarm_edit, "MINUTES");
    lv_obj_set_style_text_font(g_alarm_edit, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_alarm_edit, C_MID, LV_PART_MAIN);
    lv_obj_align(g_alarm_edit, LV_ALIGN_CENTER, 0, 62);
}

/* ── screen 3: volume ────────────────────────────────────────────────────── */
static void create_volume_screen(lv_obj_t *tile)
{
    style_tile(tile);
    make_title(tile, "V O L U M E");

    /* 270° arc — open at the bottom, like a real knob */
    g_vol_arc = lv_arc_create(tile);
    lv_obj_set_size(g_vol_arc, 350, 350);
    lv_obj_center(g_vol_arc);
    lv_arc_set_mode(g_vol_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(g_vol_arc, 0, 100);
    lv_arc_set_value(g_vol_arc, volume_val);
    lv_arc_set_bg_angles(g_vol_arc, 135, 45);  /* 270° sweep, opens at bottom */

    lv_obj_set_style_arc_color(g_vol_arc, C_RED,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_vol_arc, 22,      LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_vol_arc, true,  LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_vol_arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_vol_arc, 22,      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_vol_arc, 0, LV_PART_KNOB);

    /* large volume number */
    g_vol_lbl = lv_label_create(tile);
    lv_label_set_text_fmt(g_vol_lbl, "%d", volume_val);
    lv_obj_set_style_text_font(g_vol_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_vol_lbl, C_RED, LV_PART_MAIN);
    lv_obj_align(g_vol_lbl, LV_ALIGN_CENTER, 0, -10);

    /* "%" label below number */
    lv_obj_t *pct = lv_label_create(tile);
    lv_label_set_text(pct, "%");
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(pct, C_MID, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, 38);
}

/* ── tileview + page-dot overlay ─────────────────────────────────────────── */
static void tileview_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *tv = lv_event_get_target(e);
    active_screen = lv_obj_get_scroll_x(tv) / LCD_H_RES;
    if (active_screen < 0) active_screen = 0;
    if (active_screen > 3) active_screen = 3;

    /* reset edit fields on screen change */
    clock_edit_h = false;
    alarm_edit_h = false;
    if (g_clock_edit) {
        lv_label_set_text(g_clock_edit, "MINUTES");
        lv_obj_set_style_text_color(g_clock_edit, C_MID, LV_PART_MAIN);
    }
    if (g_alarm_edit) {
        lv_label_set_text(g_alarm_edit, "MINUTES");
        lv_obj_set_style_text_color(g_alarm_edit, C_MID, LV_PART_MAIN);
    }

    /* update dots */
    for (int i = 0; i < 4; i++) {
        if (!g_dots[i]) continue;
        lv_obj_set_style_bg_color(g_dots[i],
            i == active_screen ? C_RED : C_DIM, LV_PART_MAIN);
    }
}

static void create_ui(void)
{
    /* screen background */
    lv_obj_set_style_bg_color(lv_scr_act(), C_BG, LV_PART_MAIN);

    /* tileview ─────────────────────────────────────────────────────────── */
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

    /* build each screen */
    create_clock_screen(g_tiles[0]);
    create_arc_screen(g_tiles[1]);
    create_alarm_screen(g_tiles[2]);
    create_volume_screen(g_tiles[3]);

    /* page dots — fixed overlay, children of screen (above tileview) ──── */
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
   LVGL INFRASTRUCTURE  (identical to other demos)
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
