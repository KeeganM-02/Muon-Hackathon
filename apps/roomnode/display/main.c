/* Room node, display CPU: pixels only.
 *
 * It knows nothing about the network, the sensor or the servo. main sends one
 * room_frame_t per half second and this renders it. Everything on screen is
 * therefore a claim main made, which is the point: one source of truth, and a
 * display that cannot disagree with the door.
 *
 * Contract: FWOG_POWER_DEFAULT() + fwog_power_poll() every iteration (the red
 * 6 s hold is the only way to power this board off), and no watchdog on this
 * CPU, ever.
 *
 * Drawing policy: NO full-screen clear after the first paint. Every region owns
 * its rectangle, paints its own background, and is redrawn only when the value
 * it shows changes. Clearing and repainting the world on each 500 ms frame
 * flickers the whole panel -- visible, and worse than useless on a status
 * display somebody glances at from across the room. */

#include "fwog_display.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#include "qrcodegen.h"
#include "room_state.h"

FWOG_POWER_DEFAULT();

#define COL_BG        0x0000u
#define COL_WHITE     0xFFFFu
#define COL_FG        0xFFFFu
#define COL_DIM       0x8410u
#define COL_GREEN     0x07E0u
#define COL_AMBER     0xFD20u
#define COL_RED       0xF800u
#define COL_BLUE      0x2C9Fu

/* Fixed layout, 320x240.
 *
 * The governing rule: nothing in the left column may reach x >= LEFT_W, because
 * the QR frame owns everything to the right of it. Every region paints its own
 * background, so a region that overhangs by even one character erases part of
 * its neighbour -- that is what put black notches in the QR border the first
 * time round. Regions are disjoint by construction, not by luck. */
#define LEFT_W   168u    /* left column: 0..167, 26 characters at scale 1 */

#define HDR_Y      4u
#define FLAG_X    88u
#define STATE_Y   28u
#define PRES_Y    72u
#define HOLD_Y   110u
#define WHO_Y    148u
#define BAR_Y    164u
#define BAR_H     40u
#define RAW_Y    204u    /* just below the alert bar */
#define FLAG_Y   214u
#define URL_Y    226u

#define QR_X  170u
#define QR_Y   16u
#define QR_SZ 148u       /* version-4 URL: (33 modules + 2x2 quiet) * 4 px */
#define QR_QUIET 2

#define LEFT_COLS 26u    /* 26*6 = 156 px, so 8 + 156 = 164 < LEFT_W */

#define REFRESH_MS 100u

static bool           s_link_up;
static fwog_link_rx_t s_rx;
static bool           s_have_frame;
static uint32_t       s_frames_rx;
static uint32_t       s_bytes_rx;
static room_frame_t   s_frame;

typedef enum {
    UI_STATUS = 0,
    UI_ACTIONS,
    UI_DIAGNOSTICS,
    UI_END_SENT,
    UI_TEST_MENU,
} ui_view_t;

static ui_view_t s_view;
static bool      s_view_dirty = true;
static bool      s_view_fresh = true;
static uint32_t  s_end_sent_at_ms;
static bool      s_state_seen;
static uint8_t   s_last_state;
static bool      s_audio_ready;
static bool      s_warn_five_beeped;
static uint32_t  s_gray_down_at_ms;
static bool      s_gray_tracking;
static bool      s_gray_latched;
static unsigned  s_test_index;
static bool      s_test_override;
static room_frame_t s_test_frame;
static uint32_t  s_test_deadline_ms;

#define END_SENT_MS 1500u
#define BEEP_SAMPLES 2240u      /* 100 ms tone, 80 ms gap, 100 ms tone */
static uint8_t s_beep[BEEP_SAMPLES];

/* What the panel is currently showing, so a region can be skipped when nothing
 * it displays has changed. */
static room_frame_t s_drawn;
static bool         s_painted_any;

static uint16_t state_colour(room_state_t st) {
    switch (st) {
    case ROOM_FREE:     return COL_GREEN;
    case ROOM_HELD:     return COL_BLUE;
    case ROOM_RESERVED: return COL_AMBER;
    case ROOM_WARN:     return COL_RED;
    default:            return COL_DIM;
    }
}

static const char *state_word(room_state_t st) {
    switch (st) {
    case ROOM_FREE:     return "FREE";
    case ROOM_HELD:     return "IN USE";
    case ROOM_RESERVED: return "RESERVED";
    case ROOM_WARN:     return "ENDING";
    default:            return "UNKNOWN";
    }
}

