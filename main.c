// Send callsign in CW (on/off keying of the SI5351 CLK0 output) at 12 wpm,
// once per 30 s period measured from program start; carrier off between messages.
// J.Beale 21-Nov-2026

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "ws2812.pio.h"

// Onboard RGB LED
#define WS2812_PIN   16
static PIO  pio = pio0;
static uint sm  = 0;

// SI5351 on I2C0
#define SI_I2C       i2c0
#define SI_SDA_PIN   4
#define SI_SCL_PIN   5
#define SI_ADDR      0x60

// CW
#define CALLSIGN     "N0CALL"          // change to your callsign
#define CW_WPM       12
#define CW_UNIT_MS   (1200 / CW_WPM)     // dit length: 100 ms at 12 wpm (PARIS standard)
#define PERIOD_MS    30000               // a message starts at each multiple of this

// Morse code: A-Z then 0-9
static const char *const MORSE[36] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
    "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."
};

static const char *morse_of(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return MORSE[c - 'A'];
    if (c >= '0' && c <= '9') return MORSE[26 + c - '0'];
    return NULL;                         // unsupported character: skipped
}

static void put_pixel(uint32_t grb) {
    pio_sm_put_blocking(pio, sm, grb << 8u);   // WS2812 wants GRB, MSB first
}

static uint32_t urgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

// ---- Minimal SI5351 driver ----
static void si_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(SI_I2C, SI_ADDR, buf, 2, false);
}

static uint8_t si_read(uint8_t reg) {
    uint8_t v;
    i2c_write_blocking(SI_I2C, SI_ADDR, &reg, 1, true);
    i2c_read_blocking(SI_I2C, SI_ADDR, &v, 1, false);
    return v;
}

// Write an 8-register PLL or MultiSynth block starting at 'base'
// (PLLA = 26, MS0 = 42). 'extra' carries MS-only bits in the 3rd byte.
static void si_params(uint8_t base, uint32_t p1, uint32_t p2, uint32_t p3, uint8_t extra) {
    si_write(base + 0, (p3 >> 8) & 0xFF);
    si_write(base + 1, p3 & 0xFF);
    si_write(base + 2, extra | ((p1 >> 16) & 0x03));
    si_write(base + 3, (p1 >> 8) & 0xFF);
    si_write(base + 4, p1 & 0xFF);
    si_write(base + 5, ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F));
    si_write(base + 6, (p2 >> 8) & 0xFF);
    si_write(base + 7, p2 & 0xFF);
}

static void si5351_init_144278(void) {
    si_write(3, 0xFF);                          // all outputs disabled

    uint8_t xcl = si_read(183);
    // si_write(183, (xcl & 0x3F) | 0x80);     // 8 pF crystal load (was 123 ppm fast)
    si_write(183, (xcl & 0x3F) | 0xC0);        // 10 pF crystal load

    // PLLA: 25 MHz x (34 + 626720/1000000) = 865.668 MHz
    si_params(26, 3920, 220160, 1000000, 0x00);

    // MS0: integer divide-by-6 -> 144.278 MHz
    si_params(42, 256, 0, 1, 0x00);

    si_write(16, 0x4C);                         // CLK0: MS0 source, integer mode, 2 mA
    si_write(177, 0xA0);                        // reset PLLA and PLLB
}

static void clk0_enable(bool on) {
    uint8_t r3 = si_read(3);
    r3 = on ? (r3 & ~0x01) : (r3 | 0x01);      // bit 0 = CLK0 disable
    si_write(3, r3);
}

// ---- CW keying ----
// Wait until *t, set carrier and LED, then advance *t by 'units' dit lengths.
static void cw_key(absolute_time_t *t, bool on, uint32_t units) {
    sleep_until(*t);
    clk0_enable(on);
    put_pixel(on ? urgb(0, 20, 0) : urgb(2, 0, 0));
    *t = delayed_by_ms(*t, units * CW_UNIT_MS);
}

// Key msg starting at *t. Dit = 1 unit, dah = 3, gap between elements = 1, gap between letters = 3.
static void send_cw(absolute_time_t *t, const char *msg) {
    for (const char *p = msg; *p; p++) {
        const char *code = morse_of(*p);
        if (!code) continue;
        for (const char *e = code; *e; e++) {
            cw_key(t, true, *e == '-' ? 3 : 1);   // element on
            cw_key(t, false, 1);                  // gap after element
        }
        cw_key(t, false, 2);                      // 2 more units: letter gap = 3 units total
    }
}

int main(void) {
    i2c_init(SI_I2C, 100 * 1000);
    gpio_set_function(SI_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SI_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SI_SDA_PIN);
    gpio_pull_up(SI_SCL_PIN);

    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, WS2812_PIN, 800000, false);

    si5351_init_144278();

    absolute_time_t period_top = get_absolute_time();   // period 0 starts here, after init
    while (true) {
        absolute_time_t t = period_top;
        send_cw(&t, CALLSIGN);                 // carrier keyed for the message, ends off
        period_top = delayed_by_ms(period_top, PERIOD_MS);
        sleep_until(period_top);               // carrier stays off until the next period top
    }
}
