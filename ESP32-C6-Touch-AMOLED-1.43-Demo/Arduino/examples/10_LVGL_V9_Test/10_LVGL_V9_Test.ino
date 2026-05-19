#include "user_config.h"
#include "lvgl_port.h"
void setup()
{
  Serial.begin(115200);
  lvgl_port_init();
}
void loop()
{
#ifdef  Backlight_Testing
  delay(2000);
  set_amoled_backlight(255);
  delay(2000);
  set_amoled_backlight(200);
  delay(2000);
  set_amoled_backlight(150);
  delay(2000);
  set_amoled_backlight(100);
  delay(2000);
  set_amoled_backlight(50);
  delay(2000);
  set_amoled_backlight(0);
#endif
}
