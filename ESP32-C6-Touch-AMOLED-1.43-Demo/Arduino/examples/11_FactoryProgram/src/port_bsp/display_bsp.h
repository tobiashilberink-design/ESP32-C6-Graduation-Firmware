#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#include "i2c_bsp.h"

class DisplayPort {
private:
    const char * TAG = "DisplayPort";
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    i2c_master_dev_handle_t ret_handle = NULL;
    QueueHandle_t ret_tphandle = NULL;
    I2cMasterBus &i2cbus_;
    spi_host_device_t spihost_;
    int width_;
    int height_;
    int tp_int_;
    int tp_reset_;
    bool is_touchinitialized_ = false;
    uint8_t brightness_ = 100;

public:
    DisplayPort(I2cMasterBus &i2cbus,int width,int height,int scl = 11, int d0 = 4, int d1 = 5, int d2 = 6, int d3 = 7, int cs = 10, int rst = 3, int tp_int = -1, int tp_reset = -1, spi_host_device_t spihost = SPI2_HOST);
    ~DisplayPort();

    void DisplayPort_TouchInit(void);
    void Set_Backlight(uint8_t brightness);

    bool Get_TouchCoords(uint16_t *x,uint16_t *y);

    spi_host_device_t Get_SpiHost() { return spihost_; }
    esp_lcd_panel_handle_t Get_PanelHandle() { return panel_handle; }
    esp_lcd_panel_io_handle_t Get_IoHandle() { return io_handle; }
    int Get_Width() { return width_; }
    int Get_Height() { return height_; }
    bool Get_TouchInitialized() { return is_touchinitialized_; }
    uint8_t Get_Brightness() { return brightness_; }
    QueueHandle_t Get_TouchQueueHandle() {return ret_tphandle;}
    BaseType_t Set_TouchQueueData(uint16_t x,uint16_t y);
    BaseType_t Get_TouchQueueData(uint16_t *x,uint16_t *y);
    BaseType_t Set_ResetTouchQueue() {return xQueueReset(ret_tphandle);}
};