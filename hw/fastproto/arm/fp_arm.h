/*
 * fastproto: ARM (AArch64 + AArch32) helpers.
 * Include as "hw/fastproto/arm/fp_arm.h" (pulls in fastproto.h).
 *
 * Register helpers work on the *current* execution state of the vCPU:
 * AArch64 -> X0..X30, AArch32 -> R0..R15 (AAPCS: args/return in R0..R3).
 */

#pragma once

#include "hw/fastproto/fastproto.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"   /* GTIMER_* */
#include "hw/arm/bsa.h"          /* ARCH_TIMER_*_IRQ INTIDs */

/* ------------------------------------------------------------------------
 * Registers and calling convention (use inside hook callbacks)
 */

static inline uint64_t fp_get_reg(CPUState *cs, int n)
{
    CPUARMState *env = cpu_env(cs);
    return is_a64(env) ? env->xregs[n] : env->regs[n];
}

static inline void fp_set_reg(CPUState *cs, int n, uint64_t val)
{
    CPUARMState *env = cpu_env(cs);
    if (is_a64(env)) {
        env->xregs[n] = val;
    } else {
        env->regs[n] = val;
    }
}

/* Function argument / return value register n (X<n> / R<n>) */
#define fp_arg(cs, n)       fp_get_reg(cs, n)
#define fp_set_arg(cs, n, v) fp_set_reg(cs, n, v)

/*
 * Return from the hooked function without executing it: PC = LR
 * (AArch32: BX LR semantics incl. Thumb bit). Registers are unchanged.
 * Returns true, so a hook can simply do `return fp_return_void(cs);`.
 */
static inline bool fp_return_void(CPUState *cs)
{
    CPUARMState *env = cpu_env(cs);

    if (is_a64(env)) {
        env->pc = env->xregs[30];
    } else {
        uint32_t lr = env->regs[14];
        env->thumb = lr & 1;
        env->regs[15] = lr & ~1u;
    }
    return true;
}

/* Like fp_return_void(), with return value X0/R0 = @ret */
static inline bool fp_return(CPUState *cs, uint64_t ret)
{
    fp_set_reg(cs, 0, ret);
    return fp_return_void(cs);
}

/*
 * Skip the instruction at the current PC (e.g. an MMIO access or a call).
 * Returns true (control flow changed).
 */
bool fp_skip_insn(CPUState *cs);

/* ------------------------------------------------------------------------
 * Generic hook callbacks + FpHook table macros (see fastproto.h)
 */
bool fp_cb_return(CPUState *cs, vaddr pc, void *opaque);  /* ret h->arg */
bool fp_cb_set_reg(CPUState *cs, vaddr pc, void *opaque); /* reg = arg */
bool fp_cb_skip(CPUState *cs, vaddr pc, void *opaque);

/* Return immediately from the function at @pc with value @val */
#define FP_RET(pc_, val_, name_) \
    FP_HOOK_ARG(pc_, fp_cb_return, val_, name_)

/* Set register @reg to @val before executing @pc, then continue */
#define FP_SET_REG(pc_, reg_, val_, name_) \
    { .pc = (pc_), .cb = fp_cb_set_reg, .reg = (reg_), \
      .arg = (uint64_t)(val_), .name = (name_) }

/* Skip the instruction at @pc */
#define FP_SKIP(pc_, name_) FP_HOOK(pc_, fp_cb_skip, name_)

/* ------------------------------------------------------------------------
 * CPU start state
 */

/*
 * Reset @cs and let it start at @pc in the highest EL <= @el, like after a
 * firmware reset (secure state, EL3 if available).
 */
void fp_arm_cpu_start(CPUState *cs, int el, vaddr pc);

/* ------------------------------------------------------------------------
 * GICv3
 */
typedef struct FpGicConfig {
    unsigned num_spis;      /* number of SPIs (external IRQs), mult. of 32 */
    hwaddr dist_base;
    hwaddr redist_base;
    /*
     * INTIDs for the CPU generic timers, indexed by GTIMER_PHYS, _VIRT,
     * _HYP, _SEC, _HYPVIRT. NULL: Arm BSA defaults (see hw/arm/bsa.h).
     */
    const int *timer_intids;
    /*
     * FIXME: Reproduces the historic redfin wiring: num-irq is
     * num_spis + smp*32 and CPU i's PPIs are wired at num_spis + i*32,
     * i.e. for smp > 1 timer/PMU/maintenance IRQs land on SPIs.
     * Only kept to not change the redfin behaviour. Don't use.
     */
    bool legacy_redfin_layout;
} FpGicConfig;

/*
 * Create a GICv3 (security extensions, LPIs), map it and wire it to all
 * CPUs. GPIO input n of the result is SPI n (INTID 32 + n).
 */
DeviceState *fp_create_gicv3(const FpGicConfig *cfg);
