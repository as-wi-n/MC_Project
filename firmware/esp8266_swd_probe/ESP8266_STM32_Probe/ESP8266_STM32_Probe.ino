/*
 * ESP8266 as an SWD debug probe for STM32 / Cortex-M targets.
 * Ported from ESP32SWDProbe (github.com/jithurx/mcproject) to ESP8266.
 *
 * SERIAL-ONLY: the host talks to this probe over the USB serial port.
 * No WiFi is used. See host/flash_stm32.py.
 *
 * Wiring to target (STM32 Blue Pill / F103):
 *   SWDIO (GPIO13 / D7) -> PA13 (SWDIO / JTMS)
 *   SWCLK (GPIO12 / D6) -> PA14 (SWCLK / JTCK)
 *   NRST  (GPIO14 / D5) -> NRST   (optional, active-low reset)
 *   GND                 -> GND    (mandatory, common ground)
 *
 * The ESP8266 has no USB peripheral: the micro-USB port only drives a
 * CH340/CP2102 USB-to-UART bridge. All host<->probe traffic therefore goes
 * over that UART; SWD itself is bit-banged on the GPIOs above.
 *
 * Native binary protocol (same as the ESP32 probe, see handle_native_cmd):
 *   one command byte, optional body, reply = <status,u8><detail,u8> + payload.
 */

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

/* ---- Pin map (NodeMCU/D1 mini names in comments) ---- */
static const uint8_t PIN_SWCLK = 12;   /* D6 */
static const uint8_t PIN_SWDIO = 13;   /* D7 */
static const uint8_t PIN_NRST  = 14;   /* D5 */
static const uint8_t PIN_LED   = 2;    /* D4 (onboard LED, active-low) */

static bool swdio_output = false;
static bool blink_active = false;

/* When the serial command channel is in use, debug prints would corrupt the
 * binary protocol on the shared UART, so they are suppressed. */
static bool serial_cmd_active = false;
#define DBG_PRINT(...)   do { if (!serial_cmd_active) Serial.print(__VA_ARGS__); } while (0)
#define DBG_PRINTLN(...) do { if (!serial_cmd_active) Serial.println(__VA_ARGS__); } while (0)

static uint32_t cur_select = 0xFFFFFFFF;
static uint32_t g_dpidr = 0;   /* last DPIDR seen (CKS32 clones differ) */

/* Direct GPIO register access on ESP8266, to bypass digitalWrite/digitalRead
 * overhead. GPIO_OUT_W1TS/W1TC are write-1-to-set / write-1-to-clear. */
#define ESP_GPIO_OUT_W1TS 0x60000304u
#define ESP_GPIO_OUT_W1TC 0x60000308u
#define ESP_GPIO_IN       0x60000318u

static inline void gpio_fast_set(uint8_t pin, bool high)
{
    volatile uint32_t *reg = (volatile uint32_t *)(high ? ESP_GPIO_OUT_W1TS
                                                        : ESP_GPIO_OUT_W1TC);
    *reg = (1UL << pin);
}

static inline bool gpio_fast_get(uint8_t pin)
{
    return (*(volatile uint32_t *)ESP_GPIO_IN >> pin) & 1;
}

/* Tunable settle time between GPIO edges (ESP8266 runs at 80 MHz).
 * Raise if the link is unstable over long jumper wires; the target is
 * clocked by us, so a slower clock is always safe -- just slower. */
static uint32_t swd_delay_cycles = 50;

static inline void swd_delay(void)
{
    for (volatile uint32_t i = 0; i < swd_delay_cycles; i++) {
    }
}

static void swd_write(bool swclk, bool swdio)
{
    gpio_fast_set(PIN_SWCLK, swclk);
    if (swdio_output) {
        gpio_fast_set(PIN_SWDIO, swdio);
    }
}

/* ------------------------------------------------------------------ *
 * SWD transport (mirrors OpenOCD bitbang.c).  Error codes:
 *   1 OK, -2 parity, -3 FAULT, -4 WAIT retries exhausted
 * ------------------------------------------------------------------ */

#define SWD_START   (1 << 0)
#define SWD_APNDP   (1 << 1)
#define SWD_RNW     (1 << 2)
#define SWD_A32     (3 << 3)
#define SWD_PARITY  (1 << 5)
#define SWD_STOP    (0 << 6)
#define SWD_PARK    (1 << 7)

static void swd_clock_half(bool high)
{
    gpio_fast_set(PIN_SWCLK, high);
    swd_delay();
}

/* Clock bit_cnt bits.  rnw=false: drive SWDIO from 'bits' LSB-first.
 * rnw=true: sample SWDIO into *rdata LSB-first (pin must be input). */
static void swd_exchange(bool rnw, uint64_t bits, int bit_cnt, uint64_t *rdata)
{
    uint64_t v = rdata ? *rdata : 0;
    for (int i = 0; i < bit_cnt; i++) {
        if (!rnw)
            gpio_fast_set(PIN_SWDIO, (bits >> i) & 1);
        swd_clock_half(false);
        if (rnw && gpio_fast_get(PIN_SWDIO))
            v |= (1ULL << i);
        swd_clock_half(true);
    }
    if (rdata) *rdata = v;
}

/* Raw DP ABORT write (STKERRCLR|STKCMPCLR|WDERRCLR|ORUNERRCLR). */
static int swd_clear_sticky(void)
{
    for (int retry = 0; retry < 8; retry++) {
        uint8_t cmd = SWD_START | SWD_PARK;             /* DP write, reg 0x0 */
        pinMode(PIN_SWDIO, OUTPUT);
        swd_exchange(false, cmd, 8, NULL);
        pinMode(PIN_SWDIO, INPUT_PULLUP);
        uint64_t rd = 0;
        swd_exchange(true, 0, 1 + 3 + 1, &rd);
        int ack = (int)((rd >> 1) & 7);
        pinMode(PIN_SWDIO, OUTPUT);
        if (ack == 2 || ack == 7) {
            delayMicroseconds(20);
            continue;
        }
        if (ack != 1) return -3;
        swd_exchange(false, 0x1e, 33, NULL);            /* ABORT + parity(0) */
        digitalWrite(PIN_SWDIO, HIGH);
        return 1;
    }
    return -4;
}

static int swd_read_reg(bool ap, uint8_t reg, uint32_t *value, bool ap_delay)
{
    for (int retry = 0; retry < 1000; retry++) {
        if ((retry & 0x3F) == 0) yield();
        uint8_t cmd = SWD_RNW | (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);
        uint8_t p = 0;
        for (int i = 1; i <= 4; i++) p ^= (cmd >> i) & 1;
        if (p) cmd |= SWD_PARITY;
        cmd |= SWD_START | SWD_PARK;

        pinMode(PIN_SWDIO, OUTPUT);
        swd_exchange(false, cmd, 8, NULL);

        pinMode(PIN_SWDIO, INPUT_PULLUP);
        uint64_t rd = 0;
        swd_exchange(true, 0, 1 + 3 + 32 + 1 + 1, &rd);
        pinMode(PIN_SWDIO, OUTPUT);
        digitalWrite(PIN_SWDIO, HIGH);

        int ack = (int)((rd >> 1) & 7);
        if (ack == 2 || ack == 7) {
            if (ack == 2) swd_clear_sticky();
            swd_exchange(true, 0, 8, NULL);
            delayMicroseconds(20);
            continue;
        }
        if (ack != 1) return -3;
        uint32_t data = (uint32_t)((rd >> 4) & 0xffffffffu);
        int parity = (int)((rd >> 36) & 1);
        uint8_t dp = 0;
        for (int i = 0; i < 32; i++) dp ^= (uint8_t)((data >> i) & 1);
        if (dp != parity) return -2;
        if (value) *value = data;
        if (ap_delay) swd_exchange(false, 0, 8, NULL);
        return 1;
    }
    return -4;
}

