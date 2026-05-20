#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "driver/i2c_master.h"
#include "esp_timer.h"

extern i2c_master_bus_handle_t user_i2c_port0_handle;

typedef enum {
    WIND_SELECTING,
    WIND_TRANSITIONING,
    WIND_RUNNING,
    WIND_DONE
} wind_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void         lvgl_port_init(void);
void         on_encoder_delta(int delta);
void         on_button_press(void);
esp_err_t    set_amoled_backlight(uint8_t brig);
void         timer_update_tick(void);
int          get_active_screen(void);
wind_state_t get_wind_state(void);
int          get_wind_minutes(void);
int          get_wind_n_lit(void);
int64_t      get_wind_enter_us(void);
int64_t      get_wind_transition_start_us(void);
int64_t      get_wind_start_us(void);

#ifdef __cplusplus
}
#endif

#endif
