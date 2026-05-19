#include "adc_bsp.h"

void setup()
{
    Serial.begin(115200);
    delay(2000);

    rotary_enc_init();
    Serial.println("Rotary encoder test ready. Turn or press the knob.");
}

void loop()
{
    static int  last_pos       = 0;
    static unsigned long last_beat = 0;

    // heartbeat every 2 seconds so you can confirm serial is working
    if (millis() - last_beat >= 2000) {
        Serial.printf("Heartbeat — position: %d\n", rotary_enc_get_position());
        last_beat = millis();
    }

    int pos = rotary_enc_get_position();

    if (pos != last_pos) {
        Serial.printf("Position: %d  (delta: %+d)\n", pos, pos - last_pos);
        last_pos = pos;
    }

    if (rotary_enc_button_pressed()) {
        Serial.printf("Button pressed! Position: %d\n", pos);
    }

    delay(10);
}