static int swd_write_reg(bool ap, uint8_t reg, uint32_t wdata, bool ap_delay)
{
    for (int retry = 0; retry < 500; retry++) {
        if ((retry & 0x3F) == 0) yield();
        uint8_t cmd = (ap ? SWD_APNDP : 0) | ((reg & 0xc) << 1);
        uint8_t p = 0;
        for (int i = 1; i <= 4; i++) p ^= (cmd >> i) & 1;
        if (p) cmd |= SWD_PARITY;
        cmd |= SWD_START | SWD_PARK;

        pinMode(PIN_SWDIO, OUTPUT);
        swd_exchange(false, cmd, 8, NULL);

        pinMode(PIN_SWDIO, INPUT_PULLUP);
        uint64_t rd = 0;
        swd_exchange(true, 0, 1 + 3 + 1, &rd);
        int ack = (int)((rd >> 1) & 7);
        pinMode(PIN_SWDIO, OUTPUT);
        if (ack == 2 || ack == 7) {
            if (ack == 2) swd_clear_sticky();
            delayMicroseconds(20);
            continue;
        }
        if (ack != 1) return -3;

        uint8_t dp = 0;
        for (int i = 0; i < 32; i++) dp ^= (uint8_t)((wdata >> i) & 1);
        uint64_t out = (uint64_t)wdata | ((uint64_t)dp << 32);
        swd_exchange(false, out, 33, NULL);
        digitalWrite(PIN_SWDIO, HIGH);
        if (ap_delay) swd_exchange(false, 0, 8, NULL);
        return 1;
    }
    return -4;
}

static int swd_init(void)
{
    cur_select = 0xFFFFFFFF;
    /* full JTAG-to-SWD sequence from OpenOCD swd_seq_jtag_to_swd */
    pinMode(PIN_SWDIO, OUTPUT);
    digitalWrite(PIN_SWDIO, HIGH);
    for (int i = 0; i < 7; i++) swd_exchange(false, 0xff, 8, NULL);
    swd_exchange(false, 0x9e, 8, NULL);
    swd_exchange(false, 0xe7, 8, NULL);
    for (int i = 0; i < 7; i++) swd_exchange(false, 0xff, 8, NULL);
    swd_exchange(false, 0x00, 8, NULL);
    digitalWrite(PIN_SWDIO, HIGH);

    uint32_t dpidr = 0;
    int st = swd_read_reg(false, 0x00, &dpidr, false);
    if (st != 1) return st;
    g_dpidr = dpidr;   /* accept any DPIDR (CKS32/other clones differ); 'I' reports it */
    (void)dpidr;

    /* Clear sticky errors latched in the target DP. */
    st = swd_write_reg(false, 0x00, 0x1e, false);
    if (st != 1) return st;

    /* Request debug + system power; wait for both PWRUPACKs. */
    st = swd_write_reg(false, 0x04, 0x50000000, false);
    if (st != 1) return st;

    uint32_t cs;
    for (int i = 0; i < 100; i++) {
        st = swd_read_reg(false, 0x04, &cs, false);
        if (st != 1) return st;
        if ((cs & 0x30000000) == 0x30000000) break;
        delayMicroseconds(10);
    }
    if ((cs & 0x30000000) != 0x30000000) return -6;
    cur_select = 0xFFFFFFFF;
    return 1;
}

/* ------------------------------------------------------------------ *
 * AP/MEM-AP layer: SELECT management + generic memory access.
 * ------------------------------------------------------------------ */

static int swd_select(uint32_t sel)
{
    if (sel == cur_select) return 1;
    int st = swd_write_reg(false, 0x08, sel, false);
    if (st != 1) return st;
    cur_select = sel;
    return 1;
}

/* AP IDR Class/Type fields (ADIv5). Verified against blackmagic's adiv5.h:
 * CLASS lives at bits[16:13] (8 == MEM-AP, 0 == JTAG-AP), TYPE at bits[3:0]
 * (1 == AHB2/AHB3 -- what our known-good AP0 on this board reports: IDR
 * 0x24770011 decodes to CLASS=8, TYPE=1, matching OpenOCD's own "MEM-AP
 * AHB3" for this exact chip). */
#define ADIV5_AP_IDR_CLASS_MASK  0x0001E000u
#define ADIV5_AP_IDR_CLASS_SHIFT 13u
#define ADIV5_AP_IDR_CLASS_MEM   8u

static uint32_t g_ap_sel = 0;
static bool     g_ap_found = false;

/* Find the first APSEL that answers with a MEM-AP, instead of assuming
 * APSEL 0 like the probe originally did. Cached for the whole boot session
 * (like fpb_is_v1()'s cache) -- ap_init() below runs on nearly every SWD
 * transaction, so re-scanning every time would be a serious slowdown for a
 * property that can't change without physically rewiring the target. */
static int ap_probe(void)
{
    if (g_ap_found) return 1;
    for (uint32_t apsel = 0; apsel <= 7; apsel++) {
        int st = swd_select(0x00000000u);
        if (st != 1) return st;
        st = swd_select((apsel << 24) | 0x000000F0u);
        if (st != 1) continue;
        uint32_t idr = 0;
        /* AP reads are posted: this primes the read, it does not return the
         * value -- ap_init()'s warm-up read gets away with a single read
         * because it never looks at the result, but this does, so it needs
         * the DP RDBUFF follow-up read too (same two-step the 'D' command
         * already uses). Missing this the first time around made every
         * probed AP read back as 0, which the class check then correctly
         * (if uselessly) rejected as "not a MEM-AP" for every single slot. */
        st = swd_read_reg(true, 0x0c, &idr, true);
        if (st == 1) st = swd_read_reg(false, 0x0c, &idr, false);
        swd_select(0x00000000u);
        if (st != 1 || idr == 0) continue;      /* absent/faulted AP index */
        uint32_t cls = (idr & ADIV5_AP_IDR_CLASS_MASK) >> ADIV5_AP_IDR_CLASS_SHIFT;
        if (cls == ADIV5_AP_IDR_CLASS_MEM) {
            g_ap_sel = apsel;
            g_ap_found = true;
            return 1;
        }
    }
    return -16;                                 /* no MEM-AP found on this DP */
}

/* Warm up the AHB-AP exactly like OpenOCD: SELECT bank F, read AP IDR,
 * then return to bank 0.  Required before the first AP write or the AP
 * answers with ack=7 while it is still waking. */
