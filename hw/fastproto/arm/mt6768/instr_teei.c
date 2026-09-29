/*
 * MT6768: TEEI (Microtrust TEE) hooks
 */

#include "mt6768.h"
#include "exec/ramblock.h"
#include "exec/tb-flush.h"
#include "libafl/system.h"
#include "loglevel.inc.h"

#ifdef __OPTIMIZE__
#define REDUCE_LOGGING
#endif

/* Debug output -> UART */
static bool set_UART_flag(CPUState *cs, vaddr pc, void *opaque)
{
    fp_set_arg(cs, 0, fp_arg(cs, 0) | fp_hook_arg(opaque));
    return false;
}

/* ---- Patch the loglevel check in libuTlog.so once it is loaded ---- */
typedef struct FindReplace {
    const uint8_t *find;
    const uint8_t *replace;
    size_t len;
    ram_addr_t ramoff;      /* search window inside each RAM block */
    ram_addr_t ramlimit;
    int replaced;           /* out */
} FindReplace;

static int ram_block_replace(RAMBlock *rb, void *opaque)
{
    FindReplace *fr = opaque;
    ram_addr_t size = qemu_ram_get_used_length(rb);
    uint8_t *host = qemu_ram_get_host_addr(rb);
    uint8_t *hit;

    if (size <= fr->ramoff) {
        return 0;
    }
    size = MIN(size, fr->ramoff + fr->ramlimit);
    hit = host + fr->ramoff;
    while ((hit = memmem(hit, size - (hit - host), fr->find, fr->len))) {
        memcpy(hit, fr->replace, fr->len);
        FP_LOG("loglevel patch @ RAM offset 0x%tx\n", hit - host);
        fr->replaced++;
        hit += fr->len;
    }
    return 0;
}

static bool at_sigma_0_run(CPUState *cs, vaddr pc, void *opaque)
{
    /* libuTlog.so lives in RAM from physaddr 0x70000000 (DRAM + 0x30000000) */
    FindReplace fr = {
        .find = before_loglevel,
        .replace = replace_loglevel_high,
        .len = sizeof(before_loglevel),
        .ramoff = 0x30000000,
        .ramlimit = 0x8000000,
    };
#ifdef REDUCE_LOGGING
    fr.replace = replace_loglevel_none;
#endif
    qemu_ram_foreach_block(ram_block_replace, &fr);
    if (fr.replaced) {
        /* Patched RAM via host pointer: drop stale translations */
        tb_flush(cs);
    }
    return false;
}

/* ---- Debugging helpers (enable in the table below) ---- */

static G_GNUC_UNUSED bool R3_inv(CPUState *cs, vaddr pc, void *opaque)
{
    fp_set_reg(cs, 3, 0xabcdef13);
    return false;
}

/* Halt the vCPU here after 8 hits (true without PC change = spin forever) */
static G_GNUC_UNUSED bool halt_after_8(CPUState *cs, vaddr pc, void *opaque)
{
    static uint64_t hits;
    return ++hits >= 8;
}

#ifdef DUMMY_TIMERS
static G_GNUC_UNUSED bool test_timer_warping(CPUState *cs, vaddr pc,
                                             void *opaque)
{
    libafl_dummy_clock_set_inc(1);
    return false;
}

static bool dummy_clock_tick(CPUState *cs, vaddr pc, void *opaque)
{
    dummy_clock_inc_arm(ARM_CPU(cs));
    return false;
}
#endif

static const FpHook teei_hooks[] = {
    FP_HOOK_ARG(0xFFF007354, set_UART_flag, 0x200, "debug output -> UART"),
    FP_HOOK_ARG(0xFFFFFF80F00144FC, set_UART_flag, 1,
                "debug output -> UART (L4)"),
    FP_HOOK(0xFFFFFF80F00258A8, at_sigma_0_run, "at_sigma_0_run"),

#ifdef DUMMY_TIMERS
    /* FP_HOOK(0x4C409044, test_timer_warping, "timer warp test"), */
    FP_HOOK(0xFFFFFF80F00AC1A0, dummy_clock_tick, "dummy clock tick"),
    FP_HOOK(0xFFFFFF80F00AC1B8, dummy_clock_tick, "dummy clock tick"),
#endif

    /* FP_HOOK(0x1003538, R3_inv, "DEBUG CRASH"), */
    /* FP_HOOK(0x100297C, R3_inv, "DEBUG CRASH"), */
    /* wait at km_start, for debugging inside LibAFL: */
    /* FP_HOOK(0x1116F00, halt_after_8, "km_start"), */
};

void teei_instrument(void)
{
    FP_ADD_HOOKS(teei_hooks);
}
