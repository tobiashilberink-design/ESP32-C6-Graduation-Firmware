#include <Arduino.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "driver/i2c_master.h"
#include "src/tca9554/esp_io_expander_tca9554.h"

// ── LED ring ──────────────────────────────────────────────
#define LED_PIN    2
#define LED_COUNT  24

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── Codec handles ─────────────────────────────────────────
esp_codec_dev_handle_t    playback       = NULL;
esp_io_expander_handle_t  io_expander    = NULL;
i2c_master_bus_handle_t   i2c_bus_handle = NULL;

// ── WAV file ──────────────────────────────────────────────
// Place a 16-bit PCM stereo 24 kHz WAV named "sound.wav"
// in the sketch's data/ folder, then upload via:
//   Tools → ESP32 Sketch Data Upload (LittleFS)
#define WAV_PATH "/sound.wav"
#define SAMPLE_RATE   24000
#define CHANNELS      2
#define BITS          16
#define BUF_BYTES     1024   // samples read per iteration

static uint8_t  buf[BUF_BYTES];
static File     wav_file;
static bool     playing = false;

// ── RMS amplitude → LED brightness ───────────────────────
// Reads one channel of int16 PCM, returns brightness 0–255.
uint8_t rms_to_brightness(uint8_t *pcm, int byte_len) {
    int16_t *samples  = (int16_t *)pcm;
    int      n        = byte_len / (CHANNELS * (BITS / 8));  // frames
    float    sum_sq   = 0.0f;

    for (int i = 0; i < n; i++) {
        float s = samples[i * CHANNELS] / 32768.0f;  // left channel, -1..1
        sum_sq += s * s;
    }

    float rms = sqrtf(sum_sq / n);

    // Scale up — speech/effects typically have low RMS (~0.05–0.2)
    float scaled = rms * 6.0f;
    return (uint8_t)(constrain(scaled, 0.0f, 1.0f) * 255.0f);
}

void set_ring(uint8_t brightness) {
    uint32_t color = ring.Color(brightness, 0, 0);
    for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, color);
    ring.show();
}

// ── Codec init (identical to 08_Audio_Test) ───────────────
void codec_setup() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source              = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                = I2C_NUM_0;
    bus_cfg.scl_io_num              = GPIO_NUM_8;
    bus_cfg.sda_io_num              = GPIO_NUM_18;
    bus_cfg.glitch_ignore_cnt       = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_handle));

    esp_io_expander_new_i2c_tca9554(i2c_bus_handle,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
    esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);

    set_codec_board_type("C6_AMOLED_1_43");
    codec_init_cfg_t cfg;
    init_codec(&cfg);
    playback = get_playback_handle();

    esp_codec_dev_set_out_vol(playback, 80.0);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = SAMPLE_RATE;
    fs.channel         = CHANNELS;
    fs.bits_per_sample = BITS;
    esp_codec_dev_open(playback, &fs);
}

// ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(2000);

    // LED ring
    ring.begin();
    ring.setBrightness(255);
    set_ring(0);

    // Codec
    codec_setup();

    // Filesystem
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
        return;
    }

    wav_file = LittleFS.open(WAV_PATH, "r");
    if (!wav_file) {
        Serial.println("sound.wav not found — upload via Tools > ESP32 Sketch Data Upload");
        return;
    }

    // Skip 44-byte standard WAV header
    wav_file.seek(44);
    playing = true;
    Serial.println("Playing sound.wav");
}

void loop() {
    if (!playing) return;

    int bytes_read = wav_file.read(buf, BUF_BYTES);

    if (bytes_read <= 0) {
        // End of file — loop back
        wav_file.seek(44);
        set_ring(0);
        return;
    }

    // Compute amplitude and update LEDs before sending to speaker
    uint8_t brightness = rms_to_brightness(buf, bytes_read);
    set_ring(brightness);

    // Send PCM to codec
    esp_codec_dev_write(playback, buf, bytes_read);
}
