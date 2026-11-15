#include "fwog_display.h"
#include "pico/stdlib.h"
#include <string.h>

/* Red held 6 s powers the board off, with the LED countdown on the WS2812
   bar. Every display app must declare a policy. */
FWOG_POWER_DEFAULT();

#define TITLE_SCALE 4u   /* 6x8 font -> 24x32 px per character */
#define SUB_SCALE   2u   /* 12x16 px per character */

/* Draw `str` horizontally centered at row `y`. */
static void draw_centered(uint16_t y, const char *str, unsigned scale,
                          uint16_t fg, uint16_t bg) {
    const unsigned w = lcd_text_width_px((unsigned)strlen(str), scale);
    const uint16_t x = (w < ST7789_W) ? (uint16_t)((ST7789_W - w) / 2u) : 0u;
    lcd_text_draw(x, y, str, scale, fg, bg);
}

/* Non-blocking init, stepped with a bounded wait (same as apps/ogvegas). */
static bool lcd_start(void) {
    st7789_init_begin();
    const absolute_time_t deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    return st7789_ready();
}

int main(void) {
    board_init();

    const uint16_t black = st7789_rgb565(0, 0, 0);
    const uint16_t white = st7789_rgb565(255, 255, 255);
    const uint16_t green = st7789_rgb565(0, 220, 90);
    const uint16_t grey  = st7789_rgb565(140, 140, 140);

    const bool lcd_ok = lcd_start();
    if (lcd_ok) {
        st7789_clear(black);
        draw_centered(48u,  "Scanning",   TITLE_SCALE, white, black);
        draw_centered(92u,  "RF signals", TITLE_SCALE, white, black);
        draw_centered(150u, "433.92 MHz", SUB_SCALE,   green, black);
        board_backlight(255);
    }

    /* Poll fast (the power-button debouncer needs it), animate slowly. */
    static const char *const dots[] = { "   ", ".  ", ".. ", "..." };
    unsigned frame = 0u;
    absolute_time_t next_anim = make_timeout_time_ms(400);
    absolute_time_t next_beat = make_timeout_time_ms(1000);

    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        fwog_power_poll(now);

        if (lcd_ok && time_reached(next_anim)) {
            next_anim = make_timeout_time_ms(400);
            /* Padded to 3 columns so the previous frame is fully erased. */
            const unsigned w = lcd_text_width_px(3u, SUB_SCALE);
            lcd_text_draw_padded((uint16_t)((ST7789_W - w) / 2u), 180u,
                                 dots[frame++ % 4u], 3u, SUB_SCALE,
                                 grey, black);
        }

        if (time_reached(next_beat)) {
            next_beat = make_timeout_time_ms(1000);
            DIAG("[rf433_display] alive lcd=%s\n", lcd_ok ? "ok" : "FAILED");
        }
        sleep_ms(2);
    }
}