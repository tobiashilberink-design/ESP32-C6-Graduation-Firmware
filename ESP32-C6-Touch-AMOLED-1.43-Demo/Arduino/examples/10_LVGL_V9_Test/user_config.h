#ifndef USER_CONFIG_H
#define USER_CONFIG_H
#include "driver/gpio.h"

// I2C
#define ESP32_SCL_NUM (GPIO_NUM_8)
#define ESP32_SDA_NUM (GPIO_NUM_18)


//#define Backlight_Testing
//#define EXAMPLE_Rotate_90 //软件实现旋转


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
#define BK_LIGHT_PIN       (-1)

#define DISP_TOUCH_ADDR                   0x38
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1
#define LVGL_TASK_STACK_SIZE   (8 * 1024) /*lvgl 9 A larger cache is needed.*/
#define LVGL_TASK_PRIORITY     5


#endif