/* mm:ss, and h:mm:ss once an hour has passed -- a room held all afternoon
 * should not read 123:45. */
static void format_duration(uint32_t seconds, char *out, size_t outsz) {
    if (seconds >= 3600u) {
        snprintf(out, outsz, "%u:%02u:%02u", (unsigned)(seconds / 3600u),
                 (unsigned)((seconds / 60u) % 60u), (unsigned)(seconds % 60u));
    } else {
        snprintf(out, outsz, "%02u:%02u", (unsigned)(seconds / 60u),
                 (unsigned)(seconds % 60u));
    }
}

static bool leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap_year(year) ? 1 : 0);
}

static int64_t days_before_date(int year, int month, int day) {
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += leap_year(y) ? 366 : 365;
    }
    for (int m = 1; m < month; m++) {
        days += days_in_month(year, m);
    }
    return days + day - 1;
}

static int sunday_on_or_after(int year, int month, int day) {
    /* 1970-01-01 was Thursday. Sunday is zero. */
    const int weekday = (int)((days_before_date(year, month, day) + 4) % 7);
    return day + (7 - weekday) % 7;
}

static int64_t utc_epoch(int year, int month, int day, int hour) {
    return (days_before_date(year, month, day) * 24 + hour) * 3600;
}

static void split_epoch(int64_t epoch, int *year, int *month, int *day,
                        int *hour, int *minute) {
    int64_t days = epoch / 86400;
    int64_t second_of_day = epoch % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        days--;
    }

    int y = 1970;
    while (days >= (leap_year(y) ? 366 : 365)) {
        days -= leap_year(y) ? 366 : 365;
        y++;
    }
    int m = 1;
    while (days >= days_in_month(y, m)) {
        days -= days_in_month(y, m);
        m++;
    }

    *year = y;
    *month = m;
    *day = (int)days + 1;
    *hour = (int)(second_of_day / 3600);
    *minute = (int)((second_of_day / 60) % 60);
}

static void format_eastern_time(int64_t epoch, char *out, size_t outsz) {
    if (epoch <= 0) {
        snprintf(out, outsz, "time syncing");
        return;
    }

    int year, month, day, hour, minute;
    split_epoch(epoch, &year, &month, &day, &hour, &minute);
    const int march_sunday = sunday_on_or_after(year, 3, 8);
    const int november_sunday = sunday_on_or_after(year, 11, 1);
    const int64_t dst_start = utc_epoch(year, 3, march_sunday, 7); /* 2am EST */
    const int64_t dst_end = utc_epoch(year, 11, november_sunday, 6); /* 2am EDT */
    const bool daylight = epoch >= dst_start && epoch < dst_end;

    split_epoch(epoch - (daylight ? 4 : 5) * 3600,
                &year, &month, &day, &hour, &minute);
    const bool pm = hour >= 12;
    int hour12 = hour % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }
    snprintf(out, outsz, "%02d/%02d/%02d %d:%02d%s %s",
             month, day, year % 100, hour12, minute, pm ? "p" : "a",
             daylight ? "EDT" : "EST");
}

static void draw_wrapped(uint16_t x, uint16_t y, const char *text, unsigned cols,
                         unsigned rows, unsigned scale, uint16_t fg, uint16_t bg) {
    char line[64];
    const size_t n = strlen(text);
    unsigned row = 0;
    for (size_t i = 0; i < n && row < rows; i += cols, row++) {
        const size_t take = (n - i < cols) ? (n - i) : cols;
        memcpy(line, text + i, take);
        line[take] = '\0';
        lcd_text_draw_padded(x, (uint16_t)(y + row * LCD_GLYPH_H * scale), line,
                             cols, scale, fg, bg);
    }
}

/* ---- regions ---------------------------------------------------------- */

static void paint_header(const room_frame_t *f) {
    lcd_text_draw(8u, HDR_Y, "ROOM 1", 2u, COL_FG, COL_BG);

    lcd_text_draw_padded(FLAG_X, HDR_Y + 2u, f->net_up ? "online" : "offline",
                         8u, 1u, f->net_up ? COL_GREEN : COL_RED, COL_BG);
    lcd_text_draw_padded(FLAG_X, HDR_Y + 12u, f->sensor_ok ? "sensor" : "no sens",
                         8u, 1u, f->sensor_ok ? COL_DIM : COL_AMBER, COL_BG);
}

