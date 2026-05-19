#include "i2c_equipment.h"
#include "SensorPCF85063.hpp"
#include "SensorQMI8658.hpp"
#include "i2c_bsp.h"
#include <stdio.h>

SensorPCF85063 rtc;
SensorQMI8658  qmi;

IMUdata acc;
IMUdata gyr;

static I2cMasterBus           *i2cbus_   = NULL;
static i2c_master_dev_handle_t i2cRTCdev = NULL;
static uint8_t                 i2cRTCAddress;

static i2c_master_dev_handle_t i2cQMIdev = NULL;
static uint8_t                 i2cQMIAddress;

static i2c_master_dev_handle_t i2cAudio = NULL;

static uint32_t hal_callback(SensorCommCustomHal::Operation op, void *param1, void *param2) {
    switch (op) {
    // Set GPIO mode
    case SensorCommCustomHal::OP_PINMODE: {
        uint8_t       pin  = reinterpret_cast<uintptr_t>(param1);
        uint8_t       mode = reinterpret_cast<uintptr_t>(param2);
        gpio_config_t config;
        memset(&config, 0, sizeof(config));
        config.pin_bit_mask = 1ULL << pin;
        switch (mode) {
        case INPUT:
            config.mode = GPIO_MODE_INPUT;
            break;
        case OUTPUT:
            config.mode = GPIO_MODE_OUTPUT;
            break;
        }
        config.pull_up_en   = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type    = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&config));
    } break;
    // Set GPIO level
    case SensorCommCustomHal::OP_DIGITALWRITE: {
        uint8_t pin   = reinterpret_cast<uintptr_t>(param1);
        uint8_t level = reinterpret_cast<uintptr_t>(param2);
        gpio_set_level((gpio_num_t) pin, level);
    } break;
    // Read GPIO level
    case SensorCommCustomHal::OP_DIGITALREAD: {
        uint8_t pin = reinterpret_cast<uintptr_t>(param1);
        return gpio_get_level((gpio_num_t) pin);
    } break;
    // Get the current running milliseconds
    case SensorCommCustomHal::OP_MILLIS:
        return (uint32_t) (esp_timer_get_time() / 1000LL);
    // Delay in milliseconds
    case SensorCommCustomHal::OP_DELAY: {
        if (param1) {
            uint32_t ms = reinterpret_cast<uintptr_t>(param1);
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
    } break;
    // Delay in microseconds
    case SensorCommCustomHal::OP_DELAYMICROSECONDS: {
        uint32_t us = reinterpret_cast<uintptr_t>(param1);
        esp_rom_delay_us(us);
    } break;
    default:
        break;
    }
    return 0;
}

static bool I2cDevCallback(uint8_t address, uint8_t reg, uint8_t *buf, size_t len, bool writeReg, bool isWrite) {
    int                     ret;
    i2c_master_dev_handle_t dev_handle = NULL;
    if (i2cRTCAddress == address)
        dev_handle = i2cRTCdev;
    else if (i2cQMIAddress == address)
        dev_handle = i2cQMIdev;
    if (isWrite) {
        if (writeReg) {
            ret = i2cbus_->i2c_write_buff(dev_handle, reg, buf, len);
        } else {
            ret = i2cbus_->i2c_write_buff(dev_handle, -1, buf, len);
        }
    } else {
        if (writeReg) {
            ret = i2cbus_->i2c_read_buff(dev_handle, reg, buf, len);
        } else {
            ret = i2cbus_->i2c_read_buff(dev_handle, -1, buf, len);
        }
    }
    return (ret == ESP_OK) ? true : false;
}

void I2cRtcSetup(I2cMasterBus *i2cbus, uint8_t dev_addr) {
    if (i2cbus_ == NULL) {
        i2cbus_ = i2cbus;
    }
    if (i2cRTCdev == NULL) {
        i2c_master_bus_handle_t BusHandle = i2cbus_->Get_I2cBusHandle();
        i2c_device_config_t     dev_cfg   = {};
        dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz              = 300000;
        dev_cfg.device_address            = dev_addr;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(BusHandle, &dev_cfg, &i2cRTCdev));
        i2cRTCAddress = dev_addr;
    }
    if (rtc.begin(I2cDevCallback)) {
        ESP_LOGI("rtc", "InitWill");
    } else {
        ESP_LOGE("rtc", "InitFailure");
    }
}

