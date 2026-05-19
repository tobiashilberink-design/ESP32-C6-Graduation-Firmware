#include "adc_bsp.h"
#include "esp_timer.h"

static volatile int32_t enc_position   = 0;
static volatile uint8_t enc_prev_state = 0;
static volatile bool    btn_pending    = false;

// Full quadrature state machine: index = (prev_state << 2) | curr_state
// Valid CW  transitions: 11->10, 10->00, 00->01, 01->11
// Valid CCW transitions: 11->01, 01->00, 00->10, 10->11
// All other transitions are bounce — ignored (0)
static const int8_t dir_table[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

static void IRAM_ATTR enc_isr(void)
{
    uint8_t curr = (digitalRead(ROTARY_CLK_PIN) << 1) | digitalRead(ROTARY_DT_PIN);
    enc_position += dir_table[(enc_prev_state << 2) | curr];
    enc_prev_state = curr;
}

static void IRAM_ATTR sw_isr(void)
{
    static uint32_t last_us = 0;
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    if (now_us - last_us > 50000) {  // 50 ms debounce
        btn_pending = true;
        last_us = now_us;
    }
}

void rotary_enc_init(void)
{
    pinMode(ROTARY_CLK_PIN, INPUT_PULLUP);
    pinMode(ROTARY_DT_PIN,  INPUT_PULLUP);
    pinMode(ROTARY_SW_PIN,  INPUT_PULLUP);

    // Capture real pin state before enabling interrupts so the first
    // transition is evaluated against the correct starting state
    enc_prev_state = (digitalRead(ROTARY_CLK_PIN) << 1) | digitalRead(ROTARY_DT_PIN);

    attachInterrupt(digitalPinToInterrupt(ROTARY_CLK_PIN), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ROTARY_DT_PIN),  enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ROTARY_SW_PIN),  sw_isr,  FALLING);
}

int rotary_enc_get_position(void)
{
    return enc_position;
}

bool rotary_enc_button_pressed(void)
{
    if (btn_pending) {
        btn_pending = false;
        return true;
    }
    return false;
}
