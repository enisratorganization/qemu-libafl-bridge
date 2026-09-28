/*
 * Redfin: TZ (QSEE, EL3/S-EL1) hooks
 */

#include "redfin.h"

/* ---- ICB (bus config) memory map ---- */

#define MAX_REGIONS  6
#define MAX_CHANNELS 2
typedef struct icb_region {
    uint64_t base_addr;
    uint64_t size;
    uint64_t interleaved;
} icb_region;

typedef struct icb_mem_map {
    struct { icb_region regions[MAX_REGIONS]; } channels[MAX_CHANNELS];
} icb_mem_map;

/* ICB_Get_Memmap(?, icb_mem_map *out): 4x2 GiB DRAM on 2 channels */
static bool ICB_Get_Memmap(CPUState *cs, vaddr pc, void *opaque)
{
    static const icb_mem_map map = {
        .channels = {
            [0].regions = {
                { .base_addr = 0x80000000,  .size = 0x80000000 },
                { .base_addr = 0x100000000, .size = 0x80000000 },
            },
            [1].regions = {
                { .base_addr = 0x180000000, .size = 0x80000000 },
                { .base_addr = 0x200000000, .size = 0x80000000 },
            },
        },
    };

    fp_write(cs, fp_arg(cs, 1), &map, sizeof(map));
    return fp_return(cs, 0);
}

/* get_possible_DRAM_range(u32 *start, u64 *end) */
static bool get_possible_DRAM_range(CPUState *cs, vaddr pc, void *opaque)
{
    FP_LOG("get_possible_DRAM_range: %" PRIx64 " %" PRIx64 "\n",
           fp_arg(cs, 0), fp_arg(cs, 1));
    fp_write_u32(cs, fp_arg(cs, 0), 0x80000000);
    fp_write_u64(cs, fp_arg(cs, 1), 0x300000000);
    return fp_return(cs, 0);
}

/* Set "disable_xpu_ac" to 1, just to be sure, and skip the function */
static bool disable_xpu_ac(CPUState *cs, vaddr pc, void *opaque)
{
    fp_write_u32(cs, 0x887AED30C, 1);
    return fp_return_void(cs);
}

static bool print_smc(CPUState *cs, vaddr pc, void *opaque)
{
    FP_LOG("print_smc: %" PRIx64 "\n", fp_arg(cs, 0));
    return false;
}

/* ---- SHA offloading, 1st implementation: init(mode), update(?, src, len),
 *      final(?, dst) ---- */

static FpHash tz_hash;

static bool tzbsp_hash_init(CPUState *cs, vaddr pc, void *opaque)
{
    uint64_t mode = fp_arg(cs, 0);

    if (mode != 3 && mode != 4) {
        FP_LOG("%s unknown mode %" PRIu64 "\n", __func__, mode);
        tz_hash.active = false;
        return false;   /* not emulated: run the original code */
    }
    FP_LOG("%s %" PRIu64 "\n", __func__, mode);
    fp_hash_init(&tz_hash, mode == 3 ? QCRYPTO_HASH_ALGO_SHA256
                                     : QCRYPTO_HASH_ALGO_SHA384);
    return fp_return(cs, 0);
}

/* ---- 2nd implementation: init(?, mode), same update/final args ---- */

static FpHash tz_hash2;

static bool tzbsp2_hash_init(CPUState *cs, vaddr pc, void *opaque)
{
    uint64_t mode = fp_arg(cs, 1);

    if (mode != 3) {
        FP_LOG("%s unknown mode %" PRIu64 "\n", __func__, mode);
        tz_hash2.active = false;
        return false;
    }
    FP_LOG("%s %" PRIu64 "\n", __func__, mode);
    fp_hash_init(&tz_hash2, QCRYPTO_HASH_ALGO_SHA384);
    return fp_return(cs, 0);
}

/* update(?, src, len) and final(?, dst) for both; opaque->arg selects ctx */
static bool tz_hash_update(CPUState *cs, vaddr pc, void *opaque)
{
    FpHash *h = fp_hook_arg(opaque) ? &tz_hash2 : &tz_hash;

    FP_LOG("hash_update len %" PRIu64 "\n", fp_arg(cs, 2));
    if (!fp_hash_update(h, cs, fp_arg(cs, 1), fp_arg(cs, 2))) {
        return false;
    }
    return fp_return(cs, 0);
}

static bool tz_hash_final(CPUState *cs, vaddr pc, void *opaque)
{
    FpHash *h = fp_hook_arg(opaque) ? &tz_hash2 : &tz_hash;
    size_t sz = fp_hash_final(h, cs, fp_arg(cs, 1));

    if (sz == 0) {
        FP_LOG("hash_final: not active\n");
        return false;
    }
    FP_LOG("hash_final len %zu\n", sz);
    return fp_return(cs, 0);
}

static const FpHook tz_hooks[] = {
    FP_HOOK(0x887A395C8, ICB_Get_Memmap, "ICB_Get_Memmap"),
    FP_HOOK(0x8879DDD54, get_possible_DRAM_range, "get_possible_DRAM_range"),

    FP_RET(0x887A249A8, 0, "rpmh_register_isr"),
    FP_RET(0x887A2432C, 0, "some internal rpmh stuff"),
    FP_RET(0x887A240A4, 0, "rpmh_churn_all"),
    FP_RET(0x887A24168, 0, "rpmh_churn_single"),

    FP_RET(0x887A200A8, 0, "PDC"),
    FP_RET(0x887A290EC, 0, "VPP"),
    FP_RET(0x887A33E10, 0, "unknown_887A33E10"),
    FP_RET(0x887A44264, 0, "VMIDMT stuff (XPU, SMMU)"),
    FP_RET(0x887A55BDC, 0, "SMMU debug config"),
    FP_RET(0x887A09780, 0, "Qcom IPA (integrated HW IP switch)"),
    FP_RET(0x8879CB46C, 0, "unknown_8879CB46C"),
    FP_RET(0x887A13330, 0, "unknown_887A13330"),

    FP_RET(0x8879E3C54, 1, "XPU AC (Access Control)"),
    FP_RET(0x8879E3D70, 1, "XPU AC 2"),
    FP_HOOK(0x887A3CD5C, disable_xpu_ac, "disable_xpu_ac"),

    FP_HOOK(0x887A884CC, tzbsp_hash_init, "tzbsp_hash_init"),
    FP_HOOK_ARG(0x887A88554, tz_hash_update, 0, "tzbsp_hash_update"),
    FP_HOOK_ARG(0x887A88624, tz_hash_final, 0, "tzbsp_hash_final"),

    FP_HOOK(0x887A509BC, tzbsp2_hash_init, "tzbsp2_hash_init"),
    FP_HOOK_ARG(0x887A50AD4, tz_hash_update, 1, "tzbsp2_hash_update"),
    FP_HOOK_ARG(0x887A50B8C, tz_hash_final, 1, "tzbsp2_hash_final"),
    FP_RET(0x887A50A60, 0, "hash_free"),

    FP_RET(0x887A4D940, 0x2ECB770, "verify certchain"),

    FP_RET(0x8879D150C, 0, "tz_get_loglevel (TZ EL1)"),
    FP_RET(0x887A7A718, 1, "is_Anti_rollback_enabled"),
    FP_RET(0x887A3DC3C, 0, "some AC functionality"),

    FP_HOOK(0x14680000, hook_qsee_start, "qsee start"),
    FP_HOOK(0x887A99790, print_smc, "print_smc"),
};

void tz_instrument(void)
{
    FP_ADD_HOOKS(tz_hooks);
}
