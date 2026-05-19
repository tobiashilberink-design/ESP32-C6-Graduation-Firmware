#ifndef I2C_EQUIPMENT_H
#define I2C_EQUIPMENT_H

#include "i2c_bsp.h"

typedef struct 
{
  	int year;
  	int month;
  	int day;
  	int hour;
  	int minute;
  	int second;
  	int week;
}RtcDateTime_t;

typedef struct 
{
  	float accx;
  	float accy;
  	float accz;
  	float gyrox;
  	float gyroy;
  	float gyroz;
}QmiDate_t;

void I2cRtcSetup(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Set_I2cRtcTime(uint16_t year,uint8_t month,uint8_t day,uint8_t hour,uint8_t minute,uint8_t second);
RtcDateTime_t Get_I2cRtcTime(void);
void I2cRtcTask(void *arg);

void I2cQmiSetup(I2cMasterBus *i2cbus,uint8_t dev_addr);
void I2cQmiTask(void *arg);
QmiDate_t  Get_I2cQmiPosture(void);

#endif 
