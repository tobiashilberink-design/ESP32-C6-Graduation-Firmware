#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bsp.h"
#include "user_config.h"
#include "i2c_equipment.h"

extern "C" void app_main(void)
{
  i2c_master_init();
  i2c_qmi_setup();
  xTaskCreatePinnedToCore(i2c_qmi_loop_task, "i2c_qmi_loop_task", 3 * 1024, NULL , 2, NULL,0); 
}