static int ap_init(void)
{
    int st = ap_probe();
    if (st != 1) return st;
    st = swd_select((g_ap_sel << 24) | 0x00000000u);
    if (st != 1) return st;
    st = swd_select((g_ap_sel << 24) | 0x000000F0u);
    if (st != 1) return st;
    uint32_t idr = 0;
    st = swd_read_reg(true, 0x0c, &idr, true);
    if (st != 1) return st;
    st = swd_select((g_ap_sel << 24) | 0x00000000u);
    if (st != 1) return st;
    return 1;
}

/* Fast 32-bit block read via TAR auto-increment. */
static int swd_read_block(uint32_t addr, uint32_t *buf, uint32_t nwords)
{
    int st = ap_init();
    if (st != 1) return st;
    st = swd_write_reg(true, 0x00, 0x23000052, true);   /* CSW word, autoinc */
    if (st != 1) return st;
    st = swd_write_reg(true, 0x04, addr, true);         /* TAR */
    if (st != 1) return st;
    uint32_t tmp;
    st = swd_read_reg(true, 0x0c, &tmp, true);          /* prime AP DRW */
    if (st != 1) return st;
    for (uint32_t i = 0; i < nwords; i++) {
        if ((i & 0x3F) == 0) yield();
        st = swd_read_reg(false, 0x0c, &buf[i], false); /* DP RDBUFF */
        if (st != 1) return st;
        if (i + 1 < nwords) {
            uint32_t next_addr = addr + (i + 1) * 4;
            /* AHB-AP TAR auto-increment wraps within the current 1 KB
             * window instead of carrying into the upper address bits, so
             * TAR must be re-issued at every 1 KB boundary. */
            if ((next_addr & 0x3FFu) == 0) {
                st = swd_write_reg(true, 0x04, next_addr, true);
                if (st != 1) return st;
            }
            st = swd_read_reg(true, 0x0c, &tmp, true);
            if (st != 1) return st;
        }
    }
    return 1;
}

/* Fast 32-bit block write via TAR auto-increment. */
static int swd_write_block(uint32_t addr, const uint8_t *data, uint32_t nwords)
{
    int st = ap_init();
    if (st != 1) return st;
    st = swd_write_reg(true, 0x00, 0x23000052, true);
    if (st != 1) return st;
    st = swd_write_reg(true, 0x04, addr, true);
    if (st != 1) return st;
    const uint32_t *w = (const uint32_t *)data;
    for (uint32_t i = 0; i < nwords; i++) {
        if ((i & 0x3F) == 0) yield();
        uint32_t cur_addr = addr + i * 4;
        /* Same 1 KB TAR auto-increment wrap as swd_read_block. */
        if (i > 0 && (cur_addr & 0x3FFu) == 0) {
            st = swd_write_reg(true, 0x04, cur_addr, true);
            if (st != 1) return st;
        }
        st = swd_write_reg(true, 0x0c, w[i], true);
        if (st != 1) return st;
    }
    uint32_t dummy;
    st = swd_read_reg(false, 0x0c, &dummy, false);      /* flush write buffer */
    return st;
}

static int mem_rd32(uint32_t addr, uint32_t *val)
{
    return swd_read_block(addr, val, 1);
}

static int mem_wr32(uint32_t addr, uint32_t val)
{
    return swd_write_block(addr, (const uint8_t *)&val, 1);
}

/* Read len bytes from addr, handling arbitrary alignment/size.  Uses only
 * whole-word AHB-AP reads (the probe's sub-word reads are unreliable at
 * byte offsets != 0) and slices the requested bytes out of the result. */
static int mem_read(uint32_t addr, uint8_t *out, uint32_t len)
{
    if (len == 0) return 1;
    uint32_t off = addr & 3;
    uint32_t nwords = (off + len + 3) >> 2;
    uint8_t *tmp = (uint8_t *)malloc(nwords * 4);
    if (!tmp) return -12;
    int st = swd_read_block(addr & ~3u, (uint32_t *)tmp, nwords);
    if (st == 1) memcpy(out, tmp + off, len);
    free(tmp);
    return st;
}

