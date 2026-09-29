/*
 * Skeleton of a SysBusDevice with custom behaviour that compiles.
 *
 * For plain "return some constants" registers use FP_STUB()/fp_stub_create()
 * instead (fastproto.h) - no new device needed.
 *
 * How to:
 *  - copy to hw/fastproto/<arch>/<project>/<dev>.c, `touch` meson.build
 *  - Find&Replace devxyz -> your_dev, Devxyz -> YourDev, DEVXYZ -> YOUR_DEV
 *  - fill in the register switch cases
 *
 * Provides: 1 MMIO region, NUM_GPIO_OUT IRQ outputs (sysbus IRQs),
 * NUM_GPIO_IN inputs (named "in", state in level_in), a chardev property
 * and a vmstate that saves every member below `level_in` automatically.
 *
 * Instantiate in the machine:
 *   DeviceState *d = qdev_new("devxyz");
 *   qdev_prop_set_chr(d, "chardev", serial_hd(0));  // props BEFORE realize
 *   sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
 *   sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, 0x10000000);
 *   sysbus_connect_irq(SYS_BUS_DEVICE(d), 0, qdev_get_gpio_in(gic, 42));
 * or for simple cases: sysbus_create_simple("devxyz", 0x10000000, irq);
 */

#include "hw/fastproto/fastproto.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"

#define TYPE_DEVXYZ "devxyz"
OBJECT_DECLARE_SIMPLE_TYPE(DevxyzState, DEVXYZ)

#define DEVXYZ_MMIO_SIZE 0x1000
#define NUM_GPIO_OUT 1
#define NUM_GPIO_IN  1

struct DevxyzState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq out[NUM_GPIO_OUT];

    /* properties */
    CharBackend chr;
    uint64_t prop_u64;

    /* ---- all members below are saved in the vmstate (no pointers!) ---- */
    uint64_t level_in;                      /* GPIO input pin states */
    uint32_t regs[DEVXYZ_MMIO_SIZE / 4];    /* plain register file */
};

static uint64_t devxyz_read(void *opaque, hwaddr addr, unsigned size)
{
    DevxyzState *s = opaque;
    uint64_t val;

    switch (addr) {
    case 0x00:      /* e.g. STATUS: always ready */
        val = 1;
        break;
    default:        /* everything else: behave like RAM */
        val = s->regs[addr / 4];
        break;
    }
    fp_log_mmio(TYPE_DEVXYZ, false, addr, size, val);
    return val;
}

static void devxyz_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    DevxyzState *s = opaque;

    fp_log_mmio(TYPE_DEVXYZ, true, addr, size, val);
    switch (addr) {
    case 0x04: {    /* e.g. TX data register -> chardev */
        uint8_t ch = val;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    }
    case 0x08:      /* e.g. IRQ control: raise/lower output 0 */
        qemu_set_irq(s->out[0], val & 1);
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps devxyz_ops = {
    .read = devxyz_read,
    .write = devxyz_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* GPIO input "in": sets bit @line of level_in */
static void devxyz_gpio_set(void *opaque, int line, int level)
{
    DevxyzState *s = opaque;

    assert(line >= 0 && line < NUM_GPIO_IN);
    if (level) {
        s->level_in |= 1ULL << line;
    } else {
        s->level_in &= ~(1ULL << line);
    }
}

static const VMStateDescription vmstate_devxyz = {
    .name = TYPE_DEVXYZ,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        FP_VMSTATE_TAIL(DevxyzState, level_in),
        VMSTATE_END_OF_LIST()
    }
};

static const Property devxyz_properties[] = {
    DEFINE_PROP_CHR("chardev", DevxyzState, chr),
    DEFINE_PROP_UINT64("prop_u64", DevxyzState, prop_u64, 0),
};

/*
 * System reset: registers to their reset values.
 * Install it with device_class_set_legacy_reset() (see below)
 */
static void devxyz_reset(DeviceState *dev)
{
    DevxyzState *s = DEVXYZ(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void devxyz_init(Object *obj)
{
    DevxyzState *s = DEVXYZ(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &devxyz_ops, s, TYPE_DEVXYZ,
                          DEVXYZ_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    for (int i = 0; i < NUM_GPIO_OUT; i++) {
        sysbus_init_irq(sbd, &s->out[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), devxyz_gpio_set, "in", NUM_GPIO_IN);
}

/* Properties are valid here (not yet in _init) */
static void devxyz_realize(DeviceState *dev, Error **errp)
{
}

static void devxyz_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_devxyz;
    dc->realize = devxyz_realize;
    device_class_set_legacy_reset(dc, devxyz_reset);
    device_class_set_props(dc, devxyz_properties);
}

static const TypeInfo devxyz_info = {
    .name          = TYPE_DEVXYZ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DevxyzState),
    .instance_init = devxyz_init,
    .class_init    = devxyz_class_init,
};

static void devxyz_register_types(void)
{
    type_register_static(&devxyz_info);
}

type_init(devxyz_register_types)
