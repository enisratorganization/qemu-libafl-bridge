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
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"

#define TYPE_qcom_tcsr_mutex "qcom_tcsr_mutex"
OBJECT_DECLARE_SIMPLE_TYPE(qcom_tcsr_mutexState, qcom_tcsr_mutex)

#define NUM_GPIO_OUT 1
#define NUM_GPIO_IN 1

struct qcom_tcsr_mutexState {
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
    CharBackend prop_chr;
    char *prop_str;
    uint64_t prop_uint64;
    bool prop_bool;

    /* Put your NOT SAVED members here */

    void* _vmstate_saved_offset;
    /* members below this point are SAVED in the vmstate */
    uint32_t state[(0x1000*10) >>2];    //@TODO: Optimize size
};

static Property qcom_tcsr_mutex_properties[] = {
    DEFINE_PROP_CHR("prop_chr", qcom_tcsr_mutexState, prop_chr),
    DEFINE_PROP_STRING("prop_str", qcom_tcsr_mutexState, prop_str),
    DEFINE_PROP_UINT64("prop_uint64", qcom_tcsr_mutexState, prop_uint64, 0),
    DEFINE_PROP_BOOL("prop_bool", qcom_tcsr_mutexState, prop_bool, 0),
    DEFINE_PROP_END_OF_LIST(),
};

/**
 * mmio1 definitions
 * If you want more than one MMIO region: 
 * simply duplicate below _read(), _write() and _ops code , then Find&Replace mmio1 -> mmio2 
 * and add memory_region_init_io(...) below
 * */
static uint64_t qcom_tcsr_mutex_mmio1_read (void *opaque, hwaddr addr, unsigned size) {
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) opaque;
    uint64_t ret = 0;

    switch (addr) {

    default:
        ret = s->state[(addr >> 2)];
    }

    qemu_log_mask(LOG_TRACE, "%s: off %"HWADDR_PRIx" sz %u val %"PRIx64"\n", __func__, addr, size, ret);
    return ret;
}
static void qcom_tcsr_mutex_mmio1_write (void *opaque, hwaddr addr, uint64_t value, unsigned size) {
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) opaque;
    qemu_log_mask(LOG_TRACE, "%s: off %"HWADDR_PRIx" sz %u val %"PRIx64"\n", __func__, addr, size, value);

    switch (addr) {

    default:
        s->state[(addr >> 2)] = value;
    }
}
static const MemoryRegionOps qcom_tcsr_mutex_ops = {
    .read = qcom_tcsr_mutex_mmio1_read,
    .write = qcom_tcsr_mutex_mmio1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/**
 * Handle input pins. Automatically sets the bits to "level_in" of qcom_tcsr_mutexState
 */
static void qcom_tcsr_mutex_gpio_set(void *opaque, int line, int level)
{
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) opaque;
    assert(line >= 0 && line < NUM_GPIO_IN);

    if (level)
        s->level_in |= 1 << line;
    else
        s->level_in &= ~(1 << line);
}

/**
 * VMState automatically saves all direct struct members of qcom_tcsr_mutexState below _vmstate_saved_offset.
 * If you need to save custom objects, just add them before VMSTATE_END_OF_LIST()
 */
static const VMStateDescription vmstate_qcom_tcsr_mutex = {
    .name = "qcom_tcsr_mutex",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        {
        .name = "buf", .version_id = 1,.field_exists = 0,.size = sizeof(qcom_tcsr_mutexState)-offsetof(qcom_tcsr_mutexState, _vmstate_saved_offset), 
        .info = &vmstate_info_buffer,.flags=VMS_BUFFER,.offset = offsetof(qcom_tcsr_mutexState, _vmstate_saved_offset),  
        },
        VMSTATE_END_OF_LIST()
    }
};

static void qcom_tcsr_mutex_reset(DeviceState *dev)
{
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) dev;
}

static void qcom_tcsr_mutex_init(Object *obj)
{
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) obj;
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio1, obj, &qcom_tcsr_mutex_ops, s, "qcom_tcsr_mutex_mmio1", 0x1000*10);
    sysbus_init_mmio(sbd, &s->mmio1);

    for (int i = 0; i < NUM_GPIO_OUT; i++) { sysbus_init_irq(sbd, &s->out[i]); }
    qdev_init_gpio_in_named(dev, qcom_tcsr_mutex_gpio_set, "in", NUM_GPIO_IN);
}

static void qcom_tcsr_mutex_realize(DeviceState *dev, Error **errp)
{
    qcom_tcsr_mutexState *s = (qcom_tcsr_mutexState *) dev;

}

static void qcom_tcsr_mutex_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_qcom_tcsr_mutex;
    dc->realize = &qcom_tcsr_mutex_realize;
    dc->reset = &qcom_tcsr_mutex_reset;
    device_class_set_props(dc, qcom_tcsr_mutex_properties);
}

static const TypeInfo qcom_tcsr_mutex_info = {
    .name          = TYPE_qcom_tcsr_mutex,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(qcom_tcsr_mutexState),
    .instance_init = qcom_tcsr_mutex_init,
    .class_init    = qcom_tcsr_mutex_class_init,
};

static void qcom_tcsr_mutex_register_types(void)
{
    type_register_static(&qcom_tcsr_mutex_info);
}

type_init(qcom_tcsr_mutex_register_types)
