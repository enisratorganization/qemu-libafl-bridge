/**
 * Skeleton of a SysBusDevice that compiles.
 * It allows for convenient and fast prototyping using Copy&Paste, Find&Replace + Coding LLMs
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "qom/object.h"
#include "hw/qdev-properties.h"

#define TYPE_qcom_0x90c0000 "qcom_0x90c0000"
OBJECT_DECLARE_SIMPLE_TYPE(qcom_0x90c0000State, qcom_0x90c0000)

#define NUM_GPIO_OUT 1
#define NUM_GPIO_IN 1

struct qcom_0x90c0000State {
    SysBusDevice parent_obj;
    /* MMIO Regions */
    MemoryRegion mmio1;
    MemoryRegion mmio2;
    MemoryRegion mmio3;
    /* GPIO out (irq) */
    qemu_irq out[NUM_GPIO_OUT];
    /* GPIO in (this is a bitfield of input PIN states)*/
    uint64_t level_in;
    /* Properties */
    char *prop_char;
    uint64_t prop_uint64;
    bool prop_bool;

    /* Put your NOT SAVED members here */

    void* _vmstate_saved_offset;
    /* members below this point are SAVED in the vmstate */

};

static Property qcom_0x90c0000_properties[] = {
    DEFINE_PROP_STRING("prop_char", qcom_0x90c0000State, prop_char),
    DEFINE_PROP_UINT64("prop_uint64", qcom_0x90c0000State, prop_uint64, 0),
    DEFINE_PROP_BOOL("prop_bool", qcom_0x90c0000State, prop_bool, 0),
    DEFINE_PROP_END_OF_LIST(),
};

/**
 * mmio1 definitions
 * If you want more than one MMIO regions: 
 * simply duplicate below _read(), _write() and _ops code , then Find&Replace mmio1 -> mmio2 
 * */
static uint64_t qcom_0x90c0000_mmio1_read (void *opaque, hwaddr addr, unsigned size) {
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) opaque;
    uint64_t ret = 0;

    switch (addr) {
    case 0x10:
        ret = 0xf;
        break;

    default:
        ret = 0;
    }
    qemu_log_mask(LOG_TRACE, "%s: off %"HWADDR_PRIx" sz %u val %"PRIx64"\n", __func__, addr, size, ret);
    return ret;
}
static void qcom_0x90c0000_mmio1_write (void *opaque, hwaddr addr, uint64_t value, unsigned size) {
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) opaque;
    qemu_log_mask(LOG_TRACE, "%s: off %"HWADDR_PRIx" sz %u val %"PRIx64"\n", __func__, addr, size, value);

    switch (addr) {

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}
static const MemoryRegionOps qcom_0x90c0000_ops = {
    .read = qcom_0x90c0000_mmio1_read,
    .write = qcom_0x90c0000_mmio1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/**
 * Handle input pins. Automatically sets the bits to "level_in" of qcom_0x90c0000State
 */
static void qcom_0x90c0000_gpio_set(void *opaque, int line, int level)
{
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) opaque;
    assert(line >= 0 && line < NUM_GPIO_IN);

    if (level)
        s->level_in |= 1 << line;
    else
        s->level_in &= ~(1 << line);
}

/**
 * VMState automatically saves all direct struct members of qcom_0x90c0000State below _vmstate_saved_offset.
 * If you need to save custom objects, just add them before VMSTATE_END_OF_LIST()
 */
static const VMStateDescription vmstate_qcom_0x90c0000 = {
    .name = "qcom_0x90c0000",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        {
        .name = "buf", .version_id = 1,.field_exists = 0,.size = sizeof(qcom_0x90c0000State)-offsetof(qcom_0x90c0000State, _vmstate_saved_offset), 
        .info = &vmstate_info_buffer,.flags=VMS_BUFFER,.offset = offsetof(qcom_0x90c0000State, _vmstate_saved_offset),  
        },
        VMSTATE_END_OF_LIST()
    }
};

static void qcom_0x90c0000_reset(DeviceState *dev)
{
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) dev;
}

static void qcom_0x90c0000_init(Object *obj)
{
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) obj;
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio1, obj, &qcom_0x90c0000_ops, s, "qcom_0x90c0000_mmio1", 0x1000);
    sysbus_init_mmio(sbd, &s->mmio1);

    for (int i = 0; i < NUM_GPIO_OUT; i++) { sysbus_init_irq(sbd, &s->out[i]); }
    qdev_init_gpio_in_named(dev, qcom_0x90c0000_gpio_set, "in", NUM_GPIO_IN);
}

static void qcom_0x90c0000_realize(DeviceState *dev, Error **errp)
{
    qcom_0x90c0000State *s = (qcom_0x90c0000State *) dev;

}

static void qcom_0x90c0000_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_qcom_0x90c0000;
    dc->realize = &qcom_0x90c0000_realize;
    dc->reset = &qcom_0x90c0000_reset;
    device_class_set_props(dc, qcom_0x90c0000_properties);
}

static const TypeInfo qcom_0x90c0000_info = {
    .name          = TYPE_qcom_0x90c0000,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(qcom_0x90c0000State),
    .instance_init = qcom_0x90c0000_init,
    .class_init    = qcom_0x90c0000_class_init,
};

static void qcom_0x90c0000_register_types(void)
{
    type_register_static(&qcom_0x90c0000_info);
}

type_init(qcom_0x90c0000_register_types)
