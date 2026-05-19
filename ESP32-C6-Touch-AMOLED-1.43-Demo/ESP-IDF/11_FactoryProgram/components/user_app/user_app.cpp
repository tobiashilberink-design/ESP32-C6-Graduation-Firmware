#include <stdio.h>
#include "user_app.h"
#include "i2c_bsp.h"
#include "user_config.h"
#include "sdcard_bsp.h"
#include "freertos/FreeRTOS.h"
#include "adc_bsp.h"
#include "button_bsp.h"
#include "user_audio_bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_equipment.h"

#include "ble_scan_bsp.h"
#include "esp_wifi_bsp.h"

#include "esp_io_expander_tca9554.h"

#include "gui_guider.h"

lv_ui user_ui;

static esp_io_expander_handle_t io_expander = NULL;
extern void setBrightnes(uint8_t brig);

static void example_button_task(void* parmeter)
{
  lv_ui *ui = (lv_ui *)parmeter;
  uint8_t even_flag = 0x01;
  uint8_t ticks = 0;
  uint32_t sdcard_test = 0;
  char sdcard_buf[35] = {""};
  char sdcard_rec[35] = {""};
  for (;;)
  {
    EventBits_t even = xEventGroupWaitBits(key_groups,BIT_EVEN_ALL,pdTRUE,pdFALSE,pdMS_TO_TICKS(2 * 1000));
    if(READ_BIT(even,0)) //boot
    {
      if(READ_BIT(even_flag,0))
      {
        CLEAR_BIT(even_flag,0);
        lv_obj_clear_flag(ui->screen_carousel_1,LV_OBJ_FLAG_SCROLLABLE); //unmovable
        lv_obj_clear_flag(ui->screen_cont_4,LV_OBJ_FLAG_HIDDEN); 
        lv_obj_add_flag(ui->screen_cont_3, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        SET_BIT(even_flag,0);
        lv_obj_add_flag(ui->screen_carousel_1,LV_OBJ_FLAG_SCROLLABLE); //removable
        lv_obj_clear_flag(ui->screen_cont_3,LV_OBJ_FLAG_HIDDEN); 
        lv_obj_add_flag(ui->screen_cont_4, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_invalidate(ui->screen_carousel_1);  // 标记重绘
    }
    if(READ_BIT(even,5)) //长按 boot
    {
      audio_Test_flag = 0;
    }
    if(READ_BIT(even,3)) //弹起 boot
    {
      audio_Test_flag = 1;
    }
    if(READ_BIT(even,12)) //长按 pwr
    {
      if(READ_BIT(even_flag,1))
      {
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0);
      }
    }
    if(!READ_BIT(even_flag,1))  //
    {
      ticks++;
      if(READ_BIT(even,10) || (ticks == 4))
      {
        SET_BIT(even_flag,1);
      }
    }
    if(READ_BIT(even,1)) //dc boot
    {
      sdcard_test++;
      snprintf(sdcard_buf,33,"sdcardTest : %ld",sdcard_test);
      sdcard_file_write("/sdcard/sdcardTest.txt",sdcard_buf);
      sdcard_file_read("/sdcard/sdcardTest.txt",sdcard_rec,NULL);
      if(!strcmp(sdcard_rec,sdcard_buf))
      {
        lv_label_set_text(ui->screen_label_25, "sdcard test passed");
      }
      else
      {
        lv_label_set_text(ui->screen_label_25, "sdcard test failed");
      }
    }
    else
    {
      lv_label_set_text(ui->screen_label_25, "");
    }
  }
}
static void example_color_task(void *arg)
{
  lv_ui *ui = (lv_ui *)arg;
  lv_obj_clear_flag(ui->screen_carousel_1,LV_OBJ_FLAG_SCROLLABLE); //unmovable
  lv_obj_clear_flag(ui->screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
  lv_obj_add_flag(ui->screen_cont_3, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui->screen_cont_4, LV_OBJ_FLAG_HIDDEN);

  lv_obj_clear_flag(ui->screen_img_1,LV_OBJ_FLAG_HIDDEN); 
  lv_obj_add_flag(ui->screen_img_2, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui->screen_img_3, LV_OBJ_FLAG_HIDDEN);
  vTaskDelay(pdMS_TO_TICKS(1500));
  lv_obj_clear_flag(ui->screen_img_2,LV_OBJ_FLAG_HIDDEN); 
  lv_obj_add_flag(ui->screen_img_1, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui->screen_img_3, LV_OBJ_FLAG_HIDDEN);
  vTaskDelay(pdMS_TO_TICKS(1500));
  lv_obj_clear_flag(ui->screen_img_3,LV_OBJ_FLAG_HIDDEN); 
  lv_obj_add_flag(ui->screen_img_2, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui->screen_img_1, LV_OBJ_FLAG_HIDDEN);
  vTaskDelay(pdMS_TO_TICKS(1500));

  lv_obj_clear_flag(ui->screen_cont_3,LV_OBJ_FLAG_HIDDEN); 
  lv_obj_add_flag(ui->screen_cont_2, LV_OBJ_FLAG_HIDDEN); 

  lv_obj_add_flag(ui->screen_carousel_1,LV_OBJ_FLAG_SCROLLABLE); //removable
  vTaskDelete(NULL); 
}
static void example_user_task(void *arg)
{
  lv_ui *ui = (lv_ui *)arg;
  char obj_send_data[30] = {""};
  float adc_value = 0;
  uint32_t times = 0;
  uint32_t rtc_time = 0;
  uint32_t qmi_time = 0;
  uint32_t adc_time = 0;
  for(;;)
  {
    if(times - adc_time == 10) //2s
    {
      adc_time = times;
      adc_get_value(&adc_value,NULL);
      snprintf(obj_send_data,28,"%.2fV",adc_value);
      lv_label_set_text(ui->screen_label_7, obj_send_data);
    }
    if(times - rtc_time == 5) //1s
    {
      rtc_time = times;
      RtcDateTime_t rtc = i2c_rtc_get();
      snprintf(obj_send_data,28,"%d/%d/%d %02d:%02d:%02d",rtc.year,rtc.month,rtc.day,rtc.hour,rtc.minute,rtc.second);
      lv_label_set_text(ui->screen_label_10, obj_send_data);
    }
    if(times - qmi_time == 1) //200ms
    {
      qmi_time = times;
      ImuDate_t qmi = i2c_imu_get();
      snprintf(obj_send_data,28,"%.2f,%.2f,%.2f (g)",qmi.accx,qmi.accy,qmi.accz);
      lv_label_set_text(ui->screen_label_12, obj_send_data);
      snprintf(obj_send_data,28,"%.2f,%.2f,%.2f (dps)",qmi.gyrox,qmi.gyroy,qmi.gyroz);
      lv_label_set_text(ui->screen_label_19, obj_send_data);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    times++;
  }
}
#if SD_CARD_EN
static void example_sdcard_task(void *arg)
{
  lv_ui *ui = (lv_ui *)arg;
  char obj_send_data[10] = {""};
  EventBits_t even = xEventGroupWaitBits(sdcard_even_,(0x01),pdTRUE,pdFALSE,pdMS_TO_TICKS(8 * 1000));
  if(READ_BIT(even,0))
  {
    snprintf(obj_send_data,10,"%.2fG",user_sdcard_bsp.sdcard_size);
    lv_label_set_text(ui->screen_label_6, obj_send_data);
  }
  else
  {
    lv_label_set_text(ui->screen_label_6, "NULL");
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
  vTaskDelete(NULL); 
}
#endif
void example_scan_wifi_ble_task(void *arg)
{
  lv_ui *Send_ui = (lv_ui *)arg;
  char send_lvgl[10] = {""};
  uint8_t ble_scan_count = 0;
  uint8_t ble_mac[6];
  EventBits_t even = xEventGroupWaitBits(wifi_even_,0x02,pdTRUE,pdTRUE,pdMS_TO_TICKS(30000)); 
  espwifi_deinit(); //释放WIFI
  ble_scan_prepare();
  ble_stack_init();
  ble_scan_start();
  for(;xQueueReceive(ble_queue,ble_mac,3500) == pdTRUE;)
  {
    //ESP_LOGI(TAG, "%d",connt);
    ble_scan_count++;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if(READ_BIT(even,1))
  {
    snprintf(send_lvgl,9,"%d",user_esp_bsp.apNum);
    lv_label_set_text(Send_ui->screen_label_21, send_lvgl);
  }
  else
  {
    lv_label_set_text(Send_ui->screen_label_21, "P");
  }
  snprintf(send_lvgl,9,"%d",ble_scan_count);
  lv_label_set_text(Send_ui->screen_label_17, send_lvgl);
  ble_stack_deinit();//释放BLE
  vTaskDelete(NULL);
}
static void lvgl_obj_event_handler(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_ui *ui = (lv_ui *)e->user_data;
  lv_obj_t * module = e->current_target;
  switch (code)
  {
    case LV_EVENT_CLICKED:
    {
      if(module == ui->screen_slider_1)
      {
        uint8_t value = lv_slider_get_value(module);
        setBrightnes(value);
      }
      break;
    }
    default:
      break;
  }
}
void tp_event_callback(uint16_t x,uint16_t y)
{
  char str[12] = {""};
  snprintf(str,11,"(%hd,%hd)",x,y);
  lv_label_set_text(user_ui.screen_label_24, str);
}
void tca9554_init(void)
{
  esp_io_expander_new_i2c_tca9554(user_i2c_port0_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);

  esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_6, 1);
}
void user_app_init(void)
{
  setup_ui(&user_ui);
  user_button_init();
  adc_bsp_init();
  i2c_rtc_setup();
  i2c_rtc_setTime(2025,7,7,18,43,30);
  i2c_qmi_setup();
  espwifi_init();
  user_audio_bsp_init();
  tca9554_init();
#if SD_CARD_EN
  _sdcard_init();
  xTaskCreatePinnedToCore(example_sdcard_task, "example_sdcard_task", 2 * 1024, &user_ui, 2, NULL,0);      //sd card测试
#endif 
  xTaskCreatePinnedToCore(example_user_task, "example_user_task", 4 * 1024, &user_ui, 2, NULL,0);          //用户事件
xTaskCreatePinnedToCore(example_button_task, "example_button_task", 4 * 1024, &user_ui, 2, NULL,0);      //按钮事件  
  xTaskCreatePinnedToCore(example_color_task, "example_color_task", 4 * 1024, &user_ui, 2, NULL,0);        //RGB颜色测试
  xTaskCreatePinnedToCore(example_scan_wifi_ble_task, "example_scan_wifi_ble_task", 3 * 1024,&user_ui, 2, NULL,0);   
  xTaskCreatePinnedToCore(i2s_audio_Test, "i2s_audio_Test", 4 * 1024, &audio_Test_flag, 2, NULL,0);           
  /*even add*/
  lv_obj_add_event_cb(user_ui.screen_slider_1, lvgl_obj_event_handler, LV_EVENT_ALL, &user_ui); 
}



