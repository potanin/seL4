/*
 * Copyright 2021, Axel Heider <axelheider@gmx.de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/* Always available — needed for SysDebugPutChar even without CONFIG_PRINTING */
void uart_drv_putchar(unsigned char c);

static inline void uart_console_putchar(
    unsigned char c)
{
    /* UART console requires printing a '\r' (CR) before any '\n' (LF) */
    if (c == '\n') {
        uart_drv_putchar('\r');
    }
    uart_drv_putchar(c);
}

#ifdef CONFIG_DEBUG_BUILD
unsigned char uart_drv_getchar(void);
#endif

/* BPMP UART clock keepalive — call periodically to prevent BPMP from gating the clock */
void uart_keepalive_tick(void);
