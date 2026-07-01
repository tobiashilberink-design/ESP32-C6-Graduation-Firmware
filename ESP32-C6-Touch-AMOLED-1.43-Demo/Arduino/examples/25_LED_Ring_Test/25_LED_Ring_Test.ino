/*  25_LED_Ring_Test.ino
 *
 *  Bare-minimum WS2812B ring test on GPIO 2.
 *  Wiring:  ring DIN → GPIO 2,  ring 5V (or 3.3V) → board 5V/3.3V,
 *           ring GND → board GND (common ground is essential).
 *
 *  Data must enter the DIN side (follow the arrows on the ring PCB).
 *
 *  Sequence each run:
 *    1) all 24 LEDs RED for 1 s
 *    2) all 24 GREEN for 1 s
 *    3) all 24 BLUE for 1 s
 *    4) a single lit pixel walks around the ring once
 *
 *  If only the first LED ever lights, the data is not propagating past
 *  pixel 1 — broken DOUT→DIN joint, reversed direction, or marginal 3.3 V
 *  data into a 5 V-powered ring. Serial prints which step is running.
 */

#include <Adafruit_NeoPixel.h>

#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

static void fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, ring.Color(r, g, b));
    ring.show();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    ring.begin();
    ring.setBrightness(60);   /* modest brightness — safe on USB power */
    ring.clear();
    ring.show();
    Serial.println("LED ring test ready.");
}

void loop() {
    Serial.println("RED");   fill(255, 0, 0);   delay(1000);
    Serial.println("GREEN"); fill(0, 255, 0);   delay(1000);
    Serial.println("BLUE");  fill(0, 0, 255);   delay(1000);

    Serial.println("chase");
    for (int i = 0; i < LED_COUNT; i++) {
        ring.clear();
        ring.setPixelColor(i, ring.Color(255, 255, 255));
        ring.show();
        Serial.printf("  pixel %d\n", i);
        delay(120);
    }
}
