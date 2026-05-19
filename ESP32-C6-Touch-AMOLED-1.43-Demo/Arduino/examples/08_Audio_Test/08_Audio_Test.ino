#include <Arduino.h>
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "driver/i2c_master.h"
#include "src/tca9554/esp_io_expander_tca9554.h"

esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;
esp_io_expander_handle_t io_expander = NULL;
// I2C
i2c_master_bus_handle_t user_i2c_port0_handle = NULL;
#define ESP32_SCL_NUM (GPIO_NUM_8)
#define ESP32_SDA_NUM (GPIO_NUM_18)

static uint32_t i2c_data_pdMS_TICKS = 0;
static uint32_t i2c_done_pdMS_TICKS = 0;

void i2c_master_Init(void)
{
  i2c_data_pdMS_TICKS = pdMS_TO_TICKS(5000);
  i2c_done_pdMS_TICKS = pdMS_TO_TICKS(1000);

  /*i2c_port 0 init*/
  i2c_master_bus_config_t i2c_bus_config = {};
    i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_bus_config.i2c_port = I2C_NUM_0;
    i2c_bus_config.scl_io_num = ESP32_SCL_NUM;
    i2c_bus_config.sda_io_num = ESP32_SDA_NUM;
    i2c_bus_config.glitch_ignore_cnt = 7;
    i2c_bus_config.flags.enable_internal_pullup = true;

  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &user_i2c_port0_handle));
}

uint8_t *data_ptr = NULL;
void setup()
{
  delay(2000);
  i2c_master_Init();
  esp_io_expander_new_i2c_tca9554(user_i2c_port0_handle, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
  esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);
  set_codec_board_type("C6_AMOLED_1_43");
  codec_init_cfg_t codec_dev;
  init_codec(&codec_dev);
  playback = get_playback_handle();
  record = get_record_handle();

  esp_codec_dev_set_out_vol(playback, 90.0);        //设置90声音大小
  esp_codec_dev_set_in_gain(record, 35.0);          //设置录音时的增益
  data_ptr = (uint8_t *)heap_caps_malloc(1024 * sizeof(uint8_t), MALLOC_CAP_DEFAULT);
  esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = 24000;
    fs.channel = 2;
    fs.bits_per_sample = 16;
  
  esp_codec_dev_open(playback, &fs);                //打开播放
  esp_codec_dev_open(record, &fs);                  //打开录音
}

void loop()
{
  if(ESP_CODEC_DEV_OK == esp_codec_dev_read(record, data_ptr, 1024))
  {
    esp_codec_dev_write(playback, data_ptr, 1024);
  }
}