static void paint_clock(const room_frame_t *f) {
    char text[32];
    format_eastern_time(f->epoch, text, sizeof text);
    lcd_text_draw_padded(QR_X, HDR_Y, text, 24u, 1u, COL_DIM, COL_BG);
}

static void paint_state(const room_frame_t *f) {
    const room_state_t st = (room_state_t)f->state;
    const char *word = state_word(st);
    unsigned scale = 5u;
    while (scale > 1u && strlen(word) * LCD_GLYPH_W * scale > LEFT_W - 16u) {
        scale--;
    }
    st7789_fill_rect(0u, STATE_Y, LEFT_W, 40u, COL_BG);
    const uint16_t y = (uint16_t)(STATE_Y + 40u - LCD_GLYPH_H * scale);
    lcd_text_draw(8u, y, word, scale, state_colour(st), COL_BG);
}

static void paint_presence(const room_frame_t *f) {
    lcd_text_draw_padded(8u, PRES_Y, f->present ? "present" : "nobody", 8u, 1u,
                         COL_DIM, COL_BG);
    char dur[16];
    format_duration(f->elapsed_s, dur, sizeof dur);
    lcd_text_draw_padded(8u, PRES_Y + 10u, dur, 8u, 3u, f->present ? COL_FG : COL_DIM, COL_BG);
}

static void paint_hold(const room_frame_t *f) {
    /* Time left on the hold, when the backend told us. Always repaints both
     * rows, including the empty case: a stale countdown is worse than a blank
     * one, and this region has to erase itself either way. */
    char left[16] = "";
    const bool have = f->hold_left_s >= 0;
    if (have) {
        format_duration((uint32_t)f->hold_left_s, left, sizeof left);
    }

    const char *label = "hold left";
    if (f->state == ROOM_RESERVED) {
        label = "starts in";
    } else if (f->state == ROOM_WARN) {
        label = "ends in";
    }
    lcd_text_draw_padded(8u, HOLD_Y, have ? label : "", 9u, 1u, COL_DIM, COL_BG);
    lcd_text_draw_padded(8u, HOLD_Y + 10u, left, 8u, 3u,
                         state_colour((room_state_t)f->state), COL_BG);
}

static void paint_holder(const room_frame_t *f) {
    /* Both rows always: draw_wrapped pads only the rows it actually draws, so
     * a holder name shrinking from two lines to one would strand the old tail. */
    if (f->holder[0] != '\0') {
        draw_wrapped(8u, WHO_Y, f->holder, LEFT_COLS, 2u, 1u, COL_DIM, COL_BG);
    } else {
        lcd_text_draw_padded(8u, WHO_Y, "", LEFT_COLS, 1u, COL_DIM, COL_BG);
        lcd_text_draw_padded(8u, WHO_Y + LCD_GLYPH_H, "", LEFT_COLS, 1u, COL_DIM, COL_BG);
    }
}

static void paint_alert(const room_frame_t *f) {
    if (f->alert_level != ROOM_ALERT_NONE && f->alert[0] != '\0') {
        const uint16_t bar = (f->alert_level >= ROOM_ALERT_URGENT) ? COL_RED : COL_AMBER;
        st7789_fill_rect(0u, BAR_Y, ST7789_W, BAR_H, bar);
        draw_wrapped(8u, BAR_Y + 6u, f->alert, 24u, 2u, 2u, COL_BG, bar);
    } else if (f->state == ROOM_WARN) {
        st7789_fill_rect(0u, BAR_Y, ST7789_W, BAR_H, COL_AMBER);
        lcd_text_draw(8u, BAR_Y + 4u, "Ending soon", 2u, COL_BG, COL_AMBER);
        lcd_text_draw(8u, BAR_Y + 26u, "Please leave the room", 1u,
                      COL_BG, COL_AMBER);
    } else {
        st7789_fill_rect(0u, BAR_Y, ST7789_W, BAR_H, COL_BG);
    }
}

/* Decodes the sensor's FUNC_STATUS byte into the words the datasheet uses.
 * A hex byte on a wall display is a puzzle; "pres mot" is information. */
static void append_word(char *out, size_t outsz, const char *word) {
    const size_t used = strlen(out);
    if (used + 1u >= outsz) {
        return;
    }
    if (used > 0u) {
        out[used] = ' ';
        out[used + 1u] = '\0';
    }
    const size_t at = strlen(out);
    if (at + 1u < outsz) {
        strncat(out, word, outsz - at - 1u);
    }
}

