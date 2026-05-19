#pragma once
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

void BLEScan_DevicesInit(uint32_t scanDuration);
int BLEScan_GetScanQuantity(void);
uint8_t BLEScan_GetStatus(void);