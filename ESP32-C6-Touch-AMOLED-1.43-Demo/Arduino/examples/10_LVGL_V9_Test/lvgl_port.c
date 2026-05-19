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
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "src/sh8601/esp_lcd_sh8601.h"
#include "demos/lv_demos.h"

#define LCD_HOST    SPI2_HOST

static SemaphoreHandle_t lvgl_mux = NULL;
#define LCD_BIT_PER_PIXEL 16
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (LCD_H_RES * LVGL_BUF_HEIGHT * BYTES_PER_PIXEL)

uint8_t *lvgl_dest = NULL;

static esp_lcd_panel_io_handle_t amoled_panel_io_handle = NULL;
static i2c_master_dev_handle_t disp_touch_dev_handle = NULL;
static SemaphoreHandle_t flush_done_semaphore = NULL;

/*static api*/
static void example_increase_lvgl_tick(void *arg);
static void example_lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);
static void lvgl_flush_wait_cb(lv_display_t * disp);
static void i2c_indev_init(void);
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
static void example_lvgl_rounder_cb(lv_event_t *e);

static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
static void example_lvgl_port_task(void *arg);
static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData);


static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 80},   
  {0xC4, (uint8_t []){0x80}, 1, 0},
  //{0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
  //{0x35, (uint8_t []){0x00}, 1, 0},//TE ON
  //{0x36, (uint8_t []){0x60}, 1, 0},
  {0x53, (uint8_t []){0x20}, 1, 1},
  {0x63, (uint8_t []){0xFF}, 1, 1},
  {0x51, (uint8_t []){0x00}, 1, 1},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
};

void lvgl_port_init(void)
{
  flush_done_semaphore = xSemaphoreCreateBinary();
  assert(flush_done_semaphore);

  spi_bus_config_t buscfg = {};
  buscfg.data0_io_num = LCD_D0_PIN;
  buscfg.data1_io_num = LCD_D1_PIN;
  buscfg.sclk_io_num = LCD_PCLK_PIN;
  buscfg.data2_io_num = LCD_D2_PIN;
  buscfg.data3_io_num = LCD_D3_PIN;
  buscfg.max_transfer_sz = (LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8); 
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io_handle = NULL;

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = LCD_CS_PIN;       
  io_config.dc_gpio_num = -1;          
  io_config.spi_mode = 0;              
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;    
  io_config.on_color_trans_done = example_notify_lvgl_flush_ready;  
  //io_config.user_ctx = &disp_drv;         
  io_config.lcd_cmd_bits = 32;         
  io_config.lcd_param_bits = 8;        
  io_config.flags.quad_mode = true;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  amoled_panel_io_handle = io_handle;

  sh8601_vendor_config_t vendor_config = {};
  vendor_config.flags.use_qspi_interface = 1;
  vendor_config.init_cmds = sh8601_lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]);
  
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = LCD_RST_PIN;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
  panel_config.vendor_config = &vendor_config;

  esp_lcd_panel_handle_t panel_handle = NULL;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_set_gap(panel_handle,0x06,0x00)); //设置横向偏移
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));

  /*i2c init*/
  i2c_indev_init();


  /*lvgl port*/
  lv_init();
  lv_display_t * disp = lv_display_create(LCD_H_RES, LCD_V_RES); /* 以水平和垂直分辨率（像素）进行基本初始化 */
  lv_display_set_flush_cb(disp, example_lvgl_flush_cb);                          /* 设置刷新回调函数以绘制到显示屏 */
  lv_display_set_flush_wait_cb(disp,lvgl_flush_wait_cb);
  uint8_t *buf_1 = NULL;
  uint8_t *buf_2 = NULL;
  buf_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
  buf_2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
  lv_display_set_buffers(disp, buf_1, buf_2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_user_data(disp, panel_handle);
  lv_display_add_event_cb(disp,example_lvgl_rounder_cb,LV_EVENT_INVALIDATE_AREA,NULL);
#ifdef EXAMPLE_Rotate_90
  lvgl_dest = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA); //旋转buf
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
#endif

  /*port indev*/
  lv_indev_t *touch_indev = NULL;
  touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);
  lv_indev_set_user_data(touch_indev, disp_touch_dev_handle);

  esp_timer_create_args_t lvgl_tick_timer_args = {};
  lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
  lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));


  lvgl_mux = xSemaphoreCreateMutex(); //mutex semaphores
  assert(lvgl_mux);
  xTaskCreate(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
  if (example_lvgl_lock(-1)) 
  {   
    lv_demo_widgets();        /* A widgets example */
    //lv_demo_music();        /* A modern, smartphone-like music player demo. */
    //lv_demo_stress();       /* A stress test for LVGL. */
    //lv_demo_benchmark();    /* A demo to measure the performance of LVGL or to compare different settings. */

    // Release the mutex
    example_lvgl_unlock();
  }

}
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(LVGL_TICK_PERIOD_MS);
}
static void example_lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
  lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));