static void format_flags(uint8_t status, char *out, size_t outsz) {
    out[0] = '\0';
    if (status & ROOM_FUNC_PRES)  append_word(out, outsz, "pres");
    if (status & ROOM_FUNC_MOT)   append_word(out, outsz, "mot");
    if (status & ROOM_FUNC_SHOCK) append_word(out, outsz, "shock");
    if (out[0] == '\0') {
        snprintf(out, outsz, "no flags");
    }
}

static void paint_status_footer(void) {
    lcd_text_draw_padded(8u, RAW_Y, "Y actions  B details", LEFT_COLS, 1u,
                         COL_DIM, COL_BG);
    lcd_text_draw_padded(8u, FLAG_Y, "", LEFT_COLS, 1u, COL_DIM, COL_BG);
}

static void paint_url(const room_frame_t *f) {
    lcd_text_draw_padded(8u, URL_Y,
                         f->qr_url[0] ? f->qr_url : "URL unavailable",
                         53u, 1u, f->qr_url[0] ? COL_BLUE : COL_AMBER, COL_BG);
}

/* Encoded once and only again if the URL changes: qrcodegen allocates two
 * ~1 KB buffers and runs a Reed-Solomon pass, which is not something to do
 * every 100 ms refresh on a 200 MHz core with a panel to feed. */
#define QR_MAX_VERSION 10u

static void paint_qr(const char *url) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    /* The reservation URL is a 33-module code. A two-module quiet zone makes
     * the 148 px box exactly 37 modules * 4 px, eliminating the unused white
     * band while retaining a clean scanner boundary. */
    st7789_fill_rect(QR_X, QR_Y, QR_SZ, QR_SZ, COL_WHITE);

    if (url == NULL || url[0] == '\0') {
        lcd_text_draw(QR_X + 20u, QR_Y + QR_SZ / 2u - 4u,
                      "URL unavailable", 1u, COL_RED, COL_WHITE);
        return;
    }

    if (!qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                              qrcodegen_Mask_AUTO, true)) {
        /* Too long for the box. Say so on the panel instead of drawing
         * something unscannable and letting a human wonder. */
        lcd_text_draw_padded(QR_X + 4u, QR_Y + QR_SZ / 2u - 4u, "qr: url too long",
                             17u, 1u, COL_RED, COL_WHITE);
        return;
    }

    const int n = qrcodegen_getSize(qr);
    const int dim = n + 2 * QR_QUIET;
    int scale = (int)QR_SZ / dim;
    if (scale < 1) {
        scale = 1;
    }
    const int drawn = dim * scale;
    const int ox = (int)QR_X + ((int)QR_SZ - drawn) / 2;
    const int oy = (int)QR_Y + ((int)QR_SZ - drawn) / 2;

    /* Horizontal runs per row rather than one rect per module: a version-3 code
     * is 29x29, and 841 fill_rect calls over SPI is a visible tear. */
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; ) {
            if (!qrcodegen_getModule(qr, x, y)) {
                x++;
                continue;
            }
            int run = 1;
            while (x + run < n && qrcodegen_getModule(qr, x + run, y)) {
                run++;
            }
            st7789_fill_rect((uint16_t)(ox + (x + QR_QUIET) * scale),
                             (uint16_t)(oy + (y + QR_QUIET) * scale),
                             (uint16_t)(run * scale), (uint16_t)scale, COL_BG);
            x += run;
        }
    }
}

static bool state_can_end(uint8_t state) {
    return state == ROOM_HELD || state == ROOM_WARN;
}

static void send_action(room_action_id_t id) {
    const room_action_t action = {
        .version = ROOM_STATE_VERSION,
        .action = (uint8_t)id,
    };
    uint8_t payload[1u + sizeof action];
    payload[0] = ROOM_MSG_ACTION;
    memcpy(payload + 1, &action, sizeof action);
    (void)fwog_link_uart_send_frame(payload, sizeof payload);
}

