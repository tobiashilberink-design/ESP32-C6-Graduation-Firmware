#ifndef USER_CONFIG_H
#define USER_CONFIG_H
#include "freertos/FreeRTOS.h"
//spi handle
#define SDSPI_HOST SPI2_HOST
#define LCD_HOST SPI2_HOST

// I2C
#define ESP32_SCL_NUM (GPIO_NUM_8)
#define ESP32_SDA_NUM (GPIO_NUM_18)

//  DISP
#define EXAMPLE_LCD_H_RES              466
#define EXAMPLE_LCD_V_RES              466

#define DISP_ROT_90    1
#define DISP_ROT_NONO  0
#define Rotated DISP_ROT_NONO   //软件实现旋转

#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_11) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_4)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_5)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_6)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_7)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_3)

#define DISP_TOUCH_ADDR                   0x38
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)


// SD CARD
#define SD_CARD_EN (1)

#define SD_MISO   (GPIO_NUM_1)
#define SD_MOSI   (GPIO_NUM_2)
#define SD_CLK    (EXAMPLE_PIN_NUM_LCD_PCLK)
#define SD_CS     (GPIO_NUM_15)
#if SD_CARD_EN
extern SemaphoreHandle_t _xSemaphore_sdp;
#endif

//EXIO TCA9554
#define EXIO_TCA9554_ADDR  0x20   // 7-bit address


//PCF85063
#define RTC_PCF85063_ADDR 0x51              // 7-bit address
//QMI8658
#define IMU_QMI8658_ADDR  0x6b              // 7-bit address

#endif