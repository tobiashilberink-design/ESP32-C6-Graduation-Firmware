#ifndef ROTARY_ENC_H
#define ROTARY_ENC_H

#include <Arduino.h>

// Out A = CLK, Out B = DT, SW = button — all pulled high internally, GND on encoder completes circuit
#define ROTARY_CLK_PIN  15
#define ROTARY_DT_PIN   16
#define ROTARY_SW_PIN    17

void rotary_enc_init(void);
int  rotary_enc_get_position(void);
bool rotary_enc_button_pressed(void);

#endif
