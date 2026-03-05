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
#define ULSR_THRE   BIT(5)
#define ULSR_TEMT   BIT(6)
#define ULCR_DLAB   BIT(7)

#define UART_REG(x) ((volatile uint32_t *)(UART_PPTR + (x)))

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

    /* Divisor = 1: clock is 1.8432 MHz (set by elfloader), giving 115200 baud */
    *UART_REG(UDLL) = 1;
    *UART_REG(UDLM) = 0;

    /* 8N1, clear DLAB */
    *UART_REG(ULCR) = 0x03;

    /* No flow control */
    *UART_REG(UMCR) = 0;
}

/* Always available — needed for SysDebugPutChar even without CONFIG_PRINTING */
void uart_drv_putchar(unsigned char c)
{
    uart_drv_init();

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

