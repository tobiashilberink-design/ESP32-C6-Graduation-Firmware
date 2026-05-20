#include <Adafruit_NeoPixel.h>
#include <math.h>

#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── Perlin noise ──────────────────────────────────────────
// Permutation table doubled to 512 so perm[i+1] never wraps.
static uint8_t perm[512];

void perlin_init(uint32_t seed) {
    srand(seed);
    for (int i = 0; i < 256; i++) perm[i] = i;
    // Fisher-Yates shuffle
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        uint8_t tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
    for (int i = 0; i < 256; i++) perm[i + 256] = perm[i];
}

static float fade(float t) {
    // Ken Perlin's smoothstep: 6t^5 - 15t^4 + 10t^3
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float grad(int hash, float x) {
    return (hash & 1) ? x : -x;
}

// 1D Perlin noise — returns roughly -1 to 1
float perlin1d(float x) {
    int   i = (int)floorf(x) & 255;
    float f = x - floorf(x);
    float u = fade(f);
    return grad(perm[i], f) + u * (grad(perm[i + 1], f - 1.0f) - grad(perm[i], f));
}

// ── Fractional Brownian Motion (fBm) ─────────────────────
// Sums 4 octaves of Perlin noise at doubling frequencies and
// halving amplitudes. Produces the 1/f characteristic of real
// fire: big slow swells shaped by small fast flickers.
float fbm(float t) {
    float value     = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_val   = 0.0f;

    for (int i = 0; i < 4; i++) {
        value   += perlin1d(t * frequency) * amplitude;
        max_val += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.1f;  // slightly non-integer so octaves never harmonise
    }
    return value / max_val;  // normalised to -1..1
}

// ── Flicker function ──────────────────────────────────────
// Implements: I(t) = Lerp(MinBrightness, MaxBrightness, Noise(t × Speed))
static const float MIN_BRIGHT = 5.0f;   // near-black at low end
static const float MAX_BRIGHT = 255.0f; // full red at peak
static const float SPEED      = 4.0f;  // higher = faster flicker

float flicker(float t) {
    float noise = fbm(t * SPEED);          // -1 to 1
    float norm  = (noise + 1.0f) * 0.5f;  // remap to 0..1
    return MIN_BRIGHT + norm * (MAX_BRIGHT - MIN_BRIGHT);
}

// ─────────────────────────────────────────────────────────
void setup() {
    perlin_init(esp_random());  // unique shuffle every power-on
    ring.begin();
    ring.setBrightness(128);    // half intensity
    ring.show();
}

void loop() {
    float    t      = millis() / 1000.0f;
    uint8_t  r      = (uint8_t)flicker(t);
    uint32_t color  = ring.Color(r, 0, 0);

    for (int i = 0; i < LED_COUNT; i++) {
        ring.setPixelColor(i, color);
    }
    ring.show();

    delay(16);  // ~60 fps
}
