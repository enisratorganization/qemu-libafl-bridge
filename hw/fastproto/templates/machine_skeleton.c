/*
 * Machine (board) skeleton for an ARM SoC that compiles.
 *
 * How to:
 *  - copy to hw/fastproto/arm/<project>/<project>.c, `touch` meson.build
 *  - Find&Replace machinexyz -> your machine name
 *  - fill in CPU type, memory map, entry point, stubs and hooks
 *  - run: qemu-system-aarch64 -M machinexyz -bios fw.bin -nographic \
 *         -d fastproto,fastproto_mmio,unimp,guest_errors -D fp.log
 */

#include "hw/fastproto/arm/fp_arm.h"
#include "hw/misc/unimp.h"
#include "hw/qdev-properties-system.h"
#include "system/system.h"

#define MACHINEXYZ_CPU_TYPE "cortex-a53-arm-cpu"  /* ARM_CPU_TYPE_NAME(..) */
#define MACHINEXYZ_FW_BASE  0x00000000          /* boot ROM / entry point */
#define MACHINEXYZ_FW_SIZE  0x00100000

/* ---- Memory map: RAM/ROM regions (DRAM = machine->ram, see below) ---- */
static const FpMemRegion machinexyz_memory[] = {
    { "bootrom", MACHINEXYZ_FW_BASE, MACHINEXYZ_FW_SIZE, FP_ROM },
    { "sram",    0x14680000,         0x40000,            FP_RAM },
};

/* ---- Hooks (usually one table per boot stage in instr_<stage>.c) ---- */
static bool my_hook(CPUState *cs, vaddr pc, void *opaque)
{
    FP_LOG("my_hook: x0=0x%" PRIx64 "\n", fp_arg(cs, 0));
    return false;   /* no control flow change: continue at pc */
}

static const FpHook machinexyz_hooks[] = {
    FP_RET(0x1000, 0, "clock_init: skip, return 0"),
    FP_SET_REG(0x2000, 0, 1, "force x0 = 1 (e.g. 'is ready')"),
    FP_TRACE(0x3000, "did we get here?"),
    FP_HOOK(0x4000, my_hook, "my_hook"),
};

static void machinexyz_init(MachineState *machine)
{
    DeviceState *gic, *dev;

    /* CPUs first (CPU 0 runs, the others wait for PSCI CPU_ON) */
    fp_create_cpus(machine, NULL, NULL);

    /* Catch-all for unknown MMIO: logs accesses with -d unimp */
    create_unimplemented_device("mmio_catchall", 0x0, 0x80000000);

    /* Interrupt controller: GPIO input n = SPI n */
    gic = fp_create_gicv3(&(FpGicConfig) {
        .num_spis = 64,
        .dist_base = 0x08000000,
        .redist_base = 0x080a0000,
    });

    /* Register stubs: start empty, add registers from the MMIO log */
    FP_STUB("clk_ctrl", 0x10000000, 0x1000, FP_STUB_CONST,
            { 0x04, 0x1 });                 /* PLL locked */
    FP_STUB("scratch", 0x10001000, 0x1000, FP_STUB_RAM);

    /* A real QEMU device (or one from templates/sysbus_device_skeleton.c):
     * UART on -serial, IRQ = SPI 1. Properties must be set before realize. */
    dev = qdev_new("pl011");
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x09000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(gic, 1));

    FP_ADD_MEMORY(machinexyz_memory);
    memory_region_add_subregion(get_system_memory(), 0x80000000,
                                machine->ram);

    if (machine->firmware) {
        fp_load_firmware(machine->firmware, MACHINEXYZ_FW_BASE,
                         MACHINEXYZ_FW_SIZE);
    }

    fp_arm_cpu_start(qemu_get_cpu(0), 3, MACHINEXYZ_FW_BASE);
    FP_ADD_HOOKS(machinexyz_hooks);
}

static void machinexyz_machine_init(MachineClass *mc)
{
    static const char *const valid_cpu_types[] = {
        MACHINEXYZ_CPU_TYPE,
        NULL
    };

    mc->desc = "machinexyz";
    mc->default_cpu_type = MACHINEXYZ_CPU_TYPE;
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 8;
    mc->default_ram_size = 1 * GiB;
    mc->default_ram_id = "machinexyz.ram";
    mc->init = machinexyz_init;
    /* unmapped accesses read 0 / are ignored instead of faulting */
    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE("machinexyz", machinexyz_machine_init)
