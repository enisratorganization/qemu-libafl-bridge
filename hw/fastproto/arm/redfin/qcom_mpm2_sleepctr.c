/*
 * Redfin: MPM2 sleep counter. A free running counter that advances on every
 * read (deterministic, independent of host time). Offset 0 = counter.
 */

#include "hw/fastproto/fastproto.h"

#define TYPE_QCOM_MPM2_SLEEPCTR "qcom_mpm2_sleepctr"
OBJECT_DECLARE_SIMPLE_TYPE(QcomSleepCtrState, QCOM_MPM2_SLEEPCTR)

struct QcomSleepCtrState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    /* ---- saved in the vmstate ---- */
    uint32_t ctr;
};

static uint64_t sleepctr_read(void *opaque, hwaddr addr, unsigned size)
{
    QcomSleepCtrState *s = opaque;
    uint64_t val = 0;

    s->ctr++;
    if (addr == 0) {
        /* advances by 2 every other read, i.e. reads twice the same value */
        val = s->ctr % 2 ? s->ctr : s->ctr - 1;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_QCOM_MPM2_SLEEPCTR, addr);
    }
    fp_log_mmio(TYPE_QCOM_MPM2_SLEEPCTR, false, addr, size, val);
    return val;
}

static void sleepctr_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    fp_log_mmio(TYPE_QCOM_MPM2_SLEEPCTR, true, addr, size, val);
}

static const MemoryRegionOps sleepctr_ops = {
    .read = sleepctr_read,
    .write = sleepctr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_sleepctr = {
    .name = TYPE_QCOM_MPM2_SLEEPCTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctr, QcomSleepCtrState),
        VMSTATE_END_OF_LIST()
    }
};

static void sleepctr_init(Object *obj)
{
    QcomSleepCtrState *s = QCOM_MPM2_SLEEPCTR(obj);

    memory_region_init_io(&s->mmio, obj, &sleepctr_ops, s,
                          TYPE_QCOM_MPM2_SLEEPCTR, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void sleepctr_class_init(ObjectClass *klass, void *data)
{
    DEVICE_CLASS(klass)->vmsd = &vmstate_sleepctr;
}

static const TypeInfo sleepctr_info = {
    .name          = TYPE_QCOM_MPM2_SLEEPCTR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomSleepCtrState),
    .instance_init = sleepctr_init,
    .class_init    = sleepctr_class_init,
};

static void sleepctr_register_types(void)
{
    type_register_static(&sleepctr_info);
}

type_init(sleepctr_register_types)