#ifdef EXAMPLE_Rotate_90 
  lv_display_rotation_t rotation = lv_display_get_rotation(disp);
  lv_area_t rotated_area;
  if(rotation != LV_DISPLAY_ROTATION_0)
  {
    lv_color_format_t cf = lv_display_get_color_format(disp);
    /*Calculate the position of the rotated area*/
    rotated_area = *area;
    lv_display_rotate_area(disp, &rotated_area);
    /*Calculate the source stride (bytes in a line) from the width of the area*/
    uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
    /*Calculate the stride of the destination (rotated) area too*/
    uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
    /*Have a buffer to store the rotated area and perform the rotation*/
    
    int32_t src_w = lv_area_get_width(area);
    int32_t src_h = lv_area_get_height(area);
    lv_draw_sw_rotate(color_p, lvgl_dest, src_w, src_h, src_stride, dest_stride, rotation, cf);
    /*Use the rotated area and rotated buffer from now on*/
    area = &rotated_area;
  }
  esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2+1, area->y2+1, lvgl_dest);
#else
  //copy a buffer's content to a specific area of the display
  esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2+1, area->y2+1, color_p);
#endif
}
static void lvgl_flush_wait_cb(lv_display_t * disp) //等待发送数据完成,使用lvgl_flush_wait_cb 不需要再使用lv_disp_flush_ready(disp);
{
  xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
}
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t high_task_awoken = pdFALSE;
  xSemaphoreGiveFromISR(flush_done_semaphore, &high_task_awoken);
  return high_task_awoken == pdTRUE;
}
static void i2c_indev_init(void)
{
  i2c_master_bus_handle_t user_i2c_port0_handle;

  i2c_master_bus_config_t i2c_bus_config = {};
  i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_bus_config.i2c_port = I2C_NUM_0;
  i2c_bus_config.scl_io_num = ESP32_SCL_NUM;
  i2c_bus_config.sda_io_num = ESP32_SDA_NUM;
  i2c_bus_config.glitch_ignore_cnt = 7;
  i2c_bus_config.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &user_i2c_port0_handle));
  
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.scl_speed_hz = 300000;
  dev_cfg.device_address = DISP_TOUCH_ADDR;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &disp_touch_dev_handle));
}
static void example_lvgl_rounder_cb(lv_event_t *e)
{
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e); 

  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;
  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;

  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}
static bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}
static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}
static void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (example_lvgl_lock(-1))
    {
      task_delay_ms = lv_timer_handler();
      // Release the mutex
      example_lvgl_unlock();
    }
    if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    }
    else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}
static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData)
{
  i2c_master_dev_handle_t i2c_dev = (i2c_master_dev_handle_t)lv_indev_get_user_data(indev);
  uint8_t cmd = 0x02;
  uint8_t buf[5] = {0};
  uint16_t tp_x,tp_y;
  i2c_master_transmit_receive(i2c_dev,&cmd,1,buf,5,1000);
  if(buf[0])
  {
    tp_x = (((uint16_t)buf[1] & 0x0f)<<8) | (uint16_t)buf[2];
    tp_y = (((uint16_t)buf[3] & 0x0f)<<8) | (uint16_t)buf[4];
    if(tp_x > LCD_H_RES)
    {tp_x = LCD_H_RES;}
    if(tp_y > LCD_V_RES)
    {tp_y = LCD_V_RES;}
    indevData->point.x = tp_x;
    indevData->point.y = tp_y;
    //ESP_LOGE("tp","(%ld,%ld)",indevData->point.x,indevData->point.y);
    indevData->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    indevData->state = LV_INDEV_STATE_RELEASED;
  }
}

esp_err_t set_amoled_backlight(uint8_t brig)
{
  uint32_t lcd_cmd = 0x51;
  lcd_cmd &= 0xff;
  lcd_cmd <<= 8;
  lcd_cmd |= 0x02 << 24;
  return esp_lcd_panel_io_tx_param(amoled_panel_io_handle, lcd_cmd, &brig,1);
}