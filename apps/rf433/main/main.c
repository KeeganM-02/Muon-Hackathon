/* rf433 -- 433.92 MHz OOK/ASK pulse sniffer for the FreeWili OG.
 *
 * Puts one CC1101 into ASK/OOK asynchronous serial receive: the chip
 * demodulates and drives the raw on/off envelope out of GDO0 (IOCFG0 = 0x0D).
 * A GPIO interrupt timestamps every edge, and each burst is printed as pulse
 * durations in microseconds, Flipper .sub "RAW_Data:" style
 * (positive = carrier on, negative = off).
 *
 * Capture is armed by RSSI: the main loop tracks the noise floor and only
 * enables the edge interrupt once RSSI rises TRIGGER_MARGIN_DB above it.
 * A burst ends after GAP_US of silence. Receive only -- never transmits. */
#include "fwog_main.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include <stdlib.h>

FWOG_WATCHDOG_DEFAULT();

/* ---------------- Tunables ---------------- */
#define RF_FREQ_HZ          433920000u
#define RF_RADIO            CC1101_RADIO_CS0 /* try CC1101_RADIO_CS1 if quiet */
#define RF_RX_BW_KHZ        270.0f   /* wider = more tolerant, noisier */
#define RF_DATA_RATE_KBAUD  3.79f

#define TRIGGER_MARGIN_DB   10
#define TRIGGER_FLOOR_DBM   (-90)
#define GAP_US              10000u
#define MAX_BURST_US        2000000u
#define MIN_PULSES          16
#define MAX_PULSES          1024
#define GLITCH_US           60u
#define MAX_GLITCH_PCT      25
#define RSSI_POLL_US        500u
#define STATUS_PERIOD_MS    2000u
#define VALUES_PER_LINE     64

/* ---------------- Edge capture (ISR side) ---------------- */
#define RING_SIZE 2048u
static volatile uint32_t s_edge_t[RING_SIZE];
static volatile uint8_t  s_edge_lvl[RING_SIZE];
static volatile uint32_t s_head, s_tail;
static volatile bool     s_overflow;
static uint s_gdo_pin;

static void gdo_isr(uint gpio, uint32_t events) {
    if (gpio != s_gdo_pin) return;
    const uint32_t now = time_us_32();
    const uint32_t head = s_head;
    if (head - s_tail >= RING_SIZE) { s_overflow = true; return; }
    s_edge_t[head & (RING_SIZE - 1u)] = now;
    uint8_t lvl;
    if ((events & GPIO_IRQ_EDGE_RISE) && !(events & GPIO_IRQ_EDGE_FALL)) lvl = 1;
    else if ((events & GPIO_IRQ_EDGE_FALL) && !(events & GPIO_IRQ_EDGE_RISE)) lvl = 0;
    else lvl = gpio_get(gpio) ? 1 : 0;
    s_edge_lvl[head & (RING_SIZE - 1u)] = lvl;
    s_head = head + 1u;
}

static void capture_arm(bool on) {
    if (on) {
        const uint32_t irq = save_and_disable_interrupts();
        s_tail = s_head;
        s_overflow = false;
        restore_interrupts(irq);
    }
    gpio_set_irq_enabled(s_gdo_pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, on);
}

/* ---------------- Radio setup ---------------- */
static cc1101_t s_radio;

static bool radio_setup_ook_async(cc1101_t *r) {
    bool ok = cc1101_bringup(r);
    ok = ok && cc1101_probe(r);
    if (!ok) return false;

    ok = ok && cc1101_set_frequency(r, RF_FREQ_HZ);
    ok = ok && cc1101_set_modulation(r, CC1101_MOD_ASK);
    ok = ok && cc1101_set_rx_bandwidth(r, RF_RX_BW_KHZ);
    ok = ok && cc1101_set_data_rate(r, RF_DATA_RATE_KBAUD);
    ok = ok && cc1101_set_sync_mode(r, 0);
    ok = ok && cc1101_set_manchester(r, false);
    ok = ok && cc1101_set_crc(r, false);
    ok = ok && cc1101_set_white_data(r, false);
    ok = ok && cc1101_set_pkt_format(r, 3);     /* asynchronous serial */
    ok = ok && cc1101_set_length_config(r, 2);  /* infinite */

    /* AGC tuned for OOK */
    ok = ok && cc1101_write_reg(r, CC1101_REG_AGCCTRL2, 0x07u);
    ok = ok && cc1101_write_reg(r, CC1101_REG_AGCCTRL1, 0x00u);
    ok = ok && cc1101_write_reg(r, CC1101_REG_AGCCTRL0, 0x91u);

    /* GDO0 = async serial data out */
    ok = ok && cc1101_write_reg(r, CC1101_REG_IOCFG0, 0x0Du);

    ok = ok && cc1101_rx(r);
    return ok;
}

/* ---------------- Burst assembly (main-loop side) ---------------- */
static int32_t  s_pulse[MAX_PULSES];
static int      s_npulse;
static bool     s_in_burst;
static uint32_t s_last_edge_t, s_burst_start_t;
static int16_t  s_peak_rssi;
static uint32_t s_frames, s_dropped;

static void burst_reset(void) {
    s_in_burst = false;
    s_npulse = 0;
}

