/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "i2c_bsp.h"
#include "user_audio_bsp.h"
#include "esp_io_expander_tca9554.h"

esp_io_expander_handle_t io_expander = NULL;


void app_main(void)
{
  i2c_master_Init();
  esp_io_expander_new_i2c_tca9554(user_i2c_port0_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
  esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);

  user_audio_bsp_init();
  xTaskCreatePinnedToCore(i2s_echo, "i2s_echo", 4 * 1024, NULL, 2, NULL ,0); //运行于内核_0 
}