static void paint_actions(const room_frame_t *f) {
    st7789_clear(COL_BG);
    lcd_text_draw(8u, 12u, "ROOM ACTIONS", 2u, COL_FG, COL_BG);
    lcd_text_draw(8u, 52u, state_word((room_state_t)f->state), 2u,
                  state_colour((room_state_t)f->state), COL_BG);
    if (state_can_end(f->state)) {
        lcd_text_draw(8u, 96u, "GREEN  end reservation", 1u, COL_GREEN, COL_BG);
    } else {
        lcd_text_draw(8u, 96u, "No active reservation", 1u, COL_DIM, COL_BG);
    }
    lcd_text_draw(8u, 126u, "BLUE   sensor details", 1u, COL_BLUE, COL_BG);
    lcd_text_draw(8u, 156u, "RED or YELLOW  back", 1u, COL_RED, COL_BG);
    st7789_dma_wait();
}

static void paint_diagnostics(const room_frame_t *f, bool full) {
    if (full) {
        st7789_clear(COL_BG);
        lcd_text_draw(8u, 10u, "SENSOR DETAILS", 2u, COL_FG, COL_BG);
        lcd_text_draw(8u, 214u, "RED or YELLOW  back", 1u, COL_DIM, COL_BG);
    }

    char line[48];
    snprintf(line, sizeof line, "state %-9s net %s",
             state_word((room_state_t)f->state), f->net_up ? "up" : "down");
    lcd_text_draw_padded(8u, 52u, line, 40u, 1u, COL_FG, COL_BG);
    snprintf(line, sizeof line, "sensor %s  present %s",
             f->sensor_ok ? "ok" : "OFF", f->present ? "yes" : "no");
    lcd_text_draw_padded(8u, 72u, line, 40u, 1u,
                         f->sensor_ok ? COL_FG : COL_AMBER, COL_BG);
    snprintf(line, sizeof line, "delta %d  tpresence %d", f->tpres_delta, f->tpresence);
    lcd_text_draw_padded(8u, 102u, line, 45u, 1u, COL_FG, COL_BG);
    snprintf(line, sizeof line, "ambient %d", f->tamb_raw);
    lcd_text_draw_padded(8u, 122u, line, 45u, 1u, COL_FG, COL_BG);
    char flags[32];
    format_flags(f->func_status, flags, sizeof flags);
    snprintf(line, sizeof line, "flags %s", flags);
    lcd_text_draw_padded(8u, 142u, line, 45u, 1u, COL_FG, COL_BG);
    char clock[32];
    format_eastern_time(f->epoch, clock, sizeof clock);
    lcd_text_draw_padded(8u, 172u, clock, 32u, 1u, COL_DIM, COL_BG);
    st7789_dma_wait();
}

static void set_view(ui_view_t view);
static void note_state_transition(uint8_t state);

static void paint_test_menu(void) {
    static const char *const names[] = {
        "LIVE SERVICE", "UI: FREE", "UI: HELD 2:00", "UI: RESERVED 2:00",
        "UI: ENDING 0:10", "UI: UNKNOWN", "SENSOR DETAILS",
        "SPEAKER DOUBLE-BEEP", "SERVO LOCK 0%", "SERVO UNLOCK 90%",
    };
    st7789_clear(COL_BG);
    lcd_text_draw(8u, 10u, "DEVICE EXERCISER", 2u, COL_AMBER, COL_BG);
    char number[32];
    snprintf(number, sizeof number, "%u / %u", (unsigned)(s_test_index + 1u),
             (unsigned)count_of(names));
    lcd_text_draw(8u, 52u, number, 1u, COL_DIM, COL_BG);
    lcd_text_draw(8u, 82u, names[s_test_index], 2u, COL_FG, COL_BG);
    lcd_text_draw(8u, 142u, "Y previous    B next", 1u, COL_DIM, COL_BG);
    lcd_text_draw(8u, 166u, "GREEN run", 1u, COL_GREEN, COL_BG);
    lcd_text_draw(8u, 190u, "RED exit", 1u, COL_RED, COL_BG);
    st7789_dma_wait();
}

static void start_beep(void) {
    if (s_audio_ready && i2s_audio_is_idle()) {
        (void)i2s_audio_start(s_beep, BEEP_SAMPLES, true, true);
    }
}

