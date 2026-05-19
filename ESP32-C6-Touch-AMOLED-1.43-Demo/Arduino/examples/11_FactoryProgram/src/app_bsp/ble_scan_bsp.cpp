#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include "ble_scan_bsp.h"

BLEScan* pBLEScan = nullptr;
static uint8_t is_BLEScanInitOK_ = 0;
static int BLEScanQuantity_ = 0;

class MyBLEScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
  }
};

void BLEScan_DevicesInit(uint32_t scanDuration) {
  int bleCount = 0;
  BLEScanResults* scanResults = nullptr;
  BLEDevice::init("");

  pBLEScan = BLEDevice::getScan();
  if (pBLEScan == nullptr) {
    is_BLEScanInitOK_ = 0;
    BLEDevice::deinit();
    return;
  }

  pBLEScan->setAdvertisedDeviceCallbacks(new MyBLEScanCallback());
  pBLEScan->setActiveScan(false);
  pBLEScan->setInterval(100);    
  pBLEScan->setWindow(50);       

  scanResults = pBLEScan->start(scanDuration / 1000, true);
  if (scanResults != nullptr) {
    bleCount = scanResults->getCount();
    is_BLEScanInitOK_ = 1;
  } else {
    is_BLEScanInitOK_ = 0;
  }

  is_BLEScanInitOK_ = 1;
  BLEScanQuantity_ = bleCount;

  if (pBLEScan != nullptr) {
    pBLEScan->stop(); 
    pBLEScan->clearResults(); 
  }
  BLEDevice::deinit();
}

int BLEScan_GetScanQuantity(void) {return BLEScanQuantity_;}
uint8_t BLEScan_GetStatus(void) {return is_BLEScanInitOK_;}


#if 0
Serial.println("-----------------------------------------------------");
Serial.printf("设备地址：%s\n", advertisedDevice.getAddress().toString().c_str());
Serial.printf("设备名称：%s\n", advertisedDevice.getName().length() > 0 ? advertisedDevice.getName().c_str() : "无名称");
Serial.printf("信号强度：%d dBm\n", advertisedDevice.getRSSI());
Serial.printf("是否支持BLE连接：%s\n", advertisedDevice.isConnectable() ? "是" : "否");
Serial.printf("广播数据长度：%d 字节\n", advertisedDevice.getPayloadLength());
#endif