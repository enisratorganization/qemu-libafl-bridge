/*
 * Redfin: PIMEM "ramblur" (memory encryption) controller. RAM-like registers,
 * except that the status registers of the (at most 3) ranges toggle on every
 * read, so polling for "done" and "idle" both succeed.
 */

#include "hw/fastproto/fastproto.h"

#define TYPE_QCOM_PIMEM_RAMBLUR "qcom_pimem_ramblur"
OBJECT_DECLARE_SIMPLE_TYPE(QcomRamblurState, QCOM_PIMEM_RAMBLUR)

#define RAMBLUR_REGS_SIZE 0x1100

struct QcomRamblurState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    /* ---- saved in the vmstate ---- */
    uint32_t regs[RAMBLUR_REGS_SIZE / 4];
};

static uint64_t ramblur_read(void *opaque, hwaddr addr, unsigned size)
{
    QcomRamblurState *s = opaque;
    uint64_t val = 0;

    if (addr == 0x98 || addr == 0x9c || addr == 0xa0) {
        s->regs[addr / 4] = (s->regs[addr / 4] + 1) % 2;   /* toggle */
    }
    if (addr < RAMBLUR_REGS_SIZE) {
        val = s->regs[addr / 4];
    }
    fp_log_mmio(TYPE_QCOM_PIMEM_RAMBLUR, false, addr, size, val);
    return val;
}

static void ramblur_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    QcomRamblurState *s = opaque;

    fp_log_mmio(TYPE_QCOM_PIMEM_RAMBLUR, true, addr, size, val);
    if (addr < RAMBLUR_REGS_SIZE) {
        s->regs[addr / 4] = val;
    }
}

static const MemoryRegionOps ramblur_ops = {
    .read = ramblur_read,
    .write = ramblur_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_ramblur = {
    .name = TYPE_QCOM_PIMEM_RAMBLUR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, QcomRamblurState, RAMBLUR_REGS_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void ramblur_init(Object *obj)
{
    QcomRamblurState *s = QCOM_PIMEM_RAMBLUR(obj);

    /* The window is larger than the modelled registers: the rest reads 0 */
    memory_region_init_io(&s->mmio, obj, &ramblur_ops, s,
                          TYPE_QCOM_PIMEM_RAMBLUR, 0x8000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

/* Register reset values (0x34: encryption range 0 base/config) */
static void ramblur_reset(DeviceState *dev)
{
    QcomRamblurState *s = QCOM_PIMEM_RAMBLUR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x34 / 4] = 0x80C01000;
}

static void ramblur_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ramblur;
    /* NOT dc->legacy_reset = ...: that would never be called */
    device_class_set_legacy_reset(dc, ramblur_reset);
}

static const TypeInfo ramblur_info = {
    .name          = TYPE_QCOM_PIMEM_RAMBLUR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomRamblurState),
    .instance_init = ramblur_init,
    .class_init    = ramblur_class_init,
};

static void ramblur_register_types(void)
{
    type_register_static(&ramblur_info);
}

type_init(ramblur_register_types)