static void activate_test(unsigned index, uint32_t now_ms) {
    if (index == 0u) {
        s_test_override = false;
        note_state_transition(s_frame.state);
        set_view(UI_STATUS);
        return;
    }
    if (index == 6u) {
        set_view(UI_DIAGNOSTICS);
        return;
    }
    if (index == 7u) {
        start_beep();
        return;
    }
    if (index == 8u) {
        send_action(ROOM_ACTION_TEST_SERVO_LOCK);
        return;
    }
    if (index == 9u) {
        send_action(ROOM_ACTION_TEST_SERVO_UNLOCK);
        return;
    }

    s_test_frame = s_frame;
    s_test_frame.net_up = 1u;
    s_test_frame.sensor_ok = 1u;
    s_test_frame.alert_level = ROOM_ALERT_NONE;
    s_test_frame.alert[0] = '\0';
    snprintf(s_test_frame.holder, sizeof s_test_frame.holder, "LOCAL TEST");
    snprintf(s_test_frame.qr_url, sizeof s_test_frame.qr_url,
             "https://example.com/room-test");
    s_test_frame.hold_left_s = -1;
    s_test_frame.present = 0u;
    s_test_frame.elapsed_s = 0u;
    s_test_deadline_ms = 0u;

    switch (index) {
    case 1u:
        s_test_frame.state = ROOM_FREE;
        break;
    case 2u:
        s_test_frame.state = ROOM_HELD;
        s_test_frame.present = 1u;
        s_test_frame.elapsed_s = 75u;
        s_test_deadline_ms = now_ms + 120000u;
        break;
    case 3u:
        s_test_frame.state = ROOM_RESERVED;
        s_test_deadline_ms = now_ms + 120000u;
        break;
    case 4u:
        s_test_frame.state = ROOM_WARN;
        s_test_frame.present = 1u;
        s_test_frame.elapsed_s = 300u;
        s_test_deadline_ms = now_ms + 10000u;
        break;
    default:
        s_test_frame.state = ROOM_UNKNOWN;
        s_test_frame.net_up = 0u;
        s_test_frame.sensor_ok = 0u;
        s_test_frame.qr_url[0] = '\0';
        break;
    }

    s_test_override = true;
    note_state_transition(s_test_frame.state);
    set_view(UI_STATUS);
}

static room_frame_t *active_frame(uint32_t now_ms) {
    if (!s_test_override) {
        return &s_frame;
    }
    if (s_test_deadline_ms != 0u && now_ms < s_test_deadline_ms) {
        s_test_frame.hold_left_s =
            (int32_t)((s_test_deadline_ms - now_ms + 999u) / 1000u);
    } else {
        s_test_frame.hold_left_s = -1;
    }
    return &s_test_frame;
}

static void service_warn_audio(const room_frame_t *f) {
    if (f->state == ROOM_WARN && f->hold_left_s >= 0 &&
        f->hold_left_s <= 5 && !s_warn_five_beeped &&
        i2s_audio_is_idle()) {
        start_beep();
        s_warn_five_beeped = true;
    }
}

static void paint_end_sent(void) {
    st7789_clear(COL_BG);
    lcd_text_draw(34u, 82u, "END REQUESTED", 2u, COL_GREEN, COL_BG);
    lcd_text_draw(48u, 122u, "Waiting for service", 1u, COL_DIM, COL_BG);
    st7789_dma_wait();
}

static void init_beep(void) {
    for (unsigned i = 0; i < BEEP_SAMPLES; i++) {
        /* Two 100 ms, 1 kHz tones with an 80 ms silent gap at 8 kHz. */
        const bool tone = i < 800u || i >= 1440u;
        s_beep[i] = tone ? (uint8_t)(((i / 4u) & 1u) ? 192u : 64u) : 128u;
    }
    s_audio_ready = i2s_audio_init(pio0, 0u);
    if (s_audio_ready) {
        i2s_audio_set_volume(8);
    }
}

static void set_view(ui_view_t view) {
    s_view = view;
    s_view_dirty = true;
    s_view_fresh = true;
    if (view == UI_STATUS) {
        /* The menu owns the whole panel, so returning requires one complete
         * status paint before incremental updates resume. */
        s_painted_any = false;
    }
}

static void note_state_transition(uint8_t state) {
    if (!s_state_seen || s_last_state != state) {
        s_warn_five_beeped = false;
        if (state == ROOM_WARN) {
            start_beep();
        }
    }
    s_state_seen = true;
    s_last_state = state;
}