/* Write len bytes to addr, handling arbitrary alignment/size. */
static int mem_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) return 1;
    if ((addr & 3) == 0 && (len & 3) == 0) {
        return swd_write_block(addr, data, len >> 2);
    }
    int st = ap_init();
    if (st != 1) return st;
    uint32_t rem = len, a = addr;
    const uint8_t *p = data;
    while (rem) {
        uint32_t s;
        if ((a & 3) == 0 && rem >= 4) s = 2;
        else if ((a & 1) == 0 && rem >= 2) s = 1;
        else s = 0;
        uint32_t bytes = 1u << s;
        uint32_t off = a & 3;
        uint32_t v = 0;
        for (uint32_t k = 0; k < bytes; k++) v |= ((uint32_t)p[k]) << (8 * (off + k));
        st = swd_write_reg(true, 0x00, 0x80000010 | s, true);   /* CSW size */
        if (st != 1) return st;
        st = swd_write_reg(true, 0x04, a, true);                /* TAR */
        if (st != 1) return st;
        st = swd_write_reg(true, 0x0c, v, true);                /* AP DRW */
        if (st != 1) return st;
        uint32_t dummy;
        st = swd_read_reg(false, 0x0c, &dummy, false);
        if (st != 1) return st;
        a += bytes; p += bytes; rem -= bytes;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * Cortex-M core control (ARMv7-M / STM32F1).
 * ------------------------------------------------------------------ */

#define DHCSR   0xE000EDF0u
#define DCRSR   0xE000EDF4u
#define DCRDR   0xE000EDF8u
#define DEMCR   0xE000EDFCu
#define AIRCR   0xE000ED0Cu
#define DFSR    0xE000ED30u

#define DHCSR_S_HALT   (1u << 17)

/* Last halt reason (DFSR bits: HALTED|BKPT|DWTTRAP|VCATCH|EXTERNAL), cached
 * for the 'J' native command. */
static uint32_t g_last_dfsr = 0;

/* DFSR is sticky (write-1-to-clear): read it, then clear whatever was set,
 * so the next halt's reason isn't polluted by a prior one. */
static int cortex_read_clear_dfsr(void)
{
    uint32_t dfsr = 0;
    int st = mem_rd32(DFSR, &dfsr);
    if (st != 1) return st;
    g_last_dfsr = dfsr;
    if (dfsr) {
        st = mem_wr32(DFSR, dfsr);
        if (st != 1) return st;
    }
    return 1;
}

static int cortex_poll_halted(uint32_t timeout_ms, bool *halted)
{
    uint32_t t0 = millis();
    do {
        uint32_t dhcsr;
        int st = mem_rd32(DHCSR, &dhcsr);
        if (st != 1) return st;
        if (dhcsr & DHCSR_S_HALT) { *halted = true; return 1; }
        delay(1);
    } while (millis() - t0 < timeout_ms);
    *halted = false;
    return 1;
}

static int cortex_halt(void)
{
    int st = mem_wr32(DHCSR, 0xA05F0003);   /* DBGKEY|C_DEBUGEN|C_HALT */
    if (st != 1) return st;
    bool h = false;
    st = cortex_poll_halted(2000, &h);
    if (st != 1) return st;
    if (h) cortex_read_clear_dfsr();
    return h ? 1 : -7;
}

/* Single-step with interrupts masked. Per the ARMv7-M architecture, a write
 * that changes C_MASKINTS must not simultaneously change C_HALT/C_STEP, so
 * this takes two writes: first set C_MASKINTS while still halted, then clear
 * C_HALT and set C_STEP in a second write with C_MASKINTS held. Without this,
 * a pending interrupt can fire mid-step and land the core somewhere other
 * than the next instruction, which is what made stepping flaky. */
static int cortex_step_write(void)
{
    int st = mem_wr32(DHCSR, 0xA05F000B);   /* DBGKEY|C_DEBUGEN|C_HALT|C_MASKINTS */
    if (st != 1) return st;
    return mem_wr32(DHCSR, 0xA05F000D);     /* DBGKEY|C_DEBUGEN|C_MASKINTS|C_STEP */
}

static int cortex_resume(void)
{
    return mem_wr32(DHCSR, 0xA05F0001);     /* DBGKEY|C_DEBUGEN */
}

/* Software-watchpoint fallback.  The DWT on the earliest Cortex-M3 r1p0
 * cores (first STM32F103 silicon) is clocked but its comparators never
 * generate a debug halt, so hardware watchpoints cannot stop the CPU.
 * We record watchpoints here and, on resume, single-step the CPU comparing
 * the watched memory; when it changes we leave the core halted exactly as if
 * the watchpoint had fired.  On cores with a working DWT this is a harmless
 * (slower) duplicate of the hardware mechanism. */
#define SW_WP_MAX 8
typedef struct {
    uint32_t addr;
    uint8_t  kind;
    bool     active;
} sw_wp_t;
static sw_wp_t sw_wp[SW_WP_MAX];

static int sw_wp_add(uint32_t addr, uint8_t kind)
{
    for (int i = 0; i < SW_WP_MAX; i++) {
        if (!sw_wp[i].active) {
            sw_wp[i].addr = addr;
            sw_wp[i].kind = kind;
            sw_wp[i].active = true;
            return i;
        }
    }
    return -1;
}

static void sw_wp_remove(uint32_t addr)
{
    for (int i = 0; i < SW_WP_MAX; i++)
        if (sw_wp[i].active && sw_wp[i].addr == addr)
            sw_wp[i].active = false;
}

static void sw_wp_clear_all(void)
{
    for (int i = 0; i < SW_WP_MAX; i++) sw_wp[i].active = false;
}

static int cortex_read_reg(uint32_t sel, uint32_t *out);

/* Resume, honouring software watchpoints: step the core one instruction at
 * a time and halt as soon as any watched memory location changes. */
static int cortex_resume_checked(void)
{
    int n = 0;
    uint32_t addrs[SW_WP_MAX], old[SW_WP_MAX];
    uint8_t kinds[SW_WP_MAX];
    for (int i = 0; i < SW_WP_MAX; i++) {
        if (!sw_wp[i].active) continue;
        uint32_t v = 0;
        int st = mem_read(sw_wp[i].addr, (uint8_t *)&v, 4);
        if (st != 1) return st;
        addrs[n] = sw_wp[i].addr;
        kinds[n] = sw_wp[i].kind;
        old[n] = v;
        n++;
    }
    if (n == 0) return cortex_resume();

    DBG_PRINT("RC n=");
    DBG_PRINT(n);
    DBG_PRINT(" wp=");
    DBG_PRINT(addrs[0], HEX);
    DBG_PRINT(" old=");
    DBG_PRINTLN(old[0], HEX);

    const int max_steps = 3000;
    for (int i = 0; i < max_steps; i++) {
        if ((i & 0x3F) == 0) yield();
        int st = cortex_step_write();           /* single step, interrupts masked */
        if (st != 1) { DBG_PRINT("RC step-wr fail "); DBG_PRINTLN(st); return st; }
        bool h = false;
        st = cortex_poll_halted(100, &h);
        if (st != 1) { DBG_PRINT("RC poll fail "); DBG_PRINTLN(st); return st; }
        if (!h) { DBG_PRINTLN("RC not halted after step"); return -7; }
        if ((i % 200) == 0) {
            uint32_t pc = 0;
            cortex_read_reg(15, &pc);
            DBG_PRINT("RC step ");
            DBG_PRINT(i);
            DBG_PRINT(" pc=");
            DBG_PRINTLN(pc, HEX);
        }
        for (int k = 0; k < n; k++) {
            if (kinds[k] != 7 && kinds[k] != 4) continue;  /* write/any only */
            uint32_t v = 0;
            st = mem_read(addrs[k], (uint8_t *)&v, 4);
            if (st != 1) return st;
            if (v != old[k]) {
                DBG_PRINT("WP hit 0x");
                DBG_PRINT(addrs[k], HEX);
                DBG_PRINT(" old=");
                DBG_PRINT(old[k], HEX);
                DBG_PRINT(" new=");
                DBG_PRINTLN(v, HEX);
                return 1;                       /* leave core halted */
            }
        }
    }
    DBG_PRINTLN("WP step limit reached, resuming");
    return cortex_resume();
}

static int cortex_step(void)
{
    int st = cortex_step_write();
    if (st != 1) return st;
    bool h = false;
    st = cortex_poll_halted(2000, &h);
    if (st != 1) return st;
    if (h) cortex_read_clear_dfsr();
    return h ? 1 : -7;
}

static int cortex_read_reg(uint32_t sel, uint32_t *out)
{
    int st = mem_wr32(DCRSR, sel);          /* REGWnR = 0 (read) */
    if (st != 1) return st;
    uint32_t dummy;
    st = mem_rd32(DHCSR, &dummy);           /* barrier */
    if (st != 1) return st;
    return mem_rd32(DCRDR, out);
}

static int cortex_write_reg(uint32_t sel, uint32_t val)
{
    int st = mem_wr32(DCRDR, val);
    if (st != 1) return st;
    st = mem_wr32(DCRSR, sel | 0x00010000); /* REGWnR = 1 (write) */
    if (st != 1) return st;
    uint32_t dummy;
    return mem_rd32(DHCSR, &dummy);         /* barrier */
}

static int cortex_status(uint32_t *dhcsr)
{
    return mem_rd32(DHCSR, dhcsr);
}

static int cortex_reset_sysreq(void)
{
    return mem_wr32(AIRCR, 0x05FA0000u | 0x04u);   /* SYSRESETREQ */
}

/* ------------------------------------------------------------------ *
 * Breakpoints (FPB) and watchpoints (DWT).
 * ------------------------------------------------------------------ */

#define FPB_CTRL   0xE0002000u
#define FPB_COMP0  0xE0002008u
#define DWT_COMP0  0xE0001020u
#define DWT_FUNC0  0xE0001024u

static int debug_enable_trace(void)
{
    uint32_t demcr;
    int st = mem_rd32(DEMCR, &demcr);
    if (st != 1) return st;
    if (!(demcr & 0x01000000u)) {           /* TRCENA */
        st = mem_wr32(DEMCR, demcr | 0x01000000u);
        if (st != 1) return st;
    }
    return 1;
}

/* Detect FPB comparator layout via FP_CTRL.REV (bits 31:28): REV==0 is V1
 * (Cortex-M3/M4 -- COMP reserves bits 1 and 29, low address bit goes in
 * REPLACE[31:30]); any other value is V2 (full address in COMP[31:1]).
 * This is the architected field for exactly this purpose (see blackmagic
 * cortexm.c: priv->flash_patch_revision from CORTEXM_FPB_CTRL_REV_SHIFT) --
 * more reliable than probing behavior by writing all-ones and guessing from
 * which bits come back set. */
static int fpb_is_v1(void)
{
    static int v1 = -1;
    if (v1 >= 0) return v1;
    uint32_t ctrl;
    if (mem_rd32(FPB_CTRL, &ctrl) != 1) return -1;
    v1 = (((ctrl >> 28) & 0xFu) == 0) ? 1 : 0;
    return v1;
}

static uint32_t fpb_comp_val(int v1, uint32_t addr)
{
    if (v1) return (addr & 0x1FFFFFFCu) | ((addr & 2) ? 0x80000000u : 0x40000000u) | 1u;
    return (addr & 0x7FFFFFFEu) | 1u;
}

static int bp_set(uint32_t addr, int *slot)
{
    int v1 = fpb_is_v1();
    if (v1 < 0) return v1;
    if (v1 && (addr & 0x20000000u)) return -15;  /* V1 code region only */
    int st = debug_enable_trace();
    if (st != 1) return st;
    uint32_t ctrl;
    st = mem_rd32(FPB_CTRL, &ctrl);
    if (st != 1) return st;
    if (!(ctrl & 1)) {
        /* KEY is bit 1, not bit 31 -- confirmed against blackmagic
         * (CORTEXM_FPB_CTRL_KEY = 1U<<1U). The previous 0x80000000 landed on
         * FP_CTRL's read-only REV field, a no-op, so ENABLE may never have
         * actually latched: this was the likely reason breakpoints armed
         * cleanly (SPI-level writes all succeeded) but never halted the
         * core. */
        st = mem_wr32(FPB_CTRL, 0x00000002u | 1u);  /* KEY | ENABLE */
        if (st != 1) return st;
    }
    for (int i = 0; i < 6; i++) {
        uint32_t comp;
        st = mem_rd32(FPB_COMP0 + 4 * i, &comp);
        if (st != 1) return st;
        if (!(comp & 1)) {
            st = mem_wr32(FPB_COMP0 + 4 * i, fpb_comp_val(v1, addr));
            if (st != 1) return st;
            if (slot) *slot = i;
            return 1;
        }
    }
    return -8;                              /* no free comparator */
}

static int bp_clear(uint32_t addr)
{
    int v1 = fpb_is_v1();
    if (v1 < 0) return v1;
    for (int i = 0; i < 6; i++) {
        uint32_t comp;
        int st = mem_rd32(FPB_COMP0 + 4 * i, &comp);
        if (st != 1) return st;
        if (comp == fpb_comp_val(v1, addr)) {
            st = mem_wr32(FPB_COMP0 + 4 * i, 0);
            if (st != 1) return st;
            return 1;
        }
    }
    return 1;                               /* not found is OK */
}

static int bp_clear_all(void)
{
    for (int i = 0; i < 6; i++) {
        uint32_t comp;
        int st = mem_rd32(FPB_COMP0 + 4 * i, &comp);
        if (st != 1) return st;
        if (comp & 1) {
            st = mem_wr32(FPB_COMP0 + 4 * i, 0);
            if (st != 1) return st;
        }
    }
    return 1;
}

/* kind: 4 = any access, 6 = read, 7 = write (DWT_FUNCTION encodings). */
static int wp_set(uint32_t addr, uint32_t kind, int *slot)
{
    int st = debug_enable_trace();
    if (st != 1) return st;
    int dwt_slot = -1;
    for (int i = 0; i < 4; i++) {
        uint32_t fn;
        st = mem_rd32(DWT_FUNC0 + 16 * i, &fn);
        if (st != 1) return st;
        if ((fn & 0x1F) == 0) {
            st = mem_wr32(DWT_COMP0 + 16 * i, addr);
            if (st != 1) return st;
            st = mem_wr32(DWT_FUNC0 + 16 * i, kind & 0x1F);
            if (st != 1) return st;
            dwt_slot = i;
            break;
        }
    }
    int sw = sw_wp_add(addr, kind);
    if (dwt_slot < 0 && sw < 0) return -9;  /* no hardware or software slot */
    if (slot) *slot = (dwt_slot >= 0) ? dwt_slot : sw;
    return 1;
}

static int wp_clear(uint32_t addr)
{
    sw_wp_remove(addr);
    for (int i = 0; i < 4; i++) {
        uint32_t comp;
        int st = mem_rd32(DWT_COMP0 + 16 * i, &comp);
        if (st != 1) return st;
        if (comp == addr) {
            st = mem_wr32(DWT_FUNC0 + 16 * i, 0);
            if (st != 1) return st;
            return 1;
        }
    }
    return 1;
}

static int wp_clear_all(void)
{
    sw_wp_clear_all();
    for (int i = 0; i < 4; i++) {
        uint32_t fn;
        int st = mem_rd32(DWT_FUNC0 + 16 * i, &fn);
        if (st != 1) return st;
        if (fn & 0x1F) {
            st = mem_wr32(DWT_FUNC0 + 16 * i, 0);
            if (st != 1) return st;
        }
    }
    return 1;
}

/* Clear any leftover breakpoints/watchpoints from a previous session so a
 * new client starts with a clean debug state.  Best-effort: failures are
 * ignored so a connection never breaks on cleanup. */
static void cortex_cleanup(void)
{
    if (swd_init() != 1) return;
    /* SYSRESETREQ resets the STM32 core but not its always-on debug block,
     * so a C_HALT/C_STEP/C_MASKINTS left over from a previous session (or a
     * client that disconnected mid-step) survives target resets and even
     * ESP32 reboots. Releasing debug entirely puts DHCSR back to its true
     * power-on state so a freshly (re)connected client starts clean. */
    mem_wr32(DHCSR, 0xA05F0000);
    bp_clear_all();
    wp_clear_all();
}

/* ------------------------------------------------------------------ *
 * STM32F1 flash programming. Bank1-only register addresses, page size, and
 * dual-bank support all verified against blackmagic's stm32f1.c rather than
 * assumed -- bank1 layout matched what was already here byte-for-byte (a
 * good sign the original constants were already correct), which is the
 * useful part: it means the dual-bank/page-size additions below extend a
 * correct base instead of guessing on top of an unverified one.
 * ------------------------------------------------------------------ */

#define FLASH_BASE       0x40022000u
#define FLASH_BANK2_OFF  0x40u              /* XL-density only */
#define FLASH_BANK_SPLIT 0x08080000u
#define FLASH_KEYR(off)  (FLASH_BASE + 0x04u + (off))
#define FLASH_SR(off)    (FLASH_BASE + 0x0Cu + (off))
#define FLASH_CR(off)    (FLASH_BASE + 0x10u + (off))
#define FLASH_AR(off)    (FLASH_BASE + 0x14u + (off))

#define DBGMCU_IDCODE    0xE0042000u

/* Device density -> page size / dual-bank, from DBGMCU_IDCODE's low 12 bits
 * (confirmed against blackmagic's stm32f1_probe(): 0x410/0x412 medium/low
 * density = 1KB pages single bank; 0x414/0x418 high density/connectivity
 * line = 2KB pages single bank; 0x430 XL-density = 2KB pages, dual bank).
 * This board's own idcode (0x20030410, from the native 'I' command) is
 * medium density -- exactly the default this falls back to, so behavior on
 * tested hardware is unchanged either way. */
static uint32_t g_flash_page_size = 0x400u;
static bool     g_flash_dual_bank = false;
static bool     g_flash_ident_done = false;

static void flash_identify(void)
{
    if (g_flash_ident_done) return;
    uint32_t idcode = 0;
    if (mem_rd32(DBGMCU_IDCODE, &idcode) != 1) return;  /* leave defaults */
    switch (idcode & 0xFFFu) {
    case 0x412u: case 0x410u:                 /* low / medium density */
        g_flash_page_size = 0x400u; g_flash_dual_bank = false; break;
    case 0x414u: case 0x418u:                 /* high density / connectivity line */
        g_flash_page_size = 0x800u; g_flash_dual_bank = false; break;
    case 0x430u:                               /* XL-density: dual bank */
        g_flash_page_size = 0x800u; g_flash_dual_bank = true; break;
    default:
        g_flash_page_size = 0x400u; g_flash_dual_bank = false; break;
    }
    g_flash_ident_done = true;
}

static uint32_t flash_bank_offset(uint32_t addr)
{
    return (g_flash_dual_bank && addr >= FLASH_BANK_SPLIT) ? FLASH_BANK2_OFF : 0u;
}

static int flash_wait_bsy(uint32_t bank_off, uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    do {
        uint32_t sr;
        int st = mem_rd32(FLASH_SR(bank_off), &sr);
        if (st != 1) return st;
        if (!(sr & 1)) {
            if (sr & 0x0014) return -10;    /* PGERR | WRPRTERR */
            return 1;
        }
        delay(1);
    } while (millis() - t0 < timeout_ms);
    return -11;
}

static int flash_unlock(uint32_t bank_off)
{
    int st = mem_wr32(FLASH_KEYR(bank_off), 0x45670123);
    if (st != 1) return st;
    return mem_wr32(FLASH_KEYR(bank_off), 0xCDEF89AB);
}

static int flash_lock(uint32_t bank_off)
{
    return mem_wr32(FLASH_CR(bank_off), 0x00000080);  /* LOCK */
}

static int flash_page_erase(uint32_t addr)
{
    uint32_t bank_off = flash_bank_offset(addr);
    int st = mem_wr32(FLASH_CR(bank_off), 0x00000002);    /* PER */
    if (st != 1) return st;
    st = mem_wr32(FLASH_AR(bank_off), addr);
    if (st != 1) return st;
    st = mem_wr32(FLASH_CR(bank_off), 0x00000042);        /* PER | STRT */
    if (st != 1) return st;
    return flash_wait_bsy(bank_off, 2000);
}

static int flash_mass_erase_bank(uint32_t bank_off)
{
    int st = mem_wr32(FLASH_CR(bank_off), 0x00000004);    /* MER */
    if (st != 1) return st;
    st = mem_wr32(FLASH_CR(bank_off), 0x00000044);        /* MER | STRT */
    if (st != 1) return st;
    return flash_wait_bsy(bank_off, 2000);
}

/* Dual-bank mass erase does each bank separately with its own unlock --
 * confirmed against blackmagic's stm32f1_mass_erase(), which has no single
 * register/bit that erases both banks at once. */
static int flash_mass_erase(void)
{
    int st = flash_unlock(0u);
    if (st != 1) return st;
    st = flash_mass_erase_bank(0u);
    flash_lock(0u);
    if (st != 1 || !g_flash_dual_bank) return st;

    st = flash_unlock(FLASH_BANK2_OFF);
    if (st != 1) return st;
    st = flash_mass_erase_bank(FLASH_BANK2_OFF);
    flash_lock(FLASH_BANK2_OFF);
    return st;
}

static int flash_erase_region(uint32_t addr, uint32_t len)
{
    flash_identify();

    if (addr == 0 && len == 0)
        return flash_mass_erase();

    uint32_t end = addr + len;
    bool need1 = !g_flash_dual_bank || addr < FLASH_BANK_SPLIT;
    bool need2 = g_flash_dual_bank && end > FLASH_BANK_SPLIT;

    int st = 1;
    if (need1) st = flash_unlock(0u);
    if (st == 1 && need2) st = flash_unlock(FLASH_BANK2_OFF);
    if (st == 1) {
        for (uint32_t a = addr; a < end; a += g_flash_page_size) {
            st = flash_page_erase(a);
            if (st != 1) break;
        }
    }
    if (need2) flash_lock(FLASH_BANK2_OFF);
    if (need1) flash_lock(0u);
    return st;
}

/* PG only needs to be set once per contiguous run on a bank, not once per
 * halfword -- it stays latched until explicitly cleared, and re-writing it
 * on every single write was a full extra SWD register-write transaction per
 * halfword for no reason (confirmed against blackmagic's stm32f1_flash_write:
 * PG is set once before the aligned write loop, not per write). On this
 * board (single bank) that means PG is set exactly once for an entire
 * program call instead of once per halfword. */
static int flash_program_region(uint32_t addr, const uint8_t *data, uint32_t len)
{
    flash_identify();

    uint32_t end = addr + len;
    bool need1 = !g_flash_dual_bank || addr < FLASH_BANK_SPLIT;
    bool need2 = g_flash_dual_bank && end > FLASH_BANK_SPLIT;

    int st = 1;
    if (need1) st = flash_unlock(0u);
    if (st == 1 && need2) st = flash_unlock(FLASH_BANK2_OFF);

    if (st == 1) {
        uint32_t last_bank_off = 0xFFFFFFFFu;   /* forces PG on first iter */
        for (uint32_t off = 0; off + 1 < len; off += 2) {
            if ((off & 0xFF) == 0) yield();
            uint32_t a = addr + off;
            uint32_t bank_off = flash_bank_offset(a);
            if (bank_off != last_bank_off) {
                st = mem_wr32(FLASH_CR(bank_off), 0x00000001);  /* PG */
                if (st != 1) break;
                last_bank_off = bank_off;
            }
            uint16_t hw = (uint16_t)(data[off] | ((uint16_t)data[off + 1] << 8));
            st = mem_write(a, (const uint8_t *)&hw, 2);
            if (st != 1) break;
            st = flash_wait_bsy(bank_off, 500);
            if (st != 1) break;
        }
    }
    if (need2) flash_lock(FLASH_BANK2_OFF);
    if (need1) flash_lock(0u);
    return st;
}

static int flash_verify_region(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return -12;
    int st = mem_read(addr, buf, len);
    if (st == 1 && memcmp(buf, data, len) != 0) st = -13;
    free(buf);
    return st;
}

/* Program a region: halt, erase covering pages, program, verify. */
static int flash_program_flash(uint32_t addr, const uint8_t *data, uint32_t len)
{
    int st = cortex_halt();
    if (st != 1) return st;
    st = flash_erase_region(addr, len);
    if (st != 1) return st;
    st = flash_program_region(addr, data, len);
    if (st != 1) return st;
    return flash_verify_region(addr, data, len);
}

/* ------------------------------------------------------------------ *
 * Native binary protocol (port 5555).  Reply is <status:u8> <detail:u8>
 * followed by optional payload.  status 0 = OK.
 * ------------------------------------------------------------------ */

static bool recv_bytes(Stream &c, uint8_t *buf, int n, unsigned long timeout_ms)
{
    unsigned long t0 = millis();
    int got = 0;
    while (got < n) {
        if (c.available()) {
            buf[got++] = (uint8_t)c.read();
        } else if (millis() - t0 > timeout_ms) {
            return false;
        } else {
            delay(1);
        }
    }
    return got == n;
}

static void reply_st(Stream &c, int st)
{
    if (st == 1) {
        c.write((uint8_t)0);
        c.write((uint8_t)0);
    } else {
        c.write((uint8_t)1);
        c.write((uint8_t)(st & 0xFF));
    }
}

#define RUN_OP(op) do {                                                       \
    int __st = swd_init();                                                    \
    if (__st == 1) __st = (op);                                               \
    if (__st != 1) {                                                          \
        __st = swd_init();                                                    \
        if (__st == 1) __st = (op);                                           \
    }                                                                         \
    reply_st(c, __st);                                                        \
} while (0)

