#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <WiFi.h>
#include "wifi_scan_bsp.h"

static uint8_t is_WifiScanInitOK_ = 0;
static int WifiScanQuantity_ = 0;


void WifiScan_NetworksInit(void) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(100));
  int wifiCount = WiFi.scanNetworks();

  if (wifiCount == WIFI_SCAN_FAILED) {
    is_WifiScanInitOK_ = 0;
  }
  is_WifiScanInitOK_ = 1;
  WifiScanQuantity_ = wifiCount;
  WiFi.mode(WIFI_OFF);
}

int WifiScan_GetScanQuantity(void) {return WifiScanQuantity_;}
uint8_t WifiScan_GetStatus(void) {return is_WifiScanInitOK_;}