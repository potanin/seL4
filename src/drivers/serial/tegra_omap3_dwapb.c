/*
 * Copyright 2016, General Dynamics C4 Systems
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <config.h>
#include <stdint.h>
#include <util.h>
#include <machine/io.h>
#include <plat/machine/devices_gen.h>

#define UTHR        0x00
#define UIER        0x04
#define UFCR        0x08
#define ULCR        0x0C
#define UMCR        0x10
#define ULSR        0x14
#define UDLL        0x00
#define UDLM        0x04

#define ULSR_RDR    BIT(0)
#define ULSR_OE     BIT(1)
#define ULSR_PE     BIT(2)
#define ULSR_FE     BIT(3)
#define ULSR_BI     BIT(4)
#define ULSR_THRE   BIT(5)
#define ULSR_TEMT   BIT(6)
#define ULSR_RXFE   BIT(7)
#define ULSR_ERR    (ULSR_OE | ULSR_PE | ULSR_FE | ULSR_BI)
#define ULCR_DLAB   BIT(7)

#define UART_REG(x) ((volatile uint32_t *)(UART_PPTR + (x)))

#ifdef BPMP_HSP_DB_PPTR
static volatile uint32_t *bpmp_rx_ptr = (volatile uint32_t *)BPMP_RX_PPTR;

/*
 * Ack any un-acked BPMP responses (from timed-out sends).
 * Must be called before sending a new message to keep IVC in sync.
 */
static void bpmp_drain_pending(void)
{
    volatile uint32_t *rx = bpmp_rx_ptr;
    uint32_t r = rx[0];
    uint32_t ack = rx[16];
    if (r != ack) {
        rx[16] = r;
        asm volatile("dsb sy" ::: "memory");
    }
}

/*
 * Send one BPMP IPC message with configurable timeout.
 * Returns 1 on success, 0 on timeout.
 * After success, rx[32] = mrq_response.err (0=ok), rx[34..] = response payload.
 */
static int bpmp_send_tmo(uint32_t mrq, const uint32_t *payload, int nwords,
                          int tmo)
{
    volatile uint32_t *tx = (volatile uint32_t *)BPMP_TX_PPTR;
    volatile uint32_t *rx = bpmp_rx_ptr;
    volatile uint32_t *db = (volatile uint32_t *)(BPMP_HSP_DB_PPTR + 0x300);

    bpmp_drain_pending();

    uint32_t tc = tx[0];
    uint32_t rx_exp = rx[0] + 1;

    tx[32] = mrq;          /* MRQ code */
    tx[33] = 2;            /* flags = MSG_RING */
    for (int i = 0; i < nwords; i++) {
        tx[34 + i] = payload[i];
    }

    asm volatile("dsb sy" ::: "memory");
    tx[0] = tc + 1;
    asm volatile("dsb sy" ::: "memory");
    *db = 1;
    asm volatile("dsb sy" ::: "memory");

    while (rx[0] != rx_exp && --tmo > 0);
    if (tmo > 0) {
        rx[16] = rx_exp;
        asm volatile("dsb sy" ::: "memory");
    }
    return tmo > 0;
}

/* Normal send: ~10ms timeout (for putchar path) */
static int bpmp_send(uint32_t mrq, const uint32_t *payload, int nwords)
{
    return bpmp_send_tmo(mrq, payload, nwords, 2000000);
}

/*
 * Re-enable UARTA clock via BPMP IPC.
 * Sends CLK_ENABLE + SET_RATE + RESET_DEASSERT (full recovery).
 */
static void bpmp_uart_clock_keepalive(void)
{
    uint32_t p[4];

    /* CLK_ENABLE: MRQ_CLK=22, CMD=7, clk_id=155 */
    p[0] = (7 << 24) | 155;
    p[1] = 0;
    bpmp_send(22, p, 2);

    /* CLK_SET_RATE: MRQ_CLK=22, CMD=2, clk_id=155, rate=1843200 */
    p[0] = (2 << 24) | 155;
    p[1] = 0;
    p[2] = 1843200;
    p[3] = 0;
    bpmp_send(22, p, 4);

    /* RESET_DEASSERT: MRQ_RESET=20, CMD=2, rst=100 */
    p[0] = 2;
    p[1] = 100;
    bpmp_send(20, p, 2);
}

