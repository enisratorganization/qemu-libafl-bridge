/*
 * Redfin: XBL/SBL1 hooks (PLLs, PMIC, DDR training, ... have no model)
 */

#include "redfin.h"
#ifdef AS_LIB
#include "exec/coverage.h"
#endif

static bool DALSysGetPropertyValue(CPUState *cs, vaddr pc, void *opaque)
{
    g_autofree char *name = fp_read_str(cs, fp_arg(cs, 1), 64);

    FP_LOG("DALSysGetPropertyValue: %s id %" PRIu64 "\n", name, fp_arg(cs, 2));
    return false;
}

/* DDR info as filled in by the (skipped) DDR driver */
#define DDR_MAX_NUM_CH 8
typedef struct QEMU_PACKED ddr_size_info {
    uint32_t ddr_cs0[DDR_MAX_NUM_CH];       /* size CS0, in MEGABYTES */
    uint32_t ddr_cs1[DDR_MAX_NUM_CH];       /* size CS1 */
    uint64_t ddr_cs0_addr[DDR_MAX_NUM_CH];  /* start address CS0 */
    uint64_t ddr_cs1_addr[DDR_MAX_NUM_CH];  /* start address CS1 */
    uint32_t highest_bank_bit;
    uint32_t pad;
} ddr_size_info;

typedef struct QEMU_PACKED ddr_info {
    ddr_size_info ddr_size;
    uint32_t interleaved_memory;
    uint32_t ddr_type;
} ddr_info;

/* boot_ddr_initialize_device: publish a fixed 4x2 GiB DDR config */
static bool ddr_initialize_info(CPUState *cs, vaddr pc, void *opaque)
{
    static const ddr_info info = {
        .ddr_size = {
            .ddr_cs0 = { 2048, 2048 },
            .ddr_cs1 = { 2048, 2048 },
            .ddr_cs0_addr = { 0x80000000, 0x100000000 },
            .ddr_cs1_addr = { 0x180000000, 0x200000000 },
        },
        .interleaved_memory = 0,
        .ddr_type = 7,
    };

    fp_write(cs, 0x14891978, &info, sizeof(info));
    fp_write(cs, 0x14891A48, &info, sizeof(info) - 8);  /* ddr_system_size */
    fp_write_u8(cs, 0x14891970, 1);                     /* init done */
    return fp_return_void(cs);
}

#ifdef AS_LIB
static bool edge_cov_off(CPUState *cs, vaddr pc, void *opaque)
{
    disable_edge_coverage_single_cpu(cs);
    return false;
}

static bool edge_cov_on(CPUState *cs, vaddr pc, void *opaque)
{
    enable_edge_coverage_single_cpu(cs);
    return false;
}
#endif

static const FpHook sbl1_hooks[] = {
    FP_RET(0x1485D5A8, 1, "a_lot_of_hw_init_sub_1485D5A8"),
    FP_RET(0x148371F8, 0, "some PLL init"),
    FP_RET(0x1469F960, 1, "enable a PLL"),
    /* XBL bug: does not wait for UFS command completion */
    FP_SET_REG(0x14864298, 12, 0, "UFS: clear NON_BLOCKING flag"),
    FP_SET_REG(0x14850E30, 3, 0, "pmic_status = 0"),
    FP_SET_REG(0x14850E8C, 10, 0, "pmic: x10 = 0"),
    FP_SET_REG(0x14850EAC, 0, 0, "pmic: x0 = 0"),
    FP_SET_REG(0x1484ECBC, 0, 0, "pmic_driver_init"),
    FP_RET(0x14850F34, 0, "usb_battery_check"),
    FP_RET(0x14837364, 0, "DDR stuff"),
    FP_RET(0x14837054, 1, "boot_pre_ddr_clock_init"),
    FP_HOOK(0x14848254, DALSysGetPropertyValue, "DALSysGetPropertyValue"),
    FP_RET(0x148243E8, 0, "do_ddr_training"),
    FP_RET(0x148C0000, 0, "boot_ddi_entry"),
    FP_RET(0x148245A8, 0, "sbl1_hw_init_secondary"),
    FP_RET(0x14837368, 0, "boot_populate_cpr_settings (SMEM)"),
    FP_SET_REG(0x14850D28, 0, 0, "pm_init_smem"),
    FP_RET(0x1483706C, 0, "boot_clock_init_rpm"),
    FP_HOOK(0x1482C4B8, ddr_initialize_info, "boot_ddr_initialize_device"),
    FP_RET(0x1482D004, 0, "ddr_post_init"),

#ifdef AS_LIB
    /* exclude UFS polling loops from edge coverage (unstable in LibAFL) */
    FP_HOOK(0x14864170, edge_cov_off, "edge coverage off"),
    FP_HOOK(0x148642CC, edge_cov_on,  "edge coverage on"),
    FP_HOOK(0x148634A0, edge_cov_off, "edge coverage off"),
    FP_HOOK(0x148636C0, edge_cov_on,  "edge coverage on"),
#endif
};

void sbl1_instrument(void)
{
    FP_ADD_HOOKS(sbl1_hooks);
}
