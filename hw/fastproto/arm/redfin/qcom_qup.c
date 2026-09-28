/*
 * Redfin: Qualcomm QUP v3 serial engine, used as TX-only UART.
 * Firmware writes the byte count to 0x270 and the data word to 0x700.
 */

#include "hw/fastproto/fastproto.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"

#define TYPE_QCOM_QUP "qcom_qup"
OBJECT_DECLARE_SIMPLE_TYPE(QcomQupState, QCOM_QUP)

#define SE_HW_PARAM_0       0xe24
#define SE_M_CMD0_TX_LEN    0x270   /* bytes in the next TX word */
#define SE_GENI_TX_FIFO     0x700

struct QcomQupState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    CharBackend chr;

    /* ---- saved in the vmstate ---- */
    int32_t tx_len;
};

static uint64_t qcom_qup_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = 0;

    switch (addr) {
    case SE_HW_PARAM_0:
        val = 1 << 16 | 1 << 24;
        break;
    }
    fp_log_mmio(TYPE_QCOM_QUP, false, addr, size, val);
    return val;
}

static void qcom_qup_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    QcomQupState *s = opaque;

    fp_log_mmio(TYPE_QCOM_QUP, true, addr, size, val);
    switch (addr) {
    case SE_M_CMD0_TX_LEN:
        s->tx_len = val;
        break;
    case SE_GENI_TX_FIFO: {
        /* TX FIFO word: at most 4 bytes, little endian */
        uint8_t buf[4];
        stl_le_p(buf, val);
        qemu_chr_fe_write_all(&s->chr, buf, MIN((unsigned)s->tx_len, 4u));
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_QCOM_QUP, addr);
    }
}

static const MemoryRegionOps qcom_qup_ops = {
    .read = qcom_qup_read,
    .write = qcom_qup_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_qcom_qup = {
    .name = TYPE_QCOM_QUP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        FP_VMSTATE_TAIL(QcomQupState, tx_len),
        VMSTATE_END_OF_LIST()
    }
};

static const Property qcom_qup_properties[] = {
    DEFINE_PROP_CHR("chardev", QcomQupState, chr),
};

static void qcom_qup_init(Object *obj)
{
    QcomQupState *s = QCOM_QUP(obj);

    memory_region_init_io(&s->mmio, obj, &qcom_qup_ops, s, TYPE_QCOM_QUP,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void qcom_qup_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_qcom_qup;
    device_class_set_props(dc, qcom_qup_properties);
}

static const TypeInfo qcom_qup_info = {
    .name          = TYPE_QCOM_QUP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomQupState),
    .instance_init = qcom_qup_init,
    .class_init    = qcom_qup_class_init,
};

static void qcom_qup_register_types(void)
{
    type_register_static(&qcom_qup_info);
}

type_init(qcom_qup_register_types)
