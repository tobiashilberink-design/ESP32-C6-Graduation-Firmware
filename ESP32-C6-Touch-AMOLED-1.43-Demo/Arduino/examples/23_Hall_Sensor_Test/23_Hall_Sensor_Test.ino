/*  23_Hall_Sensor_Test.ino
 *
 *  Quadrature test for two KY-003 hall-effect sensors.
 *  Reads the LEVELS of both sensors (not edge order) and runs the same
 *  4-state Gray-code machine a rotary encoder uses, so direction is
 *  correct from any starting position and never locks to an inverted phase.
 *
 *  Wiring
 *  ------
 *  Sensor A  S  →  GPIO 15   (3-pin KY-003: signal is the labelled 'S' pin)
 *  Sensor A  -  →  GND
 *  Sensor A  +  →  3.3V      ← MUST be 3.3V, not 5V
 *
 *  Sensor B  S  →  GPIO 16
 *  Sensor B  -  →  GND
 *  Sensor B  +  →  3.3V
 *
 *  Button       →  one leg to GPIO 17, the other leg to GND.
 *               No resistor needed — internal pull-up is enabled, so the
 *               pin idles HIGH and reads LOW when the button is pressed.
 *
 *  Haptic Buzz (Pimoroni DRV2605L)
 *               2-5V →  3.3V
 *               GND  →  GND
 *               SDA  →  GPIO 18   (shared I2C — same as SH1.0 connector)
 *               SCL  →  GPIO 8
 *               TRIG →  not connected (fired over I2C; GPIO 9 is the C6 boot
 *                       strapping pin and buzzes on reset, so we avoid it)
 *               Requires the "Adafruit DRV2605 Library" to be installed.
 *
 *  For quadrature to work the two sensors must be offset by ~1/4 magnet
 *  pitch so their active regions OVERLAP. The screen shows the live (A,B)
 *  state and an "Illegal" counter: if that counter climbs while you turn,
 *  the signals are not overlapping and true quadrature is not possible
 *  with this geometry (see chat for analog / AS5600 alternatives).
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include "Adafruit_DRV2605.h"
#include "esp_timer.h"
#include "lvgl_port.h"
extern "C" {
#include "lvgl.h"
}

/* ── Hall sensor config ───────────────────────────────────────────────────── */
#define HALL_A_PIN      15
#define HALL_B_PIN      16
#define BTN_PIN         17     /* button: other leg to GND, uses internal pull-up */
#define STEPS_PER_REV   32     /* 8 magnets × 4 quadrature counts per pitch */

/* ── LED ring ─────────────────────────────────────────────────────────────── */
#define LED_PIN         2
#define LED_COUNT       24
#define LED_PCT_MIN     20     /* dimmest orange (%) */
#define LED_PCT_MAX     60     /* brightest orange (%) */
#define LED_PCT_STEP    5      /* % change per quadrature count */

static Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
static bool ring_on  = false;
static int  ring_pct = LED_PCT_MIN;

/* ── Haptic engine (DRV2605L) ─────────────────────────────────────────────── */
#define I2C_SDA_PIN     18     /* shared I2C bus (matches user_config.h) */
#define I2C_SCL_PIN     8

static Adafruit_DRV2605 drv;
static bool haptic_ok = false;

/*  Strongest short effect: ROM library 1, effect 1 = "Strong Click 100%".
 *  Loaded ONCE in setup so each trigger is just a single fast go() over I2C,
 *  letting it retrigger many times per second (a longer "buzz" effect would
 *  overlap itself and smear into one continuous vibration). */
#define HAPTIC_EFFECT  1

static inline void haptic_go() {
    if (haptic_ok) drv.go();   /* waveform already loaded — fast retrigger */
}

/* ── Quadrature state machine ─────────────────────────────────────────────── */
/*  Index = (prev_state << 2) | curr_state, where state = (A << 1) | B.
 *  +1 / -1 for the 8 valid single-bit (Gray-code) transitions,
 *   0 for "no change" and for the 4 illegal double-bit jumps. */
