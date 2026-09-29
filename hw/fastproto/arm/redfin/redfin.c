/*
 * Redfin (Smartphone Pixel 5) machine: boots the Qualcomm boot ROM (PBL) and
 * the following boot stages from (virtual) UFS, without any real drivers.
 *
 * Run example (real images are not in this repo):
 * /qemu-system-aarch64 -machine redfin -smp maxcpus=8 -bios images/pbl_sdm865.bin \
 *   -drive file=images/bootlun.bin,if=none,id=dr7,readonly=on -device ufs-lu,drive=dr7,bus=ufs-bus,lun=7 \
 *   -drive file=images/sde,if=none,id=dr0,readonly=on -device ufs-lu,drive=dr0,bus=ufs-bus,lun=0 \
 *   -drive file=images/sda,if=none,id=dr1,readonly=on -device ufs-lu,drive=dr1,bus=ufs-bus,lun=1 \
 *   -chardev file,id=qup,path=qup_serial_out.txt
 */

#include "redfin.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char.h"

#define REDFIN_CPU_TYPE     "cortex-a53-arm-cpu"
#define REDFIN_BOOTROM_BASE 0x300000
#define REDFIN_BOOTROM_SIZE 0x100000

/* ---- Memory map ---- */

static const FpMemRegion redfin_memory[] = {
    { "sram",     0x14680000,  0x40000,      FP_RAM },
    { "sram_sec", 0x14800000,  0x100000,     FP_RAM },
    { "pimem",    0x1c000000,  0x04000000,   FP_RAM },
    { "aopmem",   0x0b000000,  0x100000,     FP_RAM },
    { "rom",      REDFIN_BOOTROM_BASE, REDFIN_BOOTROM_SIZE, FP_ROM },
    /* DRAM @0x80000000 is machine->ram (-m) */
};

/* ---- MMIO register stubs (values found by trial & error) ---- */

static void redfin_create_stubs(void)
{
    FP_STUB("qcom_clkdom", 0x17800000 + 0x00541000, 0x2000, FP_STUB_CONST,
            { 0x0, 0x10000000 });
    FP_STUB("qcom_gpll4_mode", 0x177000, 0x1000, FP_STUB_CONST,
            { 0x4, 0xffffffff });
    /* PRNG: polled very often -> quiet */
    FP_STUB("qcom_prng", 0x791000, 0x1000, FP_STUB_RAM | FP_STUB_QUIET,
            { 0x000, 0x12345678, .fixed = true },   /* PRNG_DATA_OUT */
            { 0x004, 1,          .fixed = true },   /* PRNG_CFG_TZ_PRNG_STATUS */
            { 0x140, 1 << 0x19,  .fixed = true });  /* ..._PRNG_KAT_STATUS */
    FP_STUB("qcom_qfprom", 0x780000, 0x10000, FP_STUB_CONST,
            { 0x2058, 0 },
            { 0x6070, 0x40 },           /* boot config */
            { 0x6100, 0 },
            { 0x603C, 1 << 10 },
            { 0x41C0, 0x400000 });
    FP_STUB("qcom_rng", 0x793000, 0x1000, FP_STUB_RAM,
            { 0x000, 1,         .fixed = true },
            { 0x004, 1,         .fixed = true },
            { 0x140, 1 << 0x19, .fixed = true });
    FP_STUB("qcom_smmu", 0x15000000, 0x10000, FP_STUB_CONST,
            { 0x0000, 0x20200001 },
            { 0x40A0, 0xFFFCFFFC });
    FP_STUB("qcom_tcsr_boot_misc_detect", 0x1FD3000, 0x1000, FP_STUB_CONST,
            { 0x000, 0 },               /* boot mode */
            { 0x100, 1 },
            { 0x200, 0x200000 });
    FP_STUB("qcom_tcsr_wonce", 0x1FD4000, 0x1000,
            FP_STUB_RAM | FP_STUB_ANY_SIZE);
    FP_STUB("qcom_tcsr_devconfig", 0x1FC8000, 0x1000, FP_STUB_CONST,
            { 0x000, 0xD << 16 | 6 << 28 },     /* chip family */
            { 0x200, 0x200000 });
    FP_STUB("qcom_tcsr_mutex", 0x1F40000, 0xA000, FP_STUB_RAM);
    FP_STUB("qcom_timer1", 0x17C21000, 0x1000, FP_STUB_CONST,
            { 0x2c, 4 });
    FP_STUB("qcom_ufsphy", 0x1D87000, 0x1000, FP_STUB_CONST,
            { 0xd60, 1 },
            { 0xd80, 1 });
    /* CRYPTO0_CRYPTO_TOP (also @0x1dc1000, 0x1dc4000, 0x1dfa000) */
    FP_STUB("qcom_0x1dc0000", 0x1DC0000, 0x40000, FP_STUB_CONST,
            { 0x00, 0x20000101 },
            { 0x80, 0xfffffffc });
    FP_STUB("qcom_0x90c0000", 0x90c0000, 0x1000, FP_STUB_CONST,
            { 0x10, 0xf });
    FP_STUB("qcom_0x189000", 0x189000, 0x1000, FP_STUB_CONST,
            { 0x4, 0x80000000 });
    FP_STUB("qcom_0x190000", 0x190000, 0x1000, FP_STUB_CONST,
            { 0x4, 0xffffffff });
    FP_STUB("qcom_0xc230000", 0xc230000, 0x1000, FP_STUB_CONST,
            { 0x0c, 0x80000000 },
            { 0x10, 0x80000000 });
    FP_STUB("qcom_qtimer1", 0x17C20000, 0x1000, FP_STUB_CONST,
            { 0x0, 1000000000 },        /* CNTFRQ */
            { 0x4, 8 });
    FP_STUB("qcom_0xc600000", 0xc600000, 0x1000, FP_STUB_CONST);
    /* SPMI (PMIC arbiter) */
    FP_STUB("qcom_spmi_cfg",  0x0c40a000, 0x26000, FP_STUB_CONST, { 0x10, 0 });
    FP_STUB("qcom_spmi_core", 0x0c440000, 0x1100,  FP_STUB_CONST, { 0x10, 0 });
    FP_STUB("qcom_spmi_intr", 0x0e700000, 0xa0000, FP_STUB_CONST, { 0x10, 0 });
}

