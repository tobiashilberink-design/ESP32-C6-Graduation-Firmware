#include <esp_err.h>
#include <lvgl.h>
#include <demos/lv_demos.h>

#include "user_config.h"
#include "src/port_bsp/display_bsp.h"
#include "src/port_bsp/i2c_bsp.h"
#include "src/app_bsp/lvgl_bsp.h"
#include "src/ui_bsp/generated/gui_guider.h"
#include "src/port_bsp/adc_bsp.h"
#include "src/port_bsp/button_bsp.h"
#include "src/port_bsp/i2c_equipment.h"
#include "src/ExternLib/tca9554/esp_io_expander_tca9554.h"
#include "src/port_bsp/sdcard_bsp.h"
#include "src/app_bsp/wifi_scan_bsp.h"
#include "src/app_bsp/ble_scan_bsp.h"
#include "src/port_bsp/codec_bsp.h"

I2cMasterBus i2cbus(8, 18, 0);
DisplayPort *display = NULL; /*必须指针 new*/
CustomSDPort *sdport = NULL; /*必须指针 new*/
lv_ui src_ui;
CodecPort *codecport = NULL;
esp_io_expander_handle_t io_expander = NULL;
EventGroupHandle_t ContsGroups;
bool is_music = 0;

void Tca9554_Init(void) {
  ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(i2cbus.Get_I2cBusHandle(), ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, 1));
}

void Custom_ColorTask(void *arg) {
  if (Lvgl_lock(-1)) {
    lv_obj_clear_flag(src_ui.screen_carousel_1, LV_OBJ_FLAG_SCROLLABLE);  //unmovable
    lv_obj_clear_flag(src_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_3, LV_OBJ_FLAG_HIDDEN);
    Lvgl_unlock();
  }
  vTaskDelay(pdMS_TO_TICKS(1500));
  if (Lvgl_lock(-1)) {
    lv_obj_clear_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_3, LV_OBJ_FLAG_HIDDEN);
    Lvgl_unlock();
  }
  vTaskDelay(pdMS_TO_TICKS(1500));
  if (Lvgl_lock(-1)) {
    lv_obj_clear_flag(src_ui.screen_img_3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    Lvgl_unlock();
  }
  vTaskDelay(pdMS_TO_TICKS(1500));
  if (Lvgl_lock(-1)) {
    lv_obj_clear_flag(src_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(src_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(src_ui.screen_carousel_1, LV_OBJ_FLAG_SCROLLABLE);  //removable
    Lvgl_unlock();
  }
  vTaskDelete(NULL);
}

void Custom_PWRButtonTask(void *arg) {
  for (;;) {
    EventBits_t even = xEventGroupWaitBits(PWRButtonGroups, GroupSetBitsMax, pdTRUE, pdFALSE, pdMS_TO_TICKS(2 * 1000));
    if (even & GroupBit2) {
      ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander,IO_EXPANDER_PIN_NUM_6, 0));
    }
  }
}