static const int8_t quad_table[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

static volatile int32_t  hall_steps    = 0;   /* +1 = CW, -1 = CCW */
static volatile int8_t   hall_dir      = 0;   /* +1 = CW, -1 = CCW, 0 = idle */
static volatile uint8_t  quad_prev     = 0;
static volatile int32_t  illegal_count = 0;   /* double-bit jumps = no overlap */
static volatile uint32_t hall_edges    = 0;   /* any edge on either sensor */

static inline uint8_t read_state() {
    /* KY-003 is active-LOW (magnet present → LOW). Invert so MAG = 1. */
    uint8_t a = (digitalRead(HALL_A_PIN) == LOW) ? 1 : 0;
    uint8_t b = (digitalRead(HALL_B_PIN) == LOW) ? 1 : 0;
    return (uint8_t)((a << 1) | b);
}

static void IRAM_ATTR quad_isr() {
    hall_edges++;                          /* every edge on A or B */
    uint8_t curr = read_state();
    uint8_t idx  = (uint8_t)((quad_prev << 2) | curr);
    int8_t  step = quad_table[idx];

    if (step != 0) {
        hall_steps += step;
        hall_dir    = step;
    } else if (curr != quad_prev) {
        /* state changed but not a legal Gray step → a state was skipped */
        illegal_count++;
    }
    quad_prev = curr;
}

/* ── Button (polled debounce) ─────────────────────────────────────────────── */
/*  Edge interrupts can't be cleanly debounced against contact bounce, so the
 *  button is polled. We accept a new level only after it has stayed stable for
 *  DEBOUNCE_MS, then count on the released→pressed transition. A real press
 *  lasts far longer than bounce settling, so fast taps still register. */
#define DEBOUNCE_MS   8

static int32_t btn_count = 0;
static bool    btn_state = false;          /* confirmed: true = pressed */

static void poll_button() {
    static bool     raw_last   = false;    /* last raw reading */
    static uint32_t stable_ms  = 0;        /* when raw last changed */

    bool raw = (digitalRead(BTN_PIN) == LOW);
    uint32_t now = millis();

    if (raw != raw_last) {                 /* raw moved — restart stability timer */
        raw_last  = raw;
        stable_ms = now;
        return;
    }
    if (now - stable_ms < DEBOUNCE_MS) return;   /* not stable long enough yet */

    if (raw != btn_state) {                /* stable new level → confirm it */
        btn_state = raw;
        if (btn_state) {                   /* on the make: count + toggle ring */
            btn_count++;
            ring_on = !ring_on;
            /* haptic deliberately NOT fired here — timer-only for this test */
        }
    }
}

/* ── LED ring update ──────────────────────────────────────────────────────── */
/*  All 24 LEDs the same orange, scaled by ring_pct (20–60%). The base orange
 *  is full red + ~40% green; the percentage scales the overall brightness. */
static void update_ring() {
    if (!ring_on) {
        ring.clear();
        ring.show();
        return;
    }
    float f = ring_pct / 100.0f;
    uint8_t r = (uint8_t)(255 * f);
    uint8_t g = (uint8_t)(100 * f);   /* green third → orange hue */
    for (int i = 0; i < LED_COUNT; i++)
        ring.setPixelColor(i, ring.Color(r, g, 0));
    ring.show();
}

/* ── LVGL widgets ─────────────────────────────────────────────────────────── */
static lv_obj_t *lbl_sensor_a;
static lv_obj_t *lbl_sensor_b;
static lv_obj_t *lbl_state;
static lv_obj_t *lbl_direction;
static lv_obj_t *lbl_steps;
static lv_obj_t *lbl_rotations;
static lv_obj_t *lbl_illegal;
static lv_obj_t *lbl_button;
static lv_obj_t *btn_reset;

static void reset_btn_cb(lv_event_t *e) {
    hall_steps    = 0;
    hall_dir      = 0;
    illegal_count = 0;
    btn_count     = 0;
    quad_prev     = read_state();
}

static void build_test_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hall Quadrature");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* Raw sensor state — left and right */
    lbl_sensor_a = lv_label_create(scr);
    lv_label_set_text(lbl_sensor_a, "A\n---");
    lv_obj_set_style_text_color(lbl_sensor_a, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_sensor_a, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_sensor_a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_sensor_a, LV_ALIGN_CENTER, -110, -70);

    lbl_sensor_b = lv_label_create(scr);
    lv_label_set_text(lbl_sensor_b, "B\n---");
    lv_obj_set_style_text_color(lbl_sensor_b, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_sensor_b, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_sensor_b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_sensor_b, LV_ALIGN_CENTER, +110, -70);

    /* Live (A,B) 2-bit state */
    lbl_state = lv_label_create(scr);
    lv_label_set_text(lbl_state, "AB: 00");
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x66aaff), 0);
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_state, LV_ALIGN_CENTER, 0, -55);

    /* Direction — big, centre */
    lbl_direction = lv_label_create(scr);
    lv_label_set_text(lbl_direction, "--");
    lv_obj_set_style_text_color(lbl_direction, lv_color_hex(0x00cc66), 0);
    lv_obj_set_style_text_font(lbl_direction, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_direction, LV_ALIGN_CENTER, 0, -5);

    /* Step count */
    lbl_steps = lv_label_create(scr);
    lv_label_set_text(lbl_steps, "Steps: 0");
    lv_obj_set_style_text_color(lbl_steps, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_steps, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_steps, LV_ALIGN_CENTER, 0, 50);

    /* Rotations (steps / STEPS_PER_REV) */
    lbl_rotations = lv_label_create(scr);
    lv_label_set_text(lbl_rotations, "Rot: 0.00");
    lv_obj_set_style_text_color(lbl_rotations, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(lbl_rotations, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_rotations, LV_ALIGN_CENTER, 0, 82);

    /* Illegal-transition counter — the overlap test */
    lbl_illegal = lv_label_create(scr);
    lv_label_set_text(lbl_illegal, "Illegal: 0");
    lv_obj_set_style_text_color(lbl_illegal, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(lbl_illegal, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_illegal, LV_ALIGN_CENTER, 0, 114);

    /* Button press counter / live state */
    lbl_button = lv_label_create(scr);
    lv_label_set_text(lbl_button, "Btn: 0");
    lv_obj_set_style_text_color(lbl_button, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(lbl_button, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_button, LV_ALIGN_CENTER, 0, -95);

    /* Reset button */
    btn_reset = lv_btn_create(scr);
    lv_obj_set_size(btn_reset, 100, 38);
    lv_obj_align(btn_reset, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_add_event_cb(btn_reset, reset_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn_reset);
    lv_label_set_text(btn_lbl, "Reset");
    lv_obj_center(btn_lbl);
}

/* ── LVGL timer — updates labels at 10 Hz ────────────────────────────────── */
static void update_timer_cb(lv_timer_t *) {
    int32_t s   = hall_steps;
    int8_t  d   = hall_dir;
    int32_t ill = illegal_count;
    bool    ra  = (digitalRead(HALL_A_PIN) == LOW);
    bool    rb  = (digitalRead(HALL_B_PIN) == LOW);

    lv_obj_set_style_text_color(lbl_sensor_a,
        ra ? lv_color_hex(0xff8800) : lv_color_hex(0x444444), 0);
    lv_label_set_text(lbl_sensor_a, ra ? "A\nMAG" : "A\n---");

    lv_obj_set_style_text_color(lbl_sensor_b,
        rb ? lv_color_hex(0xff8800) : lv_color_hex(0x444444), 0);
    lv_label_set_text(lbl_sensor_b, rb ? "B\nMAG" : "B\n---");

    lv_label_set_text_fmt(lbl_state, "AB: %d%d", ra ? 1 : 0, rb ? 1 : 0);

    if      (d > 0) lv_label_set_text(lbl_direction, "CW");
    else if (d < 0) lv_label_set_text(lbl_direction, "CCW");
    else            lv_label_set_text(lbl_direction, "--");

    lv_label_set_text_fmt(lbl_steps,     "Steps: %ld", (long)s);
    lv_label_set_text_fmt(lbl_rotations, "Rot: %.2f",  (float)s / STEPS_PER_REV);

    /* Illegal counter turns red once it starts climbing */
    lv_obj_set_style_text_color(lbl_illegal,
        ill > 0 ? lv_color_hex(0xff3333) : lv_color_hex(0x666666), 0);
    lv_label_set_text_fmt(lbl_illegal, "Illegal: %ld", (long)ill);

    /* Button — count plus live pressed state (green while held) */
    bool pressed = (digitalRead(BTN_PIN) == LOW);
    lv_obj_set_style_text_color(lbl_button,
        pressed ? lv_color_hex(0x00cc66) : lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text_fmt(lbl_button, "Btn: %ld%s",
        (long)btn_count, pressed ? "  *" : "");
}

/* ── Arduino entry points ─────────────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);
    Serial.println("Hall quadrature test starting...");

    /* Display + LVGL — no I2C, no codec needed for this test */
    lvgl_port_init();

    /* Build and load the test screen.
     * Called immediately after lvgl_port_init() before the LVGL task
     * has had a chance to run, so no explicit lock is needed here. */
    lv_obj_t *test_scr = lv_obj_create(NULL);
    build_test_screen(test_scr);
    lv_scr_load(test_scr);
    lv_timer_create(update_timer_cb, 100, NULL);  /* runs inside LVGL task */

    /* Hall sensors — read both levels, decode quadrature on any change */
    pinMode(HALL_A_PIN, INPUT_PULLUP);
    pinMode(HALL_B_PIN, INPUT_PULLUP);
    quad_prev = read_state();
    attachInterrupt(digitalPinToInterrupt(HALL_A_PIN), quad_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(HALL_B_PIN), quad_isr, CHANGE);

    /* Button — idles HIGH via pull-up, goes LOW when pressed. Polled in loop(). */
    pinMode(BTN_PIN, INPUT_PULLUP);
    btn_state = (digitalRead(BTN_PIN) == LOW);

    /* LED ring */
    ring.begin();
    ring.setBrightness(255);
    ring.clear();
    ring.show();

    /* Haptic engine — shares the I2C bus on GPIO 18 (SDA) / 8 (SCL).
     * Triggered over I2C (drv.go); the TRIG pin is not used because GPIO 9
     * is the ESP32-C6 boot strapping pin and glitches on reset. */
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    haptic_ok = drv.begin(&Wire);
    if (haptic_ok) {
        drv.selectLibrary(1);
        drv.setMode(DRV2605_MODE_INTTRIG);   /* internal trigger via I2C go() */
        drv.setWaveform(0, HAPTIC_EFFECT);   /* preload effect once... */
        drv.setWaveform(1, 0);               /* ...so go() alone retriggers it */
    }
    Serial.printf("haptic DRV2605: %s\n", haptic_ok ? "OK" : "NOT FOUND");

    Serial.println("Ready. Spin the dial.");
}

void loop() {
    poll_button();   /* fast, non-blocking — must run every loop for debounce */

    /* Fire one strong click on every hall edge (either sensor). */
    static uint32_t last_edges = 0;
    uint32_t edges = hall_edges;
    if (edges != last_edges) {
        last_edges = edges;
        haptic_go();
    }

    /* Rotation → brightness: each quadrature count is one LED_PCT_STEP. */
    static int32_t last_steps = 0;
    int32_t steps = hall_steps;
    if (steps != last_steps) {
        ring_pct += (steps - last_steps) * LED_PCT_STEP;
        if (ring_pct < LED_PCT_MIN) ring_pct = LED_PCT_MIN;
        if (ring_pct > LED_PCT_MAX) ring_pct = LED_PCT_MAX;
        last_steps = steps;
    }
    update_ring();

    /* Serial mirror — useful if screen is hard to read during spinning */
    static uint32_t last_ms = 0;
    if (millis() - last_ms >= 500) {
        Serial.printf("dir=%+d  steps=%ld  rot=%.2f  AB=%d%d  illegal=%ld  btn=%ld  ring=%s %d%%  edges=%lu\n",
            hall_dir, (long)hall_steps,
            (float)hall_steps / STEPS_PER_REV,
            digitalRead(HALL_A_PIN) == LOW,
            digitalRead(HALL_B_PIN) == LOW,
            (long)illegal_count,
            (long)btn_count,
            ring_on ? "ON" : "off", ring_pct,
            (unsigned long)hall_edges);
        last_ms = millis();
    }
}