static void burst_emit(void) {
    if (s_npulse < MIN_PULSES) { burst_reset(); return; }

    int glitches = 0;
    uint32_t shortest = UINT32_MAX;
    for (int i = 0; i < s_npulse; i++) {
        const uint32_t d = (uint32_t)abs(s_pulse[i]);
        if (d < GLITCH_US) glitches++;
        if (d < shortest) shortest = d;
    }
    if (glitches * 100 > s_npulse * MAX_GLITCH_PCT) {
        s_dropped++;
        burst_reset();
        return;
    }

    s_frames++;
    const uint32_t dur_us = s_last_edge_t - s_burst_start_t;
    DIAG("\n# frame %lu: %d pulses, %lu.%01lu ms, peak %d dBm, shortest %lu us%s\n",
         (unsigned long)s_frames, s_npulse,
         (unsigned long)(dur_us / 1000u), (unsigned long)((dur_us % 1000u) / 100u),
         (int)s_peak_rssi, (unsigned long)shortest,
         s_npulse >= MAX_PULSES ? " (truncated)" : "");
    for (int i = 0; i < s_npulse; i++) {
        if (i % VALUES_PER_LINE == 0) DIAG("%sRAW_Data:", i ? "\n" : "");
        DIAG(" %ld", (long)s_pulse[i]);
        if ((i & 63) == 63) board_watchdog_kick();
    }
    DIAG("\n");
    burst_reset();
}

static void drain_edges(void) {
    while (s_tail != s_head) {
        const uint32_t i = s_tail & (RING_SIZE - 1u);
        const uint32_t t = s_edge_t[i];
        const uint8_t lvl = s_edge_lvl[i];
        s_tail++;

        if (!s_in_burst) {
            if (lvl == 1) {
                s_in_burst = true;
                s_npulse = 0;
                s_burst_start_t = t;
                s_last_edge_t = t;
            }
            continue;
        }
        const int32_t d = (int32_t)(t - s_last_edge_t);
        s_last_edge_t = t;
        s_pulse[s_npulse++] = lvl ? -d : d;
        if (s_npulse >= MAX_PULSES) { burst_emit(); return; }
    }
}

int main(void) {
    board_init();
    fwog_display_result_t disp = fwog_display_update_run();

#if FWOG_DIAG == FWOG_DIAG_USB
    const absolute_time_t host_deadline = make_timeout_time_ms(5000);
    while (!stdio_usb_connected() && !time_reached(host_deadline)) {
        board_watchdog_kick();
        tight_loop_contents();
    }
    sleep_ms(300);
#endif
    board_watchdog_kick();

    DIAG("\n[rf433_main] display: %s\n", fwog_display_result_text(disp));

    cc1101_bus_init(1000000u);
    cc1101_bind(&s_radio, RF_RADIO);
    s_gdo_pin = s_radio.gdo0_pin;

    if (!radio_setup_ook_async(&s_radio)) {
        while (true) {
            board_watchdog_kick();
            DIAG("[rf433_main] ERR CC1101 (CS%d) did not respond / configure\n",
                 (int)RF_RADIO);
            sleep_ms(1000);
        }
    }
    DIAG("[rf433_main] listening: %lu Hz OOK async, BW %.0f kHz, GDO0 = GPIO %u\n",
         (unsigned long)RF_FREQ_HZ, (double)RF_RX_BW_KHZ, s_gdo_pin);

    gpio_set_irq_enabled_with_callback(s_gdo_pin,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false, &gdo_isr);

    int32_t floor_x10 = (int32_t)cc1101_get_rssi(&s_radio) * 10;
    bool armed = false;
    absolute_time_t next_poll = get_absolute_time();
    absolute_time_t next_status = make_timeout_time_ms(STATUS_PERIOD_MS);
    int16_t rssi = -120;

    while (true) {
        board_watchdog_kick();

        if (armed) {
            if (s_overflow) {
                capture_arm(false);
                armed = false;
                s_dropped++;
                burst_reset();
                continue;
            }
            drain_edges();
        }

        if (time_reached(next_poll)) {
            next_poll = make_timeout_time_us(RSSI_POLL_US);
            rssi = cc1101_get_rssi(&s_radio);
            int trigger = floor_x10 / 10 + TRIGGER_MARGIN_DB;
            if (trigger < TRIGGER_FLOOR_DBM) trigger = TRIGGER_FLOOR_DBM;

            if (!armed) {
                if (rssi >= trigger) {
                    capture_arm(true);
                    armed = true;
                    burst_reset();
                    s_peak_rssi = rssi;
                } else {
                    floor_x10 += ((int32_t)rssi * 10 - floor_x10) / 64;
                }
            } else {
                if (rssi > s_peak_rssi) s_peak_rssi = rssi;
                const uint32_t now = time_us_32();
                const bool quiet = !gpio_get(s_gdo_pin) && rssi < trigger;
                if (s_in_burst) {
                    if ((quiet && now - s_last_edge_t > GAP_US) ||
                        now - s_burst_start_t > MAX_BURST_US) {
                        capture_arm(false);
                        armed = false;
                        drain_edges();
                        if (s_in_burst) burst_emit();
                    }
                } else if (quiet) {
                    capture_arm(false);
                    armed = false;
                }
            }
        }

        if (time_reached(next_status)) {
            next_status = make_timeout_time_ms(STATUS_PERIOD_MS);
            int trigger = floor_x10 / 10 + TRIGGER_MARGIN_DB;
            if (trigger < TRIGGER_FLOOR_DBM) trigger = TRIGGER_FLOOR_DBM;
            DIAG("status  rssi=%d floor=%ld trigger=%d frames=%lu dropped=%lu\n",
                 (int)rssi, (long)(floor_x10 / 10), trigger,
                 (unsigned long)s_frames, (unsigned long)s_dropped);
        }
    }
}