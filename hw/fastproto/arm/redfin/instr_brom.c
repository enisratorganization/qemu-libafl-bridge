/*
 * Redfin: PBL (boot ROM) hooks.
 * Skips HW init that has no model and offloads the SHA engine to the host.
 */

#include "redfin.h"

/* ---- SHA offloading: hash_init(ctx, mode) / update(ctx, desc) / final ---- */

static FpHash brom_hash;

static bool hash_init(CPUState *cs, vaddr pc, void *opaque)
{
    uint64_t mode = fp_arg(cs, 1);

    if (mode != 2 && mode != 3) {
        FP_LOG("hash_init unknown mode %" PRIu64 "\n", mode);
        brom_hash.active = false;
        return false;   /* not emulated: run the original code */
    }
    FP_LOG("hash_init %" PRIu64 "\n", mode);
    fp_hash_init(&brom_hash, mode == 2 ? QCRYPTO_HASH_ALGO_SHA256
                                       : QCRYPTO_HASH_ALGO_SHA384);
    return fp_return(cs, 0);
}

static bool hash_update(CPUState *cs, vaddr pc, void *opaque)
{
    /* X1 -> { u64 src; u64 len; } (physical, MMU is off/identity here) */
    uint64_t src = ldq_le_phys(&address_space_memory, fp_arg(cs, 1));
    uint64_t len = ldq_le_phys(&address_space_memory, fp_arg(cs, 1) + 8);

    FP_LOG("hash_update len %" PRIu64 "\n", len);
    if (!fp_hash_update(&brom_hash, cs, src, len)) {
        return false;
    }
    return fp_return(cs, 0);
}

static bool hash_finish(CPUState *cs, vaddr pc, void *opaque)
{
    /* X1 -> ptr -> digest buffer (double indirection, no mistake) */
    uint64_t dst = ldq_le_phys(&address_space_memory, fp_arg(cs, 1));
    size_t sz;

    dst = ldq_le_phys(&address_space_memory, dst);
    sz = fp_hash_final(&brom_hash, cs, dst);
    if (sz == 0) {
        FP_LOG("hash_finish: not active\n");
        return false;
    }
    FP_LOG("hash_finish len %zu\n", sz);
    return fp_return(cs, 0);
}

static const FpHook brom_hooks[] = {
    FP_RET(0x302A08, 0, "pbl_hw_init"),
    FP_RET(0x30F9E4, 1, "some clk control"),
    FP_SET_REG(0x303660, 17, 0x148fffff, "patch XBL_SEC upper bound"),
    FP_SET_REG(0x302344, 1, 0x1, "fix PMD for secmon"),

    FP_HOOK(0x31DB9C, hash_init,   "hash_init"),
    FP_HOOK(0x31DE0C, hash_update, "hash_update"),
    FP_HOOK(0x31E048, hash_finish, "hash_finish"),
};

void brom_instrument(void)
{
    FP_ADD_HOOKS(brom_hooks);
}
