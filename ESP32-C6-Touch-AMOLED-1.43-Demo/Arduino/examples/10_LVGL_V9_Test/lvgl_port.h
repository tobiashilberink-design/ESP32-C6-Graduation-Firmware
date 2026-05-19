#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_port_init(void);
esp_err_t set_amoled_backlight(uint8_t brig);

#ifdef __cplusplus
}
#endif



#endif










