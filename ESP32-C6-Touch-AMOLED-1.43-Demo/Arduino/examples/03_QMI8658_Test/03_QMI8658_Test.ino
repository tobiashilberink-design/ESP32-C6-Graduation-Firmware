#include "qmi_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void setup()
{
  Serial.begin(115200);
  qmi_init(); //qmi init
  xTaskCreatePinnedToCore(i2c_qmi_loop_task, "i2c_qmi_loop_task", 3 * 1024, NULL , 2, NULL,0); //rtc read data
}

void loop() 
{
  
}
