/*  24_Button_Test.ino
 *
 *  Bare-minimum button test on GPIO 1.
 *  Wiring:  one button leg → GPIO 1,  other leg → GND.
 *  No resistor — the internal pull-up holds the pin HIGH; pressing pulls
 *  it LOW. Open the Serial Monitor at 115200 baud.
 *
 *  4-pin tactile button: the two pins on the SAME side are always joined —
 *  use two DIAGONAL pins (or one pin from each side) or it will never read.
 *
 *  The loop also prints the raw pin level once a second so you can see
 *  whether it idles HIGH (1) and drops to LOW (0) when pressed.
 */

#define BTN_PIN 1

void setup() {
    Serial.begin(115200);
    delay(300);
    pinMode(BTN_PIN, INPUT_PULLUP);
    Serial.println("Button test ready. Press the button on GPIO 9.");
}

void loop() {
    static bool     confirmed = false;   /* debounced state, true = pressed */
    static bool     raw_last  = false;
    static uint32_t stable_ms = 0;
    static uint32_t presses   = 0;

    bool     raw = (digitalRead(BTN_PIN) == LOW);
    uint32_t now = millis();

    if (raw != raw_last) {               /* level moved — restart stability timer */
        raw_last  = raw;
        stable_ms = now;
    } else if (now - stable_ms >= 8 && raw != confirmed) {
        confirmed = raw;                 /* level stable for 8 ms → accept it */
        if (confirmed) {
            presses++;
            Serial.printf("PRESSED   (count = %lu)\n", (unsigned long)presses);
        } else {
            Serial.println("released");
        }
    }

    /* Heartbeat: raw level once a second. Should read 1 idle, 0 pressed.
       If it never changes, the wiring/button is the problem, not the code. */
    static uint32_t hb_ms = 0;
    if (now - hb_ms >= 1000) {
        Serial.printf("raw level = %d\n", digitalRead(BTN_PIN));
        hb_ms = now;
    }
}
