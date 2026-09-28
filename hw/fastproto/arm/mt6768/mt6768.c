/*
 * MT6768 machine: starts ATF (BL31) at EL3 with preloaded TEE + LK images.
 *
 * Needs: -L <dir> with raw images atf, atf_arg_t, mtk_bl_param_t, atags,
 *        tee, lk; -chardev ...,id=uart0; memory backends sram2, config_area:
 *   -object memory-backend-file,id=sram2,size=...,mem-path=...
 *   -object memory-backend-file,id=config_area,size=...,mem-path=...
 */

#include "mt6768.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/unimp.h"
#include "chardev/char.h"

#define MT6768_CPU_TYPE "cortex-a55-arm-cpu"
#define MT6768_ATF_BASE 0x4CE01000

static void mt6768_cpu_setup(Object *cpu, int index, void *opaque)
{
    object_property_set_int(cpu, "cntfrq", 1000000000, &error_fatal);
}

/* Raw images loaded by the (not emulated) preloader */
static const struct {
    const char *file;
    hwaddr addr;
    uint64_t max_size;
} mt6768_images[] = {
    { "atf",            MT6768_ATF_BASE, 0x100000 },
    { "atf_arg_t",      0x4CE00000,      0x100000 },
    { "mtk_bl_param_t", 0x4C080000,      0x100000 },
    { "atags",          0x4C11DA80,      0x100000 },
    { "tee",            0x70000000,      0x400000 },
    { "lk",             0x4c400000,      0x800000 },
};

static void mt6768_init(MachineState *machine)
{
    DeviceState *dev;
    Chardev *chr;
    CPUState *cs;

    fp_create_cpus(machine, mt6768_cpu_setup, NULL);

    /* log all accesses to unknown MMIO (-d unimp) */
    create_unimplemented_device("mmio_catchall", 0x300000,
                                0x100000000 - 0x300000);

    fp_create_gicv3(&(FpGicConfig) {
        .num_spis = 320,
        .dist_base = 0x0c000000,
        .redist_base = 0x0c040000,
    });

    FP_STUB("mtk_mcucfg", 0xC530000, 0x10000, FP_STUB_CONST,
            { 0xA840, 0xC001 });
    FP_STUB("mtk_trng", 0x1020f000, 0x1000, FP_STUB_CONST,
            { 0x0, 0x80000000 },
            { 0x8, 0xaabbccdd });           /* random data :) */

    chr = qemu_chr_find("uart0");
    if (!chr) {
        error_report("mt6768: chardev with id \"uart0\" not found");
        exit(1);
    }
    dev = qdev_new("mtk_uart");
    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x11002000);

    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                machine->ram);
    FP_ADD_MEMORY(((const FpMemRegion[]) {
        { "sram1", 0x100000, 0x100000, FP_RAM },
    }));
    fp_map_memdev("sram2", 0x200000);
    fp_map_memdev("config_area", 0x300000);

    for (int i = 0; i < ARRAY_SIZE(mt6768_images); i++) {
        fp_load_firmware(mt6768_images[i].file, mt6768_images[i].addr,
                         mt6768_images[i].max_size);
    }

    /* BL31 entry: x0 = bl_params, x1 = 0 */
    cs = qemu_get_cpu(0);
    fp_arm_cpu_start(cs, 3, MT6768_ATF_BASE);
    fp_set_reg(cs, 0, 0x4C080000);
    fp_set_reg(cs, 1, 0);

    atf_teei_instrument();
    teei_instrument();
}

static void mt6768_machine_init(MachineClass *mc)
{
    static const char *const valid_cpu_types[] = {
        MT6768_CPU_TYPE,
        NULL
    };

    mc->desc = "mt6768";
    mc->default_cpu_type = MT6768_CPU_TYPE;
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 8;
    mc->default_ram_size = 3 * GiB;
    mc->minimum_page_bits = 12;
    mc->init = mt6768_init;
    mc->block_default_type = IF_IDE;
    mc->units_per_default_bus = 1;
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_id = "mt6768.ram";
}

DEFINE_MACHINE("mt6768", mt6768_machine_init)