/* Applies F to the panel, painting only the regions whose content changed. */
static void apply_frame(const room_frame_t *f) {
    const room_frame_t *d = &s_drawn;
    const bool full = !s_painted_any;

    if (full) {
        st7789_clear(COL_BG);
        paint_url(f);
    }
    if (full || strcmp(f->qr_url, d->qr_url) != 0) {
        paint_qr(f->qr_url);
    }

    if (full || f->net_up != d->net_up || f->sensor_ok != d->sensor_ok) {
        paint_header(f);
    }
    if (full || f->epoch / 60 != d->epoch / 60) {
        paint_clock(f);
    }
    if (full || f->state != d->state) {
        paint_state(f);
    }
    if (full || f->present != d->present || f->elapsed_s != d->elapsed_s) {
        paint_presence(f);
    }
    if (full || f->hold_left_s != d->hold_left_s || f->state != d->state) {
        paint_hold(f);
    }
    if (full || strcmp(f->holder, d->holder) != 0) {
        paint_holder(f);
    }
    if (full || f->state != d->state || f->alert_level != d->alert_level ||
        strcmp(f->alert, d->alert) != 0) {
        paint_alert(f);
    }
    if (full) {
        paint_status_footer();
    }
    if (!full && strcmp(f->qr_url, d->qr_url) != 0) {
        paint_url(f);
    }

    st7789_dma_wait();
    s_drawn = *f;
    s_painted_any = true;
}

static void service_link(void) {
    if (!s_link_up) {
        return;
    }

    uint8_t b;
    while (fwog_link_uart_read(&b)) {
        s_bytes_rx++;
        size_t len = 0u;
        if (!fwog_link_rx_byte(&s_rx, b, &len)) {
            continue;
        }

        /* Main's breakout-direction requests come over this same link and must
         * be answered first, or main never gets its pads configured. */
        if (fwog_ioexp_link_handle(s_rx.buf, len)) {
            continue;
        }

        if (len != 1u + sizeof(room_frame_t) || s_rx.buf[0] != ROOM_MSG_STATE) {
            continue;
        }

        room_frame_t f;
        memcpy(&f, s_rx.buf + 1, sizeof f);

        /* A frame from a mismatched build is refused rather than rendered:
         * wrong firmware must look like wrong firmware. */
        if (f.version != ROOM_STATE_VERSION) {
            DIAG("[roomnode_display] frame version %u, expected %u\n",
                 f.version, ROOM_STATE_VERSION);
            continue;
        }

        f.alert[sizeof f.alert - 1u] = '\0';
        f.holder[sizeof f.holder - 1u] = '\0';
        f.qr_url[sizeof f.qr_url - 1u] = '\0';
        s_frame = f;
        s_have_frame = true;
        s_frames_rx++;
        if (s_view == UI_DIAGNOSTICS) {
            s_view_dirty = true;
        }
        if (!s_test_override) {
            note_state_transition(f.state);
        }
    }
}