static void handle_native_cmd(char cmd, Stream &c)
{
    switch (cmd) {

    case 'A':                                   /* alive ping */
        reply_st(c, 1);
        break;

    case 'X': {                                 /* block read */
        uint8_t hdr[6];
        if (!recv_bytes(c, hdr, 6, 5000)) { reply_st(c, -14); break; }
        uint32_t addr = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint32_t nwords = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8);
        if (nwords == 0) nwords = 1;
        if (nwords > 1024) nwords = 1024;
        uint32_t *buf = (uint32_t *)malloc(nwords * 4);
        if (!buf) { reply_st(c, -12); break; }
        int st = swd_init();
        if (st == 1) st = mem_read(addr, (uint8_t *)buf, nwords * 4);
        if (st != 1) {
            st = swd_init();
            if (st == 1) st = mem_read(addr, (uint8_t *)buf, nwords * 4);
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            c.write((const uint8_t *)buf, (size_t)(nwords * 4));
        } else {
            reply_st(c, st);
        }
        free(buf);
        break;
    }

    case 'M': {                                 /* memory write */
        uint8_t hdr[8];
        if (!recv_bytes(c, hdr, 8, 5000)) { reply_st(c, -14); break; }
        uint32_t addr = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint32_t len = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        if (len > 4096) { reply_st(c, -14); break; }
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        if (!buf) { reply_st(c, -12); break; }
        if (!recv_bytes(c, buf, len, 30000)) {
            free(buf); reply_st(c, -14); break;
        }
        RUN_OP(mem_write(addr, buf, len));
        free(buf);
        break;
    }

    case 'H':                                   /* halt */
        RUN_OP(cortex_halt());
        break;

    case 'C':                                   /* resume */
        RUN_OP(cortex_resume_checked());
        break;

    case 'S':                                   /* step */
        RUN_OP(cortex_step());
        break;

    case 'U':                                   /* system reset */
        RUN_OP(cortex_reset_sysreq());
        break;

    case 'T': {                                 /* status */
        int st = swd_init();
        if (st != 1) { reply_st(c, st); break; }
        uint32_t dhcsr = 0;
        st = mem_rd32(DHCSR, &dhcsr);
        if (st != 1) {
            st = swd_init();
            if (st == 1) st = mem_rd32(DHCSR, &dhcsr);
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            for (int i = 0; i < 4; i++) c.write((uint8_t)(dhcsr >> (8 * i)));
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'G': {                                 /* read core regs r0-r15+xpsr */
        uint32_t regs[17];
        int st = swd_init();
        if (st == 1) {
            for (int i = 0; i < 16; i++) {
                st = cortex_read_reg(i, &regs[i]);
                if (st != 1) break;
            }
            if (st == 1) st = cortex_read_reg(16, &regs[16]);
        }
        if (st != 1) {
            st = swd_init();
            for (int i = 0; i < 16 && st == 1; i++)
                st = cortex_read_reg(i, &regs[i]);
            if (st == 1) st = cortex_read_reg(16, &regs[16]);
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            for (int i = 0; i < 17; i++) {
                for (int k = 0; k < 4; k++)
                    c.write((uint8_t)(regs[i] >> (8 * k)));
            }
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'N': {                                 /* write core reg */
        uint8_t hdr[5];
        if (!recv_bytes(c, hdr, 5, 5000)) { reply_st(c, -14); break; }
        uint32_t sel = hdr[0];
        uint32_t val = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                       ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        RUN_OP(cortex_write_reg(sel, val));
        break;
    }

    case 'F': {                                 /* FPB breakpoint */
        uint8_t hdr[5];
        if (!recv_bytes(c, hdr, 5, 5000)) { reply_st(c, -14); break; }
        uint8_t op = hdr[0];
        uint32_t addr = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                        ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        int slot = -1;
        int st = swd_init();
        if (st == 1) {
            if (op == 1) st = bp_set(addr, &slot);
            else if (op == 0) st = bp_clear(addr);
            else st = bp_clear_all();
        }
        if (st != 1) {
            st = swd_init();
            if (st == 1) {
                if (op == 1) st = bp_set(addr, &slot);
                else if (op == 0) st = bp_clear(addr);
                else st = bp_clear_all();
            }
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            c.write((uint8_t)(slot & 0xFF));
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'V': {                                 /* DWT watchpoint */
        uint8_t hdr[6];
        if (!recv_bytes(c, hdr, 6, 5000)) { reply_st(c, -14); break; }
        uint8_t op = hdr[0];
        uint32_t addr = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                        ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        uint8_t kind = hdr[5];
        int slot = -1;
        int st = swd_init();
        if (st == 1) {
            if (op == 1) st = wp_set(addr, kind, &slot);
            else if (op == 0) st = wp_clear(addr);
            else st = wp_clear_all();
        }
        if (st != 1) {
            st = swd_init();
            if (st == 1) {
                if (op == 1) st = wp_set(addr, kind, &slot);
                else if (op == 0) st = wp_clear(addr);
                else st = wp_clear_all();
            }
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            c.write((uint8_t)(slot & 0xFF));
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'Y': {                                 /* flash erase */
        uint8_t hdr[8];
        if (!recv_bytes(c, hdr, 8, 5000)) { reply_st(c, -14); break; }
        uint32_t addr = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint32_t len = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        int st = swd_init();
        if (st == 1) {
            st = cortex_halt();
            if (st == 1) st = flash_erase_region(addr, len);
        }
        if (st != 1) {
            st = swd_init();
            if (st == 1) {
                st = cortex_halt();
                if (st == 1) st = flash_erase_region(addr, len);
            }
        }
        reply_st(c, st);
        break;
    }

    case 'P': {                                 /* flash program + verify */
        uint8_t hdr[8];
        if (!recv_bytes(c, hdr, 8, 5000)) { reply_st(c, -14); break; }
        uint32_t addr = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint32_t len = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        if (len > 4096 || (len & 1) || (addr & 1)) { reply_st(c, -14); break; }
        uint8_t *buf = (uint8_t *)malloc(len ? len : 2);
        if (!buf) { reply_st(c, -12); break; }
        if (!recv_bytes(c, buf, len, 30000)) {
            free(buf); reply_st(c, -14); break;
        }
        int st = swd_init();
        if (st == 1) st = flash_program_flash(addr, buf, len);
        if (st != 1) {
            st = swd_init();
            if (st == 1) st = flash_program_flash(addr, buf, len);
        }
        reply_st(c, st);
        free(buf);
        break;
    }

    case 'D': {                                 /* AP/DP register read */
        uint8_t hdr[3];
        if (!recv_bytes(c, hdr, 3, 5000)) { reply_st(c, -14); break; }
        uint8_t is_ap = hdr[0], bank = hdr[1], reg = hdr[2];
        uint32_t val = 0;
        int st = swd_init();
        if (st == 1 && is_ap) st = ap_probe();
        if (st == 1) {
            /* bank<<4 used to implicitly assume APSEL 0 (the AP index byte
             * lived in bits[31:24], never touched here) -- now ORs in the
             * auto-probed AP instead of hardcoding it. Identical behavior
             * on this board, since g_ap_sel resolves to 0 here anyway. */
            if (is_ap) st = swd_select((g_ap_sel << 24) | ((uint32_t)bank << 4));
            if (st == 1) {
                if (is_ap) {
                    /* AP reads are posted: prime AP read, then get data
                     * from DP RDBUFF. */
                    st = swd_read_reg(true, reg & 0xc, &val, true);
                    if (st == 1) st = swd_read_reg(false, 0x0c, &val, false);
                } else {
                    st = swd_read_reg(false, reg & 0xc, &val, false);
                }
            }
        }
        if (st != 1) {
            st = swd_init();
            if (st == 1 && is_ap) st = ap_probe();
            if (st == 1) {
                if (is_ap) st = swd_select((g_ap_sel << 24) | ((uint32_t)bank << 4));
                if (st == 1) {
                    if (is_ap) {
                        st = swd_read_reg(true, reg & 0xc, &val, true);
                        if (st == 1) st = swd_read_reg(false, 0x0c, &val, false);
                    } else {
                        st = swd_read_reg(false, reg & 0xc, &val, false);
                    }
                }
            }
        }
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            for (int i = 0; i < 4; i++) c.write((uint8_t)(val >> (8 * i)));
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'W': {                                 /* AP/DP register write */
        uint8_t hdr[7];
        if (!recv_bytes(c, hdr, 7, 5000)) { reply_st(c, -14); break; }
        uint8_t is_ap = hdr[0], bank = hdr[1], reg = hdr[2];
        uint32_t val = (uint32_t)hdr[3] | ((uint32_t)hdr[4] << 8) |
                       ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 24);
        int st = swd_init();
        if (st == 1 && is_ap) st = ap_probe();
        if (st == 1) {
            if (is_ap) st = swd_select((g_ap_sel << 24) | ((uint32_t)bank << 4));
            if (st == 1) {
                if (is_ap) st = swd_write_reg(true, reg & 0xc, val, true);
                else st = swd_write_reg(false, reg & 0xc, val, false);
            }
        }
        if (st != 1) {
            st = swd_init();
            if (st == 1 && is_ap) st = ap_probe();
            if (st == 1) {
                if (is_ap) st = swd_select((g_ap_sel << 24) | ((uint32_t)bank << 4));
                if (st == 1) {
                    if (is_ap) st = swd_write_reg(true, reg & 0xc, val, true);
                    else st = swd_write_reg(false, reg & 0xc, val, false);
                }
            }
        }
        reply_st(c, st);
        break;
    }

    case 'J': {                                  /* halt reason: live DFSR read+clear */
        /* cortex_halt()/cortex_step() cache DFSR right after they cause a
         * halt, but a breakpoint or watchpoint can also halt the core
         * asynchronously while it's free-running after 'C' -- read live
         * here rather than trusting the cache, or an async halt's real
         * reason is invisible until the next explicit halt/step. */
        int st = cortex_read_clear_dfsr();
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            for (int i = 0; i < 4; i++) c.write((uint8_t)(g_last_dfsr >> (8 * i)));
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'K': {                                  /* extended part info: CPUID, flash geometry, AP used */
        uint32_t cpuid = 0;
        int st = swd_init();
        if (st == 1) st = ap_probe();
        if (st == 1) st = mem_rd32(0xE000ED00u, &cpuid);       /* CPUID */
        if (st == 1) flash_identify();
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            uint32_t v[2] = { cpuid, g_flash_page_size };
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 4; k++)
                    c.write((uint8_t)(v[i] >> (8 * k)));
            c.write((uint8_t)(g_flash_dual_bank ? 1 : 0));
            c.write((uint8_t)g_ap_sel);
        } else {
            reply_st(c, st);
        }
        break;
    }

    case 'I': {                                 /* chip info */
        uint32_t dpidr = 0, fsize = 0, idcode = 0;
        int st = swd_init();
        if (st == 1) st = swd_read_reg(false, 0x00, &dpidr, false);
        if (st == 1) st = mem_rd32(0x1FFFF7E0u, &fsize);
        if (st == 1) st = mem_rd32(0xE0042000u, &idcode);
        if (st == 1) {
            c.write((uint8_t)0);
            c.write((uint8_t)0);
            uint32_t v[3] = { dpidr, fsize, idcode };
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 4; k++)
                    c.write((uint8_t)(v[i] >> (8 * k)));
        } else {
            reply_st(c, st);
        }
        break;
    }

    default:
        reply_st(c, -14);
        break;
    }
}


void setup()
{
    pinMode(PIN_SWCLK, OUTPUT);
    pinMode(PIN_SWDIO, OUTPUT);
    pinMode(PIN_NRST,  OUTPUT);
    pinMode(PIN_LED,   OUTPUT);
    digitalWrite(PIN_SWCLK, HIGH);
    digitalWrite(PIN_SWDIO, HIGH);
    digitalWrite(PIN_NRST,  HIGH);
    digitalWrite(PIN_LED,   HIGH);   /* active-low on ESP8266: HIGH = off */

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("ESP8266 ST-Link probe (serial-only) starting...");
    Serial.print("SWCLK=GPIO"); Serial.print(PIN_SWCLK);
    Serial.print("  SWDIO=GPIO"); Serial.print(PIN_SWDIO);
    Serial.print("  NRST=GPIO"); Serial.println(PIN_NRST);

    /* A serial link has no per-client "connect" event, so release any stale
     * debug-halt state left on the target right at boot. Best-effort: does
     * nothing if no target is wired/powered yet. */
    cortex_cleanup();

    Serial.println("Ready. Waiting for host commands...");
}

void loop()
{
    /* Serial command channel: native binary protocol, no WiFi needed. */
    if (Serial.available()) {
        serial_cmd_active = true;
        char cmd = (char)Serial.read();
        /* 'Q' and remote_bitbang single-char commands are ignored here;
         * the host client only speaks the native binary protocol. */
        if (cmd != 'Q' && !strchr("01234567RrstuOocdefgZzBb", cmd)) {
            handle_native_cmd(cmd, Serial);
        }
    }
    yield();
}