/* ---- Machine ---- */

static void redfin_cpu_setup(Object *cpu, int index, void *opaque)
{
    /* The firmware expects MPIDR.Aff2 == 0xff */
    uint64_t aff = arm_build_mp_affinity(index, ARM_DEFAULT_CPUS_PER_CLUSTER);

    object_property_set_uint(cpu, "mp-affinity", aff | 0xff0000,
                             &error_fatal);
}

static const int redfin_timer_intids[] = {
    /* from the UEFI config (gArmTokenSpaceGuid.PcdArmArchTimer*IntrNum) */
    [GTIMER_PHYS]    = 18,
    [GTIMER_VIRT]    = 19,
    [GTIMER_HYP]     = 0,      /* "unused" in the UEFI config */
    [GTIMER_SEC]     = 17,
    [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
};

static void redfin_init(MachineState *machine)
{
    DeviceState *dev;
    Chardev *chr;

    fp_create_cpus(machine, redfin_cpu_setup, NULL);

    /* Interrupt controller first (wired to the CPUs) */
    fp_create_gicv3(&(FpGicConfig) {
        .num_spis = 384,
        .dist_base = 0x17a00000,
        .redist_base = 0x17a60000,
        .timer_intids = redfin_timer_intids,
        .legacy_redfin_layout = true,   /* FIXME: see FpGicConfig */
    });

    /*
     * QEMU's UFS host controller (LUNs are added with -device ufs-lu).
     * The boot ROM needs three non-upstream bits (see hw/ufs/ufs.h):
     *  - permissive-uic: it brings the UniPro link up attribute by attribute
     *  - config-desc:    it reads the Configuration Descriptor
     *  - boot-lun=7:     LUN 7 (bootlun.bin) is the boot LU it loads XBL from
     */
    dev = qdev_new("ufs");
    qdev_prop_set_bit(dev, "permissive-uic", true);
    qdev_prop_set_bit(dev, "config-desc", true);
    qdev_prop_set_int32(dev, "boot-lun", 7);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x1d84000);

    redfin_create_stubs();

    /* Devices with real behaviour (see qcom_*.c in this folder) */
    sysbus_create_simple("qcom_mpm2_sleepctr", 0xC221000, NULL);
    sysbus_create_simple("qcom_pimem_ramblur", 0x610000, NULL);

    /* QUP serial engine as UART: needs -chardev ...,id=qup */
    chr = qemu_chr_find("qup");
    if (!chr) {
        error_report("redfin: chardev with id \"qup\" not found "
                     "(add e.g. -chardev file,id=qup,path=serial.txt)");
        exit(1);
    }
    dev = qdev_new("qcom_qup");
    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x888000);

    FP_ADD_MEMORY(redfin_memory);
    memory_region_add_subregion(get_system_memory(), 0x80000000,
                                machine->ram);

    /* Boot ROM image (-bios) */
    if (machine->firmware != NULL) {
        fp_load_firmware(machine->firmware, REDFIN_BOOTROM_BASE,
                         REDFIN_BOOTROM_SIZE);
    }

    /* Primary CPU starts in the boot ROM at EL3 */
    fp_arm_cpu_start(qemu_get_cpu(0), 3, REDFIN_BOOTROM_BASE);

    brom_instrument();
    xbl_sec_instrument();
    sbl1_instrument();
    tz_instrument();
    xbl_uefi_instrument();
}

static void redfin_machine_init(MachineClass *mc)
{
    static const char *const valid_cpu_types[] = {
        REDFIN_CPU_TYPE,
        NULL
    };

    mc->desc = "redfin";
    mc->default_cpu_type = REDFIN_CPU_TYPE;
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 8;
    mc->default_ram_size = 1 * GiB;
    mc->minimum_page_bits = 12;
    mc->init = redfin_init;
    mc->block_default_type = IF_IDE;
    mc->units_per_default_bus = 1;
    /* unmapped accesses read 0 / are ignored instead of faulting */
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_id = "redfin.ram";
}

DEFINE_MACHINE("redfin", redfin_machine_init)