int main(void) {
    board_init();

    /* The display bootloader deinits the link before jumping here, so the
     * application brings it back up -- this is what lets main's
     * fwog_io_dir_apply() complete while the panel is still initializing. */
    s_link_up = fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_rx);
    fwog_buttons_init();
    init_beep();

    DIAG("\n[roomnode_display] alive, link=%s audio=%s\n",
         s_link_up ? "ok" : "FAILED", s_audio_ready ? "ok" : "FAILED");

    /* Staged init with the link serviced between steps: main may already be
     * waiting on the expander ack that unlocks its UART pads. */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(1000);
    while (!st7789_ready() && !time_reached(lcd_deadline)) {
        service_link();
        (void)fwog_power_poll(to_ms_since_boot(get_absolute_time()));
        st7789_init_step();
        tight_loop_contents();
    }

    if (!st7789_ready()) {
        DIAG("[roomnode_display] panel never became ready\n");
    } else {
        st7789_clear(COL_BG);
        st7789_dma_wait();
        board_backlight(255);

        /* Something on screen from the first frame: a black panel is
         * indistinguishable from a hung one. */
        lcd_text_draw(8u, HDR_Y, "ROOM 1", 2u, COL_FG, COL_BG);
        lcd_text_draw_padded(8u, STATE_Y, "waiting", 9u, 3u, COL_DIM, COL_BG);
        lcd_text_draw_padded(8u, PRES_Y, "for main CPU", 16u, 1u, COL_DIM, COL_BG);
    }

    absolute_time_t next_check = make_timeout_time_ms(REFRESH_MS);
    absolute_time_t next_log = make_timeout_time_ms(5000);
    while (true) {
        service_link();
        i2s_audio_process();

        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const fwog_buttons_t buttons = fwog_power_poll(now_ms).buttons;
        const uint8_t gray = FWOG_BTN_BIT(FWOG_BTN_GRAY);
        const uint8_t yellow = FWOG_BTN_BIT(FWOG_BTN_YELLOW);
        const uint8_t green = FWOG_BTN_BIT(FWOG_BTN_GREEN);
        const uint8_t blue = FWOG_BTN_BIT(FWOG_BTN_BLUE);
        const uint8_t red = FWOG_BTN_BIT(FWOG_BTN_RED);

        if (buttons.down & gray) {
            if (!s_gray_tracking) {
                s_gray_tracking = true;
                s_gray_down_at_ms = now_ms;
            } else if (!s_gray_latched && now_ms - s_gray_down_at_ms >= 500u) {
                s_gray_latched = true;
                set_view(UI_TEST_MENU);
            }
        } else {
            s_gray_tracking = false;
            s_gray_latched = false;
        }

        room_frame_t *shown = active_frame(now_ms);
        service_warn_audio(shown);

        if (s_view == UI_STATUS) {
            if ((buttons.pressed & yellow) && s_test_override) {
                set_view(UI_TEST_MENU);
            } else if (buttons.pressed & yellow) {
                set_view(UI_ACTIONS);
            } else if (buttons.pressed & blue) {
                set_view(UI_DIAGNOSTICS);
            }
        } else if (s_view == UI_ACTIONS) {
            if (buttons.pressed & (red | yellow)) {
                set_view(UI_STATUS);
            } else if (buttons.pressed & blue) {
                set_view(UI_DIAGNOSTICS);
            } else if ((buttons.pressed & green) &&
                       state_can_end(shown->state)) {
                send_action(ROOM_ACTION_END);
                s_end_sent_at_ms = now_ms;
                set_view(UI_END_SENT);
            }
        } else if (s_view == UI_DIAGNOSTICS) {
            if (buttons.pressed & (red | yellow)) {
                set_view(UI_STATUS);
            }
        } else if (s_view == UI_TEST_MENU) {
            if (buttons.pressed & red) {
                set_view(UI_STATUS);
            } else if (buttons.pressed & yellow) {
                s_test_index = (s_test_index + 9u) % 10u;
                s_view_dirty = true;
            } else if (buttons.pressed & blue) {
                s_test_index = (s_test_index + 1u) % 10u;
                s_view_dirty = true;
            } else if (buttons.pressed & green) {
                activate_test(s_test_index, now_ms);
            }
        } else if ((buttons.pressed & (red | yellow)) ||
                   now_ms - s_end_sent_at_ms >= END_SENT_MS) {
            set_view(UI_STATUS);
        }

        if (time_reached(next_check)) {
            next_check = make_timeout_time_ms(REFRESH_MS);
            if (s_view == UI_TEST_MENU && s_view_dirty) {
                paint_test_menu();
                s_view_dirty = false;
                s_view_fresh = false;
            } else if (s_have_frame) {
                if (s_view == UI_STATUS) {
                    apply_frame(shown);
                    s_view_dirty = false;
                } else if (s_view_dirty && s_view == UI_ACTIONS) {
                    paint_actions(shown);
                    s_view_dirty = false;
                    s_view_fresh = false;
                } else if (s_view_dirty && s_view == UI_DIAGNOSTICS) {
                    paint_diagnostics(shown, s_view_fresh);
                    s_view_dirty = false;
                    s_view_fresh = false;
                } else if (s_view_dirty && s_view == UI_END_SENT) {
                    paint_end_sent();
                    s_view_dirty = false;
                    s_view_fresh = false;
                }
            }
        }

        /* Repeated, not once at boot: pico_stdio_usb drops anything written
         * before the host asserts DTR, so a boot-time summary is unreadable in
         * practice. This says whether frames are arriving and what was drawn. */
        if (time_reached(next_log)) {
            next_log = make_timeout_time_ms(5000);
            DIAG("[roomnode_display] frames=%lu bytes=%lu have=%d state=%u "
                 "present=%d net=%d tpres=%d d=%d\n",
                 (unsigned long)s_frames_rx, (unsigned long)s_bytes_rx,
                 s_have_frame ? 1 : 0, (unsigned)s_frame.state,
                 s_frame.present ? 1 : 0, s_frame.net_up ? 1 : 0,
                 s_frame.tpresence, s_frame.tpres_delta);
        }
        tight_loop_contents();
    }
}
