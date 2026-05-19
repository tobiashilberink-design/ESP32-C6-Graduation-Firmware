#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"


void setup()
{
  Serial.begin(115200);
  lvgl_port_init();
}
void loop()
{
  static unsigned long last = 0;
  if (millis() - last >= 1000) {
    Serial.println("Serial OK - tick");
    last = millis();
  }

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