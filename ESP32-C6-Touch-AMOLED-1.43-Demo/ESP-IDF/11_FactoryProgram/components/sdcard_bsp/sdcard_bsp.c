#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sdcard_bsp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "user_config.h"

#define SDlist "/sdcard"        //sd card 目录

static const char *TAG = "_sdcard";

sdmmc_card_t *card_host = NULL; //句柄
EventGroupHandle_t sdcard_even_ = NULL;
sdcard_bsp_t user_sdcard_bsp;

void _sdcard_init(void)
{
  sdcard_even_ = xEventGroupCreate();
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,    //如果挂靠失败，创建分区表并格式化SD卡
    .max_files = 5,                    //打开文件最大数
    .allocation_unit_size = 512       //类似扇区大小
  };
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_CS;
  slot_config.host_id = SDSPI_HOST;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SDSPI_HOST;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdspi_mount(SDlist, &host, &slot_config, &mount_config, &card_host)); //挂靠SD卡
  if(card_host != NULL)
  {
    sdmmc_card_print_info(stdout, card_host); //Print out the information of the card.
    user_sdcard_bsp.sdcard_size = (float)(card_host->csd.capacity)/2048/1024; //G
    xEventGroupSetBits(sdcard_even_,0x01);
  }
}

/* Write data
path: Path
data: Data */ 
esp_err_t sdcard_file_write(const char *path, const char *data)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "w"); //Obtain the path address
  if(f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fprintf(f, data); 
  fclose(f);
  return ESP_OK;
}
/*
Read data
path: path */
esp_err_t sdcard_file_read(const char *path, char *buffer, size_t *out_len)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fseek(f, 0, SEEK_END);     //Move the pointer to the very end.
  uint32_t unlen = ftell(f);
  //fgets(pxbuf, unlen, f); //read characters from file
  fseek(f, 0, SEEK_SET); //Move the pointer to the very beginning.
  uint32_t poutLen = fread((void *)buffer,1,unlen,f);
  if(out_len != NULL)
  *out_len = poutLen;
  fclose(f);
  return ESP_OK;
}