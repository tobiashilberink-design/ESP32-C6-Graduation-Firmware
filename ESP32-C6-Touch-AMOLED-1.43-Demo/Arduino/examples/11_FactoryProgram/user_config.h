#ifndef USER_CONFIG_H
#define USER_CONFIG_H
#include "driver/gpio.h"

/*sdcard en*/
#define ESP32_SDCARD_EN 1

/*Consider shortening the compilation time by setting 0*/
#define ESP32_WIFI_BLE_Scan_EN 1


// I2C
#define ESP32_SCL_NUM (GPIO_NUM_8)
#define ESP32_SDA_NUM (GPIO_NUM_18)

#define LCD_H_RES 466
#define LCD_V_RES 466
#define LVGL_BUF_HEIGHT 50 

#define LCD_CS_PIN         GPIO_NUM_10
#define LCD_PCLK_PIN       GPIO_NUM_11
#define LCD_D0_PIN         GPIO_NUM_4
#define LCD_D1_PIN         GPIO_NUM_5
#define LCD_D2_PIN         GPIO_NUM_6
#define LCD_D3_PIN         GPIO_NUM_7
#define LCD_RST_PIN        GPIO_NUM_3

#define DISP_TOUCH_ADDR                   0x38

#endif