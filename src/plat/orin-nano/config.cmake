#
# Copyright 2025
#
# SPDX-License-Identifier: GPL-2.0-only
#

declare_platform(orin-nano KernelPlatformOrinNano PLAT_ORIN_NANO KernelSel4ArchAarch64)

if(KernelPlatformOrinNano)

    declare_seL4_arch(aarch64)
    # Cortex-A78AE (ARMv8.2) — using A72 as closest supported CPU
    set(KernelArmCortexA72 ON)
    set(KernelArchArmV8a ON)
    # Override A72's 44-bit PA to 40-bit: Orin uses <1TB physical space
    # and 44-bit creates massive device untypeds (up to 2^43) that break sel4test
    set(KernelPlatformPASizeBitsOverride 40)
    set(KernelArmGicV3 ON)
    set(KernelAArch64SErrorIgnore ON)
    set(KernelPrinting ON CACHE BOOL "" FORCE)
    config_set(KernelARMPlatform ARM_PLAT "orin-nano")
    config_set(KernelArmMach MACH "nvidia")
    list(APPEND KernelDTSList "tools/dts/orin-nano.dts")
    list(APPEND KernelDTSList "src/plat/orin-nano/overlay-orin-nano.dts")
    declare_default_headers(
        TIMER_FREQUENCY 31250000
        MAX_IRQ 608
        NUM_PPI 32
        KERNEL_WCET 10u
        TIMER drivers/timer/arm_generic.h
        INTERRUPT_CONTROLLER arch/machine/gic_v3.h
    )
endif()

add_sources(
    DEP "KernelPlatformOrinNano"
    CFILES src/arch/arm/machine/gic_v3.c src/arch/arm/machine/l2c_nop.c
)
