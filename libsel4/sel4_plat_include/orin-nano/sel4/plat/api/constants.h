/*
 * Copyright 2025
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sel4/config.h>

/* Tegra T234 (Orin Nano) has 6x Cortex-A78AE cores.
 * Using Cortex-A72 constants as the closest supported CPU. */
#if defined(CONFIG_ARM_CORTEX_A72)
#include <sel4/arch/constants_cortex_a72.h>
#else
#error "unsupported core"
#endif
