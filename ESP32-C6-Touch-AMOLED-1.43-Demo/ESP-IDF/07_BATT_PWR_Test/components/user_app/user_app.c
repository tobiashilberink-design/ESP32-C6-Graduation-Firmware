#include <stdio.h>
#include "user_app.h"
#include "i2c_bsp.h"
#include "user_config.h"
#include "freertos/FreeRTOS.h"
#include "button_bsp.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_io_expander_tca9554.h"

#include "gui_guider.h"

lv_ui user_ui;

static esp_io_expander_handle_t io_expander = NULL;

static void example_button_task(void* parmeter)
{
  lv_ui *task_ui = (lv_ui *)parmeter;
  uint8_t even_flag = 0x01;
  uint8_t ticks = 0;
  for (;;)
  {
    EventBits_t even = xEventGroupWaitBits(key_groups,BIT_EVEN_ALL,pdTRUE,pdFALSE,pdMS_TO_TICKS(2 * 1000));
    if(READ_BIT(even,12)) //长按 pwr
    {
      if(READ_BIT(even_flag,1)) //获取标志位
      {
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0);
        lv_label_set_text(task_ui->screen_label_1,"Power on failed");
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
  }
}

void tca9554_init(void)
{
  esp_io_expander_new_i2c_tca9554(user_i2c_port0_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);

  esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io_expander,IO_EXPANDER_PIN_NUM_6, 1);
}
void user_app_init(void)
{
  setup_ui(&user_ui);
  user_button_init();
  tca9554_init();
  lv_label_set_text(user_ui.screen_label_1,"Power on succeeded");
  xTaskCreatePinnedToCore(example_button_task, "example_button_task", 4 * 1024, &user_ui, 2, NULL,0);      //按钮事件  
}