/*
 * Proactive UART clock keepalive using ARM generic timer.
 * BPMP firmware (T234) gates the UART clock ~8-9 min after boot.
 * We re-send CLK_ENABLE + SET_RATE every KEEPALIVE_SECONDS.
 */
#define KEEPALIVE_SECONDS 60
#define KEEPALIVE_TICKS ((uint64_t)31250000 * KEEPALIVE_SECONDS)

static uint64_t bpmp_last_keepalive = 0;

static inline uint64_t uart_read_cntpct(void)
{
    uint64_t val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/* Diagnostics — written to by keepalive, read by putchar for output */
static int keepalive_fire_count = 0;
static int keepalive_last_ipc_ok = -1;    /* -1=never, 0=timeout, 1=ok */
static int32_t keepalive_last_mrq_err = 0;
static int32_t keepalive_last_clk_state = -1;  /* -1=unknown, 0=disabled, 1=enabled */

/*
 * Send CLK_ENABLE + SET_RATE (without RESET_DEASSERT which glitches UART).
 * Returns: 1 if both IPCs succeeded AND BPMP returned no error, 0 otherwise.
 */
static int bpmp_clk_enable_and_set_rate(void)
{
    uint32_t p[4];
    volatile uint32_t *rx = bpmp_rx_ptr;

    /* CLK_ENABLE: MRQ_CLK=22, CMD=7, clk_id=155 */
    p[0] = (7 << 24) | 155;
    p[1] = 0;
    if (!bpmp_send(22, p, 2)) {
        keepalive_last_ipc_ok = 0;
        return 0;
    }
    keepalive_last_mrq_err = (int32_t)rx[32];

    /* CLK_SET_RATE: MRQ_CLK=22, CMD=2, clk_id=155, rate=1843200 */
    p[0] = (2 << 24) | 155;
    p[1] = 0;
    p[2] = 1843200;
    p[3] = 0;
    if (!bpmp_send(22, p, 4)) {
        keepalive_last_ipc_ok = 0;
        return 0;
    }

    keepalive_last_ipc_ok = 1;

    /* Query: CMD_CLK_IS_ENABLED (cmd=6) to verify */
    p[0] = (6 << 24) | 155;
    p[1] = 0;
    if (bpmp_send(22, p, 2)) {
        keepalive_last_clk_state = (int32_t)rx[34];
    }

    return (keepalive_last_mrq_err == 0);
}

static void bpmp_keepalive_check(void)
{
    uint64_t now = uart_read_cntpct();
    if (now - bpmp_last_keepalive >= KEEPALIVE_TICKS) {
        bpmp_last_keepalive = now;
        bpmp_clk_enable_and_set_rate();
        keepalive_fire_count++;
    }
}
#endif /* BPMP_HSP_DB_PPTR */

static int uart_initialised = 0;

static void uart_drv_init(void)
{
    if (uart_initialised) {
        return;
    }
    uart_initialised = 1;

    /* Disable interrupts */
    *UART_REG(UIER) = 0;

    /* Wait for any in-progress TX — short timeout in case clock is dead */
    int timeout = 50000;
    while (!(*UART_REG(ULSR) & ULSR_TEMT) && --timeout > 0);

    /* Enable and clear FIFOs */
    *UART_REG(UFCR) = 0x07; /* Enable FIFOs + clear TX/RX */

    /* Set DLAB to access divisor */
    *UART_REG(ULCR) = ULCR_DLAB;

    /* Divisor = 1: clock should be set to 1.8432 MHz by elfloader
     * via BPMP, giving 115200 baud */
    *UART_REG(UDLL) = 1;
    *UART_REG(UDLM) = 0;

    /* 8N1, clear DLAB */
    *UART_REG(ULCR) = 0x03;

    /* No flow control */
    *UART_REG(UMCR) = 0;
}

#ifdef BPMP_HSP_DB_PPTR
/*
 * Write a diagnostic line via raw UART (bypassing putchar to avoid recursion).
 * Only call when UART is known to be working.
 */
static void uart_raw_puts(const char *s)
{
    while (*s) {
        int t = 100000;
        while (!(*UART_REG(ULSR) & ULSR_THRE) && --t > 0);
        *UART_REG(UTHR) = *s++;
    }
}

static void uart_raw_putdec(int val)
{
    char buf[12];
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    if (neg) buf[i++] = '-';
    while (--i >= 0) {
        int t = 100000;
        while (!(*UART_REG(ULSR) & ULSR_THRE) && --t > 0);
        *UART_REG(UTHR) = buf[i];
    }
}

/* Print keepalive diagnostics once, when fire_count transitions from 0->1 */
static int diag_printed = 0;
static void keepalive_print_diag(void)
{
    if (diag_printed || keepalive_fire_count == 0) return;
    diag_printed = 1;
    uart_raw_puts("\n[BPMP KA#1: ipc=");
    uart_raw_putdec(keepalive_last_ipc_ok);
    uart_raw_puts(" err=");
    uart_raw_putdec(keepalive_last_mrq_err);
    uart_raw_puts(" clk=");
    uart_raw_putdec(keepalive_last_clk_state);
    uart_raw_puts("]\n");
}
#endif

/* Always available — needed for SysDebugPutChar even without CONFIG_PRINTING */
void uart_drv_putchar(unsigned char c)
{
    uart_drv_init();

#ifdef BPMP_HSP_DB_PPTR
    /* Time-based proactive keepalive: re-enable UART clock before it dies */
    bpmp_keepalive_check();

    /* Print diagnostics after first keepalive fires */
    keepalive_print_diag();
#endif

    /* Wait for THR/FIFO to have space (THRE).
     * At 115200 baud, 16-byte FIFO drains in ~1.4ms.
     * 100K iterations ~ 1-3ms — enough for full FIFO drain. */
    int timeout = 100000;
    while (timeout-- > 0) {
        uint32_t lsr = *UART_REG(ULSR);
        if (lsr & ULSR_THRE) {
            break;
        }
    }

    if (timeout <= 0) {
        /* UART not responding — BPMP may have gated the clock.
         * Do full clock recovery + UART re-init. */
#ifdef BPMP_HSP_DB_PPTR
        bpmp_uart_clock_keepalive();
        bpmp_last_keepalive = uart_read_cntpct();
#endif
        uart_initialised = 0;
        uart_drv_init();
        timeout = 100000;
        while (!(*UART_REG(ULSR) & ULSR_THRE) && --timeout > 0);
    }

    *UART_REG(UTHR) = c;
}

#ifdef CONFIG_DEBUG_BUILD
unsigned char uart_drv_getchar(void)
{
    uart_drv_init();

    while ((*UART_REG(ULSR) & ULSR_RDR) == 0);

    return *UART_REG(UTHR);
}
#endif /* CONFIG_DEBUG_BUILD */

/*
 * Timer-tick callable keepalive — quick send (~10us timeout).
 * Sends CLK_ENABLE with a very short timeout to avoid blocking the
 * timer interrupt handler. If BPMP doesn't respond in time, the
 * un-acked response is cleaned up by bpmp_drain_pending() on next
 * bpmp_send() call from the putchar path.
 */
void uart_keepalive_tick(void)
{
#ifdef BPMP_HSP_DB_PPTR
    uint64_t now = uart_read_cntpct();
    if (now - bpmp_last_keepalive >= KEEPALIVE_TICKS) {
        bpmp_last_keepalive = now;
        uint32_t p[4];
        /* CLK_ENABLE: CMD=7, clk_id=155 */
        p[0] = (7 << 24) | 155;
        p[1] = 0;
        bpmp_send_tmo(22, p, 2, 500);
        /* CLK_SET_RATE: CMD=2, clk_id=155, rate=1843200 */
        p[0] = (2 << 24) | 155;
        p[1] = 0;
        p[2] = 1843200;
        p[3] = 0;
        bpmp_send_tmo(22, p, 4, 500);
    }
#endif
}