void Custom_BottButtonTask(void *arg) {
  bool is_TouchCont = 1;
  for (;;) {
    EventBits_t even = xEventGroupWaitBits(BootButtonGroups, GroupSetBitsMax, pdTRUE, pdFALSE, pdMS_TO_TICKS(2 * 1000));
    if (even & GroupBit0) {  // 单击
      if (is_TouchCont) {
        is_TouchCont = 0;
        if (Lvgl_lock(-1)) {
          lv_obj_clear_flag(src_ui.screen_carousel_1, LV_OBJ_FLAG_SCROLLABLE);  //unmovable
          lv_obj_clear_flag(src_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(src_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
          Lvgl_unlock();
          xEventGroupSetBits(ContsGroups,GroupBit0);
        }
      } else {
        is_TouchCont = 1;
        if (Lvgl_lock(-1)) {
          lv_obj_add_flag(src_ui.screen_carousel_1, LV_OBJ_FLAG_SCROLLABLE);  //removable
          lv_obj_clear_flag(src_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(src_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
          Lvgl_unlock();
          xEventGroupClearBits(ContsGroups,GroupBit0);
          xEventGroupSetBits(ContsGroups,GroupBit1);
        }
      } 
    } else if(even & GroupBit2) { /*music*/
      is_music = 1;
      xEventGroupClearBits(ContsGroups,GroupBit2);
      xEventGroupSetBits(ContsGroups,GroupBit3);
    } else if(even & GroupBit3) {
      is_music = 0;
      xEventGroupClearBits(ContsGroups,GroupBit3);
      xEventGroupSetBits(ContsGroups,GroupBit2);
    } else if(even & GroupBit1) { /*sdcard*/
      xEventGroupSetBits(ContsGroups,GroupBit4);
    }
  }
}

#if ESP32_SDCARD_EN
void Custom_SdcardTestTask(void *arg) {
  bool is_sdcard_test = 1;
  char lvglstr[30] = {""};
  char sdcard_read[60] = {""};
  char sdcard_write[60] = {"Hello, welcome to Waveshare Electronics."};
  if (sdport->SDPort_GetStatus()) {
    snprintf(lvglstr, 30, "%.2fG", sdport->SDPort_GetSdcardSize());
  } else {
    strcpy(lvglstr,"NULL");
  }
  if (Lvgl_lock(-1)) {
    lv_label_set_text(src_ui.screen_label_6, lvglstr);
    Lvgl_unlock();
  }
  for(;;) {
    EventBits_t even = xEventGroupWaitBits(ContsGroups, (GroupBit4), pdTRUE, pdFALSE, pdMS_TO_TICKS(2 * 1000));
    if(even & GroupBit4) {
      if(ESP_OK != sdport->SDPort_WriteFile("/sdcard/sdcardTest.txt",sdcard_write,strlen(sdcard_write))) {
        strcpy(lvglstr,"sdcard test failed");
      }
      if(ESP_OK != sdport->SDPort_ReadFile("/sdcard/sdcardTest.txt",(uint8_t *)sdcard_read,NULL)) {
        strcpy(lvglstr,"sdcard test failed");
      }
      if(!strcmp(sdcard_read,sdcard_write)) {
        strcpy(lvglstr,"sdcard test passed");
      } else {
        strcpy(lvglstr,"sdcard test failed");
      }
      if (Lvgl_lock(-1)) {
        lv_label_set_text(src_ui.screen_label_25, lvglstr);
        Lvgl_unlock();
      }
      is_sdcard_test = 0;
    } else {
      if(0 == is_sdcard_test) {
        if (Lvgl_lock(-1)) {
          lv_label_set_text(src_ui.screen_label_25, "");
          Lvgl_unlock();
        }
        is_sdcard_test = 1;
      }
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
  vTaskDelete(NULL);
}
#endif

void Custom_UserTask(void *arg) {
  uint32_t times = 0;
  uint32_t rtc_time = 0;
  uint32_t qmi_time = 0;
  uint32_t adc_time = 0;
  char lvglstr[30] = { "" };
  for (;;) {
    if (times - adc_time == 10) {  //2s
      adc_time = times;
      snprintf(lvglstr, 30, "%.2fV", Adc_GetBatteryVoltage());
      if (Lvgl_lock(-1)) {
        lv_label_set_text(src_ui.screen_label_7, lvglstr);
        Lvgl_unlock();
      }
    }
    if (times - rtc_time == 5) {  //1s
      rtc_time = times;
      RtcDateTime_t rtc = Get_I2cRtcTime();
      snprintf(lvglstr, 30, "%d/%d/%d %02d:%02d:%02d", rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);
      if (Lvgl_lock(-1)) {
        lv_label_set_text(src_ui.screen_label_10, lvglstr);
        Lvgl_unlock();
      }
    }
    if (times - qmi_time == 1) {  //200ms
      qmi_time = times;
      QmiDate_t qmi = Get_I2cQmiPosture();
      snprintf(lvglstr, 30, "%.2f,%.2f,%.2f (g)", qmi.accx, qmi.accy, qmi.accz);
      if (Lvgl_lock(-1)) {
        lv_label_set_text(src_ui.screen_label_12, lvglstr);
        Lvgl_unlock();
      }
      snprintf(lvglstr, 30, "%.2f,%.2f,%.2f (dps)", qmi.gyrox, qmi.gyroy, qmi.gyroz);
      if (Lvgl_lock(-1)) {
        lv_label_set_text(src_ui.screen_label_19, lvglstr);
        Lvgl_unlock();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    times++;
  }
}

#if ESP32_WIFI_BLE_Scan_EN
void Custom_WifiBTScanTask(void *arg) {
  char lvglstr[10] = { "" };
  WifiScan_NetworksInit();
  if (WifiScan_GetStatus()) {
    snprintf(lvglstr, 10, "%d", WifiScan_GetScanQuantity());
    if (Lvgl_lock(-1)) {
      lv_label_set_text(src_ui.screen_label_21, lvglstr);
      Lvgl_unlock();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
  BLEScan_DevicesInit(5000); /*Set the scanning time to 5 seconds.*/
  if (BLEScan_GetStatus()) {
    snprintf(lvglstr, 10, "%d", BLEScan_GetScanQuantity());
    if (Lvgl_lock(-1)) {
      lv_label_set_text(src_ui.screen_label_17, lvglstr);
      Lvgl_unlock();
    }
  }
  vTaskDelete(NULL);
}
#endif

void Custom_TouchTestTask(void *arg) {
  bool is_touch_init = 1;
  char lvglstr[12] = {""};
  uint16_t x,y;
  for(;;) {
    EventBits_t even = xEventGroupWaitBits(ContsGroups, (GroupBit0 | GroupBit1), pdFALSE, pdFALSE, pdMS_TO_TICKS(2 * 1000));
    if (even & GroupBit0) { //tp conts
      if(is_touch_init) {
        is_touch_init = 0;
        display->Set_ResetTouchQueue();
      } else {
        if(pdTRUE == display->Get_TouchQueueData(&x,&y)) {
          snprintf(lvglstr,12,"(%hd,%hd)",x,y);
          if (Lvgl_lock(-1)) {
            lv_label_set_text(src_ui.screen_label_24, lvglstr);
            Lvgl_unlock();
          }
        }
      }
    } else if(even & GroupBit1) {
      if(0 == is_touch_init) {
        is_touch_init = 1;
      }
      xEventGroupClearBits(ContsGroups,GroupBit1);
    }
  }
}

void Custom_AudioTestTask(void *arg) {
  uint8_t *rec_data_ptr = (uint8_t *)heap_caps_malloc(512 * sizeof(uint8_t), MALLOC_CAP_DEFAULT);
  codecport->CodecPort_SetSpeakerVol(90);
  codecport->CodecPort_SetMicGain(35);
  uint32_t bytes_sizt = 0;   //codecport->CodecPort_GetPcmData();
  for(;;) {
    EventBits_t even = xEventGroupWaitBits(ContsGroups, (GroupBit2 | GroupBit3), pdFALSE, pdFALSE, pdMS_TO_TICKS(2 * 1000));
    if (even & GroupBit2) {
      if(ESP_CODEC_DEV_OK == codecport->CodecPort_EchoRead(rec_data_ptr,512)) {
        codecport->CodecPort_PlayWrite(rec_data_ptr,512);
      }
    } else if(even & GroupBit3) {
      size_t bytes_write = 0;
      uint8_t *data_ptr = codecport->CodecPort_GetPcmData(&bytes_sizt);
      while ((bytes_write < bytes_sizt) && is_music) {
        codecport->CodecPort_PlayWrite(data_ptr, 256);
        data_ptr += 256;
        bytes_write += 256;
      }
    }
  }
}

void Lvgl_Slider_Event_Callback(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t * module = e->current_target;
  switch (code) {
    case LV_EVENT_CLICKED: {
      if(module == src_ui.screen_slider_1) {
        uint8_t Backlight = lv_slider_get_value(module);
        display->Set_Backlight(Backlight);
      }
      break;
    }
    default:
      break;
  }
}

void setup() {
  ContsGroups = xEventGroupCreate();
  Serial.begin(115200);
  Tca9554_Init();
  display = new DisplayPort(i2cbus, 466, 466);
  display->DisplayPort_TouchInit();
  Lvgl_PortInit(*display);
  Adc_PortInit();
  Custom_ButtonInit();
  I2cRtcSetup(&i2cbus, 0x51);
  Set_I2cRtcTime(2026, 1, 1, 0, 0, 0);
  I2cQmiSetup(&i2cbus, 0x6b);
  codecport = new CodecPort(i2cbus,"C6_AMOLED_1_43");
  xEventGroupSetBits(ContsGroups,GroupBit2); /*echo*/
#if ESP32_SDCARD_EN
  sdport = new CustomSDPort("/sdcard", *display);
#endif
  /*lvgl ui init*/
  if (Lvgl_lock(-1)) {
    setup_ui(&src_ui);
    Lvgl_unlock();
  }
  while(0 == gpio_get_level(GPIO_NUM_2)) {
    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  xEventGroupClearBits(PWRButtonGroups,GroupSetBitsMax);
  xEventGroupClearBits(BootButtonGroups,GroupSetBitsMax);
  xTaskCreate(Custom_ColorTask, "Custom_ColorTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(Custom_BottButtonTask, "Custom_BottButtonTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(Custom_PWRButtonTask, "Custom_PWRButtonTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(Custom_UserTask, "Custom_UserTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(Custom_TouchTestTask, "Custom_TouchTestTask", 4 * 1024, NULL, 2, NULL);
  xTaskCreate(Custom_AudioTestTask, "Custom_AudioTestTask", 4 * 1024, NULL, 2, NULL);
#if ESP32_SDCARD_EN
  xTaskCreate(Custom_SdcardTestTask, "Custom_SdcardTestTask", 4 * 1024, NULL, 2, NULL);
#endif
#if ESP32_WIFI_BLE_Scan_EN
  xTaskCreate(Custom_WifiBTScanTask, "Custom_WifiBTScanTask", 4 * 1024, NULL, 2, NULL);
#endif
  lv_obj_add_event_cb(src_ui.screen_slider_1, Lvgl_Slider_Event_Callback, LV_EVENT_ALL, NULL); 
}



void loop() {
}