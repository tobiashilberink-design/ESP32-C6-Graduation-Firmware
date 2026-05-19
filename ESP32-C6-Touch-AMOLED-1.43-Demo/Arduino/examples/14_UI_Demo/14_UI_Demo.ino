#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "esp_timer.h"

#define ENC_CLK 15
#define ENC_DT  16
#define ENC_SW  17

static volatile int32_t enc_pos  = 0;
static volatile uint8_t enc_prev = 0;
static volatile bool    btn_flag = false;

static const int8_t dir_table[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

void IRAM_ATTR enc_isr(void)
{
    uint8_t curr = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    enc_pos += dir_table[(enc_prev << 2) | curr];
    enc_prev = curr;
}

void IRAM_ATTR sw_isr(void)
{
    static uint32_t last_us = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - last_us > 50000) {
        btn_flag = true;
        last_us  = now;
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(ENC_CLK, INPUT_PULLUP);
    pinMode(ENC_DT,  INPUT_PULLUP);
    pinMode(ENC_SW,  INPUT_PULLUP);
    enc_prev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT),  enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW),  sw_isr,  FALLING);

    lvgl_port_init();
}

void loop()
{
    static int32_t last_enc = 0;
    int32_t enc = enc_pos;

    if (enc != last_enc) {
        on_encoder_delta(enc - last_enc);
        last_enc = enc;
    }

    if (btn_flag) {
        btn_flag = false;
        on_button_press();
    }
}
