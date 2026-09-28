/*
 * fastproto: ARM helpers (see hw/fastproto/arm/fp_arm.h)
 */

#include "hw/fastproto/arm/fp_arm.h"
#include "qobject/qlist.h"
#include "target/arm/gtimer.h"
#include "hw/arm/bsa.h"
#include "hw/intc/arm_gicv3_common.h"

/* ---- Instruction skipping ---- */

bool fp_skip_insn(CPUState *cs)
{
    CPUARMState *env = cpu_env(cs);

    if (is_a64(env)) {
        env->pc += 4;
    } else if (env->thumb) {
        /* T32: 32-bit if the first halfword is 0b11101/0b11110/0b11111.. */
        uint8_t b[2] = { 0 };
        fp_read(cs, env->regs[15], b, sizeof(b));
        env->regs[15] += (lduw_le_p(b) >> 11) >= 0x1d ? 4 : 2;
    } else {
        env->regs[15] += 4;
    }
    return true;
}

/* ---- Generic hook callbacks ---- */

bool fp_cb_return(CPUState *cs, vaddr pc, void *opaque)
{
    return fp_return(cs, fp_hook_arg(opaque));
}

bool fp_cb_set_reg(CPUState *cs, vaddr pc, void *opaque)
{
    const FpHook *h = opaque;

    fp_set_reg(cs, h->reg, h->arg);
    return false;
}

bool fp_cb_skip(CPUState *cs, vaddr pc, void *opaque)
{
    return fp_skip_insn(cs);
}

/* ---- CPU start state ---- */

void fp_arm_cpu_start(CPUState *cs, int el, vaddr pc)
{
    cpu_reset(cs);
    arm_emulate_firmware_reset(cs, el);
    cpu_set_pc(cs, pc);
    arm_rebuild_hflags(cpu_env(cs));
}

/* ---- GICv3 ---- */

static const int fp_bsa_timer_intids[] = {
    [GTIMER_PHYS]    = ARCH_TIMER_NS_EL1_IRQ,
    [GTIMER_VIRT]    = ARCH_TIMER_VIRT_IRQ,
    [GTIMER_HYP]     = ARCH_TIMER_NS_EL2_IRQ,
    [GTIMER_SEC]     = ARCH_TIMER_S_EL1_IRQ,
    [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
};

DeviceState *fp_create_gicv3(const FpGicConfig *cfg)
{
    unsigned int smp_cpus = MACHINE(qdev_get_machine())->smp.cpus;
    const int *timer_intids = cfg->timer_intids ?: fp_bsa_timer_intids;
    DeviceState *gic = qdev_new(gicv3_class_name());
    SysBusDevice *gicbusdev = SYS_BUS_DEVICE(gic);
    QList *redist_region_count;
    unsigned num_irq;

    /* num-irq counts SPIs + the 32 internal IRQs (SGIs/PPIs) */
    num_irq = cfg->num_spis + GIC_INTERNAL;
    if (cfg->legacy_redfin_layout) {
        num_irq = cfg->num_spis + smp_cpus * GIC_INTERNAL;
    }

    qdev_prop_set_uint32(gic, "revision", 3);
    qdev_prop_set_uint32(gic, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(gic, "num-irq", num_irq);
    qdev_prop_set_bit(gic, "has-security-extensions", true);

    /* one redistributor region holding all CPUs */
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, smp_cpus);
    qdev_prop_set_array(gic, "redist-region-count", redist_region_count);

    object_property_set_link(OBJECT(gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);
    qdev_prop_set_bit(gic, "has-lpi", true);

    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, cfg->dist_base);
    sysbus_mmio_map(gicbusdev, 1, cfg->redist_base);

    /*
     * GIC GPIO inputs: [0, num_irq - 32) are SPIs, followed by 32 internal
     * lines (indexed by INTID) per CPU. Wire the CPU timer, PMU and GIC
     * maintenance outputs to the PPIs and the GIC outputs to the CPUs.
     */
    for (int i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int intidbase = (num_irq - GIC_INTERNAL) + i * GIC_INTERNAL;

        if (cfg->legacy_redfin_layout) {
            intidbase = cfg->num_spis + i * GIC_INTERNAL;
        }

        for (int t = 0; t < ARRAY_SIZE(fp_bsa_timer_intids); t++) {
            qdev_connect_gpio_out(cpudev, t,
                                  qdev_get_gpio_in(gic, intidbase +
                                                   timer_intids[t]));
        }
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gic, intidbase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic, intidbase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    return gic;
}
