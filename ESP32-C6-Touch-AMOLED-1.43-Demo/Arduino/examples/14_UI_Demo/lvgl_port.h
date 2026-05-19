#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "driver/i2c_master.h"

extern i2c_master_bus_handle_t user_i2c_port0_handle;

#ifdef __cplusplus
extern "C" {
#endif

void      lvgl_port_init(void);
void      on_encoder_delta(int delta);
void      on_button_press(void);
esp_err_t set_amoled_backlight(uint8_t brig);

#ifdef __cplusplus
}
#endif

#endif
