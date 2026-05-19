#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include "display_bsp.h"
#include "../ExternLib/sh8601/esp_lcd_sh8601.h"

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 80},   
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

DisplayPort::DisplayPort(I2cMasterBus &i2cbus,int width,int height,int scl, int d0, int d1, int d2, int d3, int cs, int rst, int tp_int, int tp_reset, spi_host_device_t spihost) : 
i2cbus_(i2cbus),
spihost_(spihost),
width_(width),
height_(height),
tp_int_(tp_int),
tp_reset_(tp_reset)
{
	int max_transfer_sz = width_ * 50 * 2;
	ESP_LOGI(TAG, "SPI BUS init");
	spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num  = scl,              
    buscfg.data0_io_num = d0,                   
    buscfg.data1_io_num = d1,                   
    buscfg.data2_io_num = d2,                   
    buscfg.data3_io_num = d3,                      
    buscfg.max_transfer_sz = max_transfer_sz;
    ESP_ERROR_CHECK(spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO));

	ESP_LOGI(TAG, "Install panel IO");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = cs;       
    io_config.dc_gpio_num = -1;           
    io_config.spi_mode = 0;               
    io_config.pclk_hz = 40 * 1000 * 1000; 
    io_config.trans_queue_depth = 1;     
    io_config.on_color_trans_done = NULL;   
    io_config.user_ctx = NULL;          
    io_config.lcd_cmd_bits = 32;          
    io_config.lcd_param_bits = 8;         
    io_config.flags.quad_mode = true;                       
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

	esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = rst;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
	
	ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ret_tphandle =  xQueueCreate(5,sizeof(uint32_t));
    assert(ret_tphandle);
}

DisplayPort::~DisplayPort() {
}

void DisplayPort::DisplayPort_TouchInit(void) {
    i2c_master_bus_handle_t BusHandle = i2cbus_.Get_I2cBusHandle();
    i2c_device_config_t     dev_cfg   = {};
    dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz              = 300000;
    dev_cfg.device_address            = 0x38;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(BusHandle, &dev_cfg, &ret_handle));

    is_touchinitialized_ = true;
}

bool DisplayPort::Get_TouchCoords(uint16_t *x,uint16_t *y) {
    uint8_t data_src_touch[5];
    i2cbus_.i2c_read_buff(ret_handle,0x02,data_src_touch,5);
    if(1 == data_src_touch[0]) {
        *x = (((uint16_t)data_src_touch[1] & 0x0f)<<8) | (uint16_t)data_src_touch[2];
        *y = (((uint16_t)data_src_touch[3] & 0x0f)<<8) | (uint16_t)data_src_touch[4];   
        return 1;
    }
    return 0;
}

void DisplayPort::Set_Backlight(uint8_t brightness) {
    if(brightness > 100) brightness = 100;
    brightness_ = brightness;
    uint8_t bl_val = (uint8_t)((brightness_ * 255) / 100);
    uint32_t CMD = 0x51;
    CMD &= 0xFF;
    CMD <<= 8;
    CMD |= 0x02 << 24;
    esp_lcd_panel_io_tx_param(io_handle, CMD, &bl_val,1);
}

BaseType_t DisplayPort::Set_TouchQueueData(uint16_t x,uint16_t y) {
    uint32_t ret = (((uint32_t)x<<16) | y);
    return xQueueSend(ret_tphandle,&ret,0);
}

BaseType_t DisplayPort::Get_TouchQueueData(uint16_t *x,uint16_t *y) {
    uint32_t ret = 0;
    BaseType_t err = 0;
    err = xQueueReceive(ret_tphandle,&ret,0);
    *x = (uint16_t)(ret>>16);
    *y = (uint16_t)(ret & 0xffff);
    return err;
}