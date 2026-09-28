/*
 * Hook file skeleton: one file per firmware/boot stage (instr_<stage>.c).
 * Copy next to your machine, `touch` meson.build, declare
 * <stage>_instrument() in your project header and call it from machine init.
 *
 * Hook rules (details: include/libafl/instrument.h):
 *  - hooks run BEFORE the instruction at pc, on the vCPU thread, w/o BQL
 *  - return false: continue at pc (only regs/memory changed)
 *  - return true:  PC was changed (fp_return(), fp_skip_insn(), ...)
 *  - one hook per pc (a second one replaces the first, with a warning)
 *  - pc is a guest virtual address (with MMU on: the VA the code runs at)
 */

#include "hw/fastproto/arm/fp_arm.h"

/* Log arguments of an interesting function, then run it normally */
static bool trace_args(CPUState *cs, vaddr pc, void *opaque)
{
    g_autofree char *str = fp_read_str(cs, fp_arg(cs, 1), 64);

    FP_LOG("%s(0x%" PRIx64 ", \"%s\")\n", ((const FpHook *)opaque)->name,
           fp_arg(cs, 0), str);
    return false;
}

/* Replace a function: fill an output struct, return success */
static bool get_config(CPUState *cs, vaddr pc, void *opaque)
{
    fp_write_u32(cs, fp_arg(cs, 0), 0x1234);    /* *out = 0x1234 */
    return fp_return(cs, 0);
}

/* Patch a global variable, continue */
static bool set_flag(CPUState *cs, vaddr pc, void *opaque)
{
    fp_write_u8(cs, 0x80001000, 1);
    return false;
}

static const FpHook stage_hooks[] = {
    FP_RET(0x80000100, 0, "pmic_init: skip"),
    FP_RET(0x80000200, 1, "is_hw_ready: always true"),
    FP_SET_REG(0x80000300, 3, 0, "status = 0 (x3)"),
    FP_SKIP(0x80000400, "skip MMIO poll insn"),
    FP_TRACE(0x80000500, "reached main loop"),
    FP_HOOK(0x80000600, trace_args, "load_image"),
    FP_HOOK(0x80000700, get_config, "get_config"),
    FP_HOOK(0x80000800, set_flag, "set_flag"),
};

void stage_instrument(void);
void stage_instrument(void)
{
    FP_ADD_HOOKS(stage_hooks);
}
