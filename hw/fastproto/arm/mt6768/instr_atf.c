/*
 * MT6768: ATF (BL31, EL3) hooks
 */

#include "mt6768.h"

/* SPSR for the next stage: AArch64 EL1t instead of the original mode */
#define SPSR_EL1T_A64 0b111000100

/* console handlers (incl. putchar) = 0: all output goes via the UART */
static bool set_console(CPUState *cs, vaddr pc, void *opaque)
{
    fp_set_arg(cs, 0, 0);
    fp_set_arg(cs, 1, 0);
    fp_set_arg(cs, 2, 0);
    return false;
}

static const FpHook atf_hooks[] = {
    FP_SET_REG(0x4CE030A4, 0, SPSR_EL1T_A64, "EL3 -> LK: set SPSR"),
    FP_SET_REG(0x4CE0B370, 0, SPSR_EL1T_A64, "EL3 -> KERNEL: set SPSR"),
    FP_HOOK(0x4CE190F0, set_console, "set_console"),
    /* ATF wants to disable UART_BASE, we do not allow it */
    FP_SKIP(0x4CE18B84, "keep UART enabled"),
};

void atf_teei_instrument(void)
{
    FP_ADD_HOOKS(atf_hooks);
}