void Set_I2cRtcTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    rtc.setDateTime(year, month, day, hour, minute, second);
}

RtcDateTime_t Get_I2cRtcTime(void) {
    RtcDateTime_t time;
    RTC_DateTime  datetime = rtc.getDateTime();
    time.year              = datetime.getYear();
    time.month             = datetime.getMonth();
    time.day               = datetime.getDay();
    time.hour              = datetime.getHour();
    time.minute            = datetime.getMinute();
    time.second            = datetime.getSecond();
    time.week              = datetime.getWeek();
    return time;
}

void I2cRtcTask(void *arg) {
    for (;;) {
        RTC_DateTime datetime = rtc.getDateTime();
        printf(" Year :%d", datetime.getYear());
        printf(" Month:%d", datetime.getMonth());
        printf(" Day :%d", datetime.getDay());
        printf(" Hour:%d", datetime.getHour());
        printf(" Minute:%d", datetime.getMinute());
        printf(" Sec :%d", datetime.getSecond());
        printf(" Week:%d", datetime.getWeek());
        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void I2cQmiSetup(I2cMasterBus *i2cbus, uint8_t dev_addr) {
    if (i2cbus_ == NULL) {
        i2cbus_ = i2cbus;
    }
    if (i2cQMIdev == NULL) {
        i2c_master_bus_handle_t BusHandle = i2cbus_->Get_I2cBusHandle();
        i2c_device_config_t     dev_cfg   = {};
        dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz              = 300000;
        dev_cfg.device_address            = dev_addr;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(BusHandle, &dev_cfg, &i2cQMIdev));
        i2cQMIAddress = dev_addr;
    }
    if (qmi.begin(I2cDevCallback, hal_callback, i2cQMIAddress)) {
        ESP_LOGI("qmi", "InitWill");
    }
    ESP_LOGI("qmi", "ID:%02x", qmi.getChipID());
    if (qmi.selfTestAccel()) {
        ESP_LOGI("qmi", "Accelerometer self-test successful");
    } else {
        ESP_LOGE("qmi", "Accelerometer self-test failed!");
    }
    if (qmi.selfTestGyro()) {
        ESP_LOGI("qmi", "Gyroscope self-test successful");
    } else {
        if(qmi.selfTestGyro()) {
            ESP_LOGI("qmi", "Gyroscope self-test successful");
        } else {
            ESP_LOGE("qmi", "Gyroscope self-test failed!");
        }
    }
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz, SensorQMI8658::LPF_MODE_0);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_64DPS, SensorQMI8658::GYR_ODR_896_8Hz, SensorQMI8658::LPF_MODE_3);
    qmi.enableGyroscope();
    qmi.enableAccelerometer();

    // Print register configuration information
    qmi.dumpCtrlRegister();
}

QmiDate_t Get_I2cQmiPosture(void) {
    QmiDate_t imuData;
    memset(&imuData, 0, sizeof(QmiDate_t));
    if (qmi.getDataReady()) {
        if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
            imuData.accx = acc.x;
            imuData.accy = acc.y;
            imuData.accz = acc.z;
        }
        if (qmi.getGyroscope(gyr.x, gyr.y, gyr.z)) {
            imuData.gyrox = gyr.x;
            imuData.gyroy = gyr.y;
            imuData.gyroz = gyr.z;
        }
    }
    return imuData;
}

void I2cQmiTask(void *arg) {
    for (;;) {
        if (qmi.getDataReady()) {
            if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
                // Print to serial plotter
                printf("ACCEL.x:%.2f,ACCEL.y:%.2f,ACCEL.z:%.2f Unit:g\n", acc.x, acc.y, acc.z);
            }
            if (qmi.getGyroscope(gyr.x, gyr.y, gyr.z)) {
                // Print to serial plotter
                printf("GYRO.x:%.2f,GYRO.y:%.2f,GYRO.z:%.2f Unit:degrees/sec\n", gyr.x, gyr.y, gyr.z);
                // Serial.print(" GYRO.x:"); Serial.print(gyr.x); Serial.println(" degrees/sec");
                // Serial.print(",GYRO.y:"); Serial.print(gyr.y); Serial.println(" degrees/sec");
                // Serial.print(",GYRO.z:"); Serial.print(gyr.z); Serial.println(" degrees/sec");
            }
            printf("Temperature: %.2f Unit:degrees C\n", qmi.getTemperature_C());
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
