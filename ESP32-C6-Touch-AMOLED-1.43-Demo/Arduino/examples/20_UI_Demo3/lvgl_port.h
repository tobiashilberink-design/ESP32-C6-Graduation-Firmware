#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "esp_timer.h"

/* ── screen indices (0-based) ─────────────────────────────────────────────── */
#define SCR_CLOCK    0
#define SCR_ALARM    1
#define SCR_GUIDE    2
#define SCR_WIND     3
#define SCR_HEART    4
#define SCR_CONNECT  5
#define SCR_MUSIC    6
#define SCR_PODCAST  7
#define SCR_BGNOISE  8
#define SCR_VOLUME   9
#define NUM_SCREENS  10

/* ── wind-down state ──────────────────────────────────────────────────────── */
typedef enum {
    WIND_SELECTING,
    WIND_RUNNING,
    WIND_DONE
} wind_state_t;

/* ── guided wind-down choice ──────────────────────────────────────────────── */
typedef enum {
    GUIDE_IM_GOOD = 0,
    GUIDE_HEART_COHERENCE,
    GUIDE_CONVERSATION,
    GUIDE_BACKGROUND_NOISE,
    GUIDE_MUSIC,
    GUIDE_PODCAST,
    GUIDE_COUNT
} guide_choice_t;

#ifdef __cplusplus
extern "C" {
#endif

void           lvgl_port_init(void);
void           on_encoder_delta(int delta);
void           on_button_press(void);
void           timer_update_tick(void);

int            get_active_screen(void);
wind_state_t   get_wind_state(void);
guide_choice_t get_guide_choice(void);
int            get_wind_minutes(void);
int            get_wind_n_lit(void);
int64_t        get_wind_enter_us(void);
int64_t        get_wind_start_us(void);

#ifdef __cplusplus
}
#endif

#endif